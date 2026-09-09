#include "dlna/dlna_stream.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

#include "audio/audio_output.h"
#include "dlna/dlna_renderer.h"

static const char *TAG = "dlna_stream";

/* ── M2 scope ──────────────────────────────────────────────────────────────
 * Transport is WAV/PCM passthrough first: it validates the whole chain
 * (HTTP pull -> shared USB pipeline -> speaker) with zero codec deps.
 * MP3/AAC/FLAC software decoding lands as the next step; unknown formats
 * are detected, logged and the stream stops cleanly.
 */

#define STREAM_TASK_STACK 8192
#define STREAM_TASK_PRIO 8
#define HTTP_BUF_SIZE 4096
#define PCM_CHUNK_FRAMES 1024 /* ~23 ms at 44.1 kHz */
#define HEADER_SNIFF 64

typedef enum {
  CMD_PLAY = 0,
  CMD_PAUSE,
  CMD_RESUME,
  CMD_STOP,
  CMD_SEEK,
} stream_cmd_t;

typedef struct {
  stream_cmd_t cmd;
  double arg;      /* SEEK seconds / PLAY rate hint */
  char uri[DLNA_URI_MAX];
} stream_msg_t;

static QueueHandle_t s_cmd_q = NULL;
static TaskHandle_t s_task = NULL;

static volatile bool s_active = false;   /* task is running a stream */
static volatile bool s_paused = false;   /* task is paused (HTTP idle) */
static char s_uri[DLNA_URI_MAX] = {0};
static uint32_t s_rate = 44100;
static uint32_t s_channels = 2;
static uint32_t s_bits = 16;

/* Decoded-PCM accounting for position/duration (WAV mode). */
static volatile uint64_t s_pcm_frames_played = 0;
static volatile uint64_t s_pcm_frames_total = 0;
static volatile double s_duration = 0.0;
static double s_seek_target = 0.0; /* WAV: drop frames until this time */

static int16_t *s_pcm_buf = NULL;

/* ── format sniffing ─────────────────────────────────────────────────────── */

typedef enum {
  FMT_WAV = 0,
  FMT_MP3,
  FMT_AAC,
  FMT_FLAC,
  FMT_UNKNOWN,
} stream_format_t;

static stream_format_t sniff_format(const uint8_t *h, size_t n) {
  if (n >= 12 && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0) {
    return FMT_WAV;
  }
  if (n >= 4 && memcmp(h, "fLaC", 4) == 0) {
    return FMT_FLAC;
  }
  if (n >= 4 && memcmp(h, "ID3", 3) == 0) {
    return FMT_MP3;
  }
  if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0) {
    return FMT_MP3;
  }
  if (n >= 8 && memcmp(h + 4, "ftyp", 4) == 0) {
    return FMT_AAC;
  }
  return FMT_UNKNOWN;
}

/* ── WAV parsing ─────────────────────────────────────────────────────────── */

typedef struct {
  bool have_fmt;
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  uint64_t data_remaining; /* bytes of PCM still expected (0 = unknown) */
  uint64_t data_pos;       /* PCM bytes consumed so far */
} wav_ctx_t;

/* Parse RIFF chunks from a byte stream. Returns:
 *   >0 : PCM bytes appended to out/out_cap (via feed)
 *    0 : need more bytes (buffer full -> advance anyway)
 *   -1 : fatal error
 * Feed callback keeps chunk state between calls. */
static int wav_consume(wav_ctx_t *ctx, const uint8_t *buf, size_t len,
                       int16_t *pcm, size_t pcm_cap, size_t *pcm_frames) {
  static uint8_t chunk[8];        /* current chunk header */
  static size_t chunk_off = 0;
  static uint32_t chunk_len = 0;  /* remaining bytes in current chunk */
  static bool in_data = false;
  static bool skip = false;       /* inside a non-PCM chunk */

  size_t i = 0;
  *pcm_frames = 0;

  while (i < len) {
    if (!in_data && !skip && chunk_off < 8) {
      /* read a chunk header: "XXXX" + u32 LE */
      size_t need = 8 - chunk_off;
      size_t take = len - i < need ? len - i : need;
      memcpy(chunk + chunk_off, buf + i, take);
      chunk_off += take;
      i += take;
      if (chunk_off == 8) {
        uint32_t id = chunk[0] | (chunk[1] << 8) | (chunk[2] << 16) |
                      ((uint32_t)chunk[3] << 24);
        chunk_len = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) |
                    ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        chunk_off = 0;
        if (id == 0x20746D66) { /* 'fmt ' */
          /* fmt comes before data; cache rate/ch/bits from the stream. */
          in_data = false;
          skip = true;
          ctx->have_fmt = true;
        } else if (id == 0x61746164) { /* 'data' */
          in_data = true;
          ctx->data_remaining = chunk_len;
          if (!ctx->have_fmt) {
            ESP_LOGW(TAG, "WAV data chunk before fmt; assuming 44.1k/16/2");
            ctx->rate = 44100;
            ctx->channels = 2;
            ctx->bits = 16;
          }
        } else {
          in_data = false;
          skip = true;
        }
      }
      continue;
    }

    if (in_data) {
      size_t avail = chunk_len;
      if (avail > len - i) {
        avail = len - i;
      }
      if (avail == 0) {
        in_data = false;
        skip = true; /* next header */
        continue;
      }
      /* Convert bytes to int16 stereo frames on the fly. */
      const uint8_t *p = buf + i;
      size_t frames_in = avail / (ctx->channels * (ctx->bits / 8));
      size_t cap = pcm_cap / 2;
      if (frames_in > cap) {
        frames_in = cap;
      }
      if (frames_in > 0) {
        if (ctx->bits == 16) {
          memcpy(pcm, p, frames_in * ctx->channels * 2);
        } else if (ctx->bits == 24) {
          /* 24-bit LE -> shift right into int16 */
          for (size_t f = 0; f < frames_in * ctx->channels; f++) {
            int32_t v = (int32_t)p[f * 3] | ((int32_t)p[f * 3 + 1] << 8) |
                        ((int32_t)p[f * 3 + 2] << 16);
            pcm[f] = (int16_t)(v >> 8);
          }
        } else {
          return -1; /* unsupported bit depth */
        }
        *pcm_frames = frames_in;
        ctx->data_pos += frames_in * ctx->channels * (ctx->bits / 8);
        chunk_len -= frames_in * ctx->channels * (ctx->bits / 8);
        i += frames_in * ctx->channels * (ctx->bits / 8);
        if (chunk_len == 0) {
          in_data = false;
          skip = true;
        }
        return 1; /* caller feeds one PCM chunk */
      }
      /* frame-aligned but zero-capacity: force progress */
      i = len;
      return 1;
    }

    if (skip) {
      size_t skip_n = chunk_len;
      if (skip_n > len - i) {
        skip_n = len - i;
      }
      i += skip_n;
      chunk_len -= skip_n;
      if (chunk_len == 0) {
        skip = false;
      }
    }
  }
  return 0;
}

/* ── HTTP stream task ────────────────────────────────────────────────────── */

static void feed_pcm(int16_t *pcm, size_t frames) {
  /* Seek: drop frames until the target position. */
  if (s_seek_target > 0.0) {
    double frame_time = 1.0 / (double)s_rate;
    uint64_t drop = (uint64_t)(s_seek_target / frame_time);
    if (drop > 0) {
      uint64_t have = s_pcm_frames_played;
      if (have < drop) {
        size_t d = (size_t)(drop - have);
        if (d > frames) {
          d = frames;
        }
        s_pcm_frames_played += d;
        frames -= d;
        pcm += d * s_channels;
      }
      if (frames == 0) {
        return;
      }
    }
    s_seek_target = 0.0;
  }
  audio_output_usb_host_feed_pcm(pcm, frames, s_rate);
  s_pcm_frames_played += frames;
}

static void stream_task(void *arg) {
  esp_http_client_handle_t client = NULL;
  wav_ctx_t wav;
  stream_msg_t msg;
  bool in_stream = false;
  stream_format_t fmt = FMT_UNKNOWN;

  memset(&wav, 0, sizeof(wav));
  s_pcm_buf = malloc(PCM_CHUNK_FRAMES * 2 * sizeof(int16_t));
  if (!s_pcm_buf) {
    ESP_LOGE(TAG, "pcm buf alloc failed");
    s_active = false;
    vTaskDelete(NULL);
    return;
  }

  while (xQueueReceive(s_cmd_q, &msg, portMAX_DELAY)) {
    switch (msg.cmd) {
    case CMD_PLAY: {
      if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
      }
      snprintf(s_uri, sizeof(s_uri), "%s", msg.uri);
      s_rate = msg.arg > 0 ? (uint32_t)msg.arg : 44100;
      s_channels = 2;
      s_bits = 16;
      s_pcm_frames_played = 0;
      s_pcm_frames_total = 0;
      s_duration = 0.0;
      s_seek_target = 0.0;
      memset(&wav, 0, sizeof(wav));
      fmt = FMT_UNKNOWN;
      in_stream = false;
      s_paused = false;

      esp_http_client_config_t cfg = {
          .url = s_uri,
          .timeout_ms = 15000,
          .buffer_size = HTTP_BUF_SIZE,
          .disable_auto_redirect = false,
      };
      client = esp_http_client_init(&cfg);
      if (!client) {
        ESP_LOGE(TAG, "http client init failed: %s", s_uri);
        s_active = false;
        continue;
      }
      esp_err_t err = esp_http_client_open(client, 0);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed (%s): %s", esp_err_to_name(err),
                 s_uri);
        esp_http_client_cleanup(client);
        client = NULL;
        s_active = false;
        continue;
      }
      int status = esp_http_client_fetch_headers(client);
      ESP_LOGI(TAG, "HTTP %d, content-length=%d", status,
               esp_http_client_get_content_length(client));

      /* Sniff the first bytes for format detection. */
      uint8_t sniff[HEADER_SNIFF];
      int got = esp_http_client_read(client, (char *)sniff, sizeof(sniff));
      if (got <= 0) {
        ESP_LOGW(TAG, "no data from stream");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
        s_active = false;
        continue;
      }
      fmt = sniff_format(sniff, (size_t)got);
      ESP_LOGI(TAG, "stream format: %s",
               fmt == FMT_WAV ? "WAV" : fmt == FMT_MP3 ? "MP3"
               : fmt == FMT_AAC ? "AAC" : fmt == FMT_FLAC ? "FLAC"
                                                          : "UNKNOWN");
      if (fmt != FMT_WAV) {
        ESP_LOGE(TAG, "format not supported yet (M2 stage 1 = WAV only)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
        s_active = false;
        continue;
      }

      /* First chunk into the WAV parser. */
      size_t frames = 0;
      int r = wav_consume(&wav, sniff, (size_t)got, s_pcm_buf,
                          PCM_CHUNK_FRAMES * 2, &frames);
      if (r > 0 && frames > 0) {
        s_rate = wav.rate;
        s_channels = wav.channels;
        s_bits = wav.bits;
        feed_pcm(s_pcm_buf, frames);
      }
      s_active = true;
      in_stream = true;
      ESP_LOGI(TAG, "DLNA stream playing: %d Hz %d-bit %dch", s_rate, s_bits,
               s_channels);
      break;
    }

    case CMD_PAUSE:
      if (in_stream) {
        s_paused = true;
        ESP_LOGI(TAG, "DLNA stream paused");
      }
      break;

    case CMD_RESUME:
      if (in_stream) {
        s_paused = false;
        ESP_LOGI(TAG, "DLNA stream resumed");
      }
      break;

    case CMD_STOP:
      if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
      }
      in_stream = false;
      s_active = false;
      s_paused = false;
      s_duration = 0.0;
      ESP_LOGI(TAG, "DLNA stream stopped");
      break;

    case CMD_SEEK:
      if (msg.arg < 0) {
        msg.arg = 0;
      }
      s_seek_target = msg.arg;
      ESP_LOGI(TAG, "DLNA stream seek to %.1fs", msg.arg);
      break;
    }

    /* Streaming loop (runs until the stream ends or a command arrives). */
    while (in_stream && !s_paused && client) {
      if (uxQueueMessagesWaiting(s_cmd_q) > 0) {
        break; /* handle pending command */
      }
      uint8_t buf[HTTP_BUF_SIZE];
      int n = esp_http_client_read(client, (char *)buf, sizeof(buf));
      if (n <= 0) {
        ESP_LOGI(TAG, "stream ended (read=%d)", n);
        break;
      }
      size_t frames = 0;
      int r = wav_consume(&wav, buf, (size_t)n, s_pcm_buf,
                          PCM_CHUNK_FRAMES * 2, &frames);
      if (r < 0) {
        ESP_LOGE(TAG, "WAV parse error");
        break;
      }
      if (r > 0 && frames > 0) {
        feed_pcm(s_pcm_buf, frames);
      }
    }

    if (!in_stream && client) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      client = NULL;
    }
  }

  free(s_pcm_buf);
  s_pcm_buf = NULL;
  s_active = false;
  vTaskDelete(NULL);
}

/* ── public API ──────────────────────────────────────────────────────────── */

esp_err_t dlna_stream_init(void) {
  if (s_cmd_q) {
    return ESP_OK;
  }
  s_cmd_q = xQueueCreate(8, sizeof(stream_msg_t));
  if (!s_cmd_q) {
    return ESP_ERR_NO_MEM;
  }
  BaseType_t ok = xTaskCreatePinnedToCore(stream_task, "dlna_stream",
                                          STREAM_TASK_STACK, NULL,
                                          STREAM_TASK_PRIO, &s_task, 0);
  if (ok != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "DLNA stream task started");
  return ESP_OK;
}

esp_err_t dlna_stream_play(const char *uri) {
  if (!uri || !uri[0]) {
    return ESP_ERR_INVALID_ARG;
  }
  stream_msg_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.cmd = CMD_PLAY;
  snprintf(msg.uri, sizeof(msg.uri), "%s", uri);
  if (xQueueSend(s_cmd_q, &msg, pdMS_TO_TICKS(200)) != pdPASS) {
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

void dlna_stream_pause(void) {
  stream_msg_t msg = {.cmd = CMD_PAUSE};
  xQueueSend(s_cmd_q, &msg, pdMS_TO_TICKS(100));
}

void dlna_stream_resume(void) {
  stream_msg_t msg = {.cmd = CMD_RESUME};
  xQueueSend(s_cmd_q, &msg, pdMS_TO_TICKS(100));
}

void dlna_stream_stop(void) {
  stream_msg_t msg = {.cmd = CMD_STOP};
  xQueueSend(s_cmd_q, &msg, pdMS_TO_TICKS(100));
}

void dlna_stream_seek(double seconds) {
  stream_msg_t msg = {.cmd = CMD_SEEK, .arg = seconds};
  xQueueSend(s_cmd_q, &msg, pdMS_TO_TICKS(100));
}

double dlna_stream_get_position(void) {
  if (!s_active) {
    return 0.0;
  }
  double bytes_per_sec =
      (double)s_rate * s_channels * (double)(s_bits / 8);
  if (bytes_per_sec <= 0) {
    return 0.0;
  }
  return (double)s_pcm_frames_played * (double)s_channels *
         (double)(s_bits / 8) / bytes_per_sec;
}

double dlna_stream_get_duration(void) {
  return s_duration;
}

bool dlna_stream_is_playing(void) {
  return s_active && !s_paused;
}
