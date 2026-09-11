#include "dlna/dlna_stream.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "audio/audio_output.h"
#include "dlna/dlna_renderer.h"
/* dr_flac: single-file FLAC decoder (public domain). Implementation is
 * compiled into this TU (a separate .c was not picked up by the build). */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#include "minimp3.h"
#include "codecs/libhelix-aac/aacdec.h"
#include "codecs/libfaad2/include/neaacdec.h"
#define STB_VORBIS_HEADER_ONLY
#include "codecs/stb_vorbis.c"

static const char *TAG = "dlna_stream";

/* TEMP DIAG: crash checkpoint. Survives a software restart (RTC fast mem),
 * written by the stream task at each stage; read back after a panic-reboot
 * via /api/audio/usb to learn which stage crashed. 0 = no stream started. */
RTC_NOINIT_ATTR uint32_t g_crash_stage = 0;
void dlna_cp(uint32_t s) { g_crash_stage = s; }
uint32_t dlna_stream_get_crash_stage(void) { return g_crash_stage; }
/* TEMP DIAG: report compile-time decoder struct size */
uint32_t dlna_stream_get_dec_size(void) { return (uint32_t)sizeof(mp3dec_t); }
uint32_t dlna_stream_get_scratch_size(void) {
  /* scratch typedef is implementation-scoped in this build; report the
   * decode_frame stack footprint we configured around (BSS scratch). */
  return 0;
}

/* 鈹€鈹€ M2 scope 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
 * Transport: WAV/PCM passthrough + MP3 software decode (minimp3, single-file
 * public-domain decoder). FLAC/AAC land next; unknown formats are detected,
 * logged and the stream stops cleanly.
 */

/* minimp3's mp3dec_decode_frame puts a ~16 KB mp3dec_scratch_t on the
 * task stack (maindata 2.8 KB + grbuf 4.6 KB + syn filterbank 8.4 KB), plus
 * the decode call chain. 8 KB overflowed silently and corrupted the decode
 * state for 48 kHz / 320 kbps streams (no PCM frames produced). 32 KB gives
 * ample headroom; the task is lazy-created at play time and its stack lives
 * in PSRAM (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y) 鈥?a 32 KB internal
 * stack repeatedly failed to allocate once httpd/USB/FIFO were up.
 * 32 KB of PSRAM stack was enough for decode (scratch is now BSS) but the
 * HTTP read chain (esp_http_client_read -> transport -> lwIP recv) needs
 * headroom; 48 KB with the read buffer on the PSRAM heap keeps it safe. */
#define STREAM_TASK_STACK 98304
/* Below httpd (prio 5) so the SOAP Play handler finishes sending its response
 * before the (CPU-heavy, PSRAM-stack) stream task starts pulling HTTP. */
#define STREAM_TASK_PRIO 3
#define HTTP_BUF_SIZE 4096
#define PCM_CHUNK_FRAMES 1024 /* ~23 ms at 44.1 kHz */
#define HEADER_SNIFF 64

/* MP3 streaming state (minimp3) */
#define MP3_IN_CAP 16384
#define MP3_OUT_FRAMES 2304 /* two max MP3 frames, per channel */
/* NOTE: s_mp3_dec (BSS) sits directly before s_mp3_in_len in BSS layout, and
 * minimp3's decode_frame does a memset(dec, 0, sizeof(mp3dec_t)); if that size
 * ever overruns by even a few bytes it silently zeroes the neighbouring
 * s_mp3_in_len / s_mp3_in. The 1KB guard absorbs such overruns, and the large
 * in/out buffers are kept in PSRAM so a decode overrun can never corrupt the
 * decoder's own input state. */
static mp3dec_t s_mp3_dec;
/* TEMP DIAG: minimp3's decode_frame does memset(dec, 0, sizeof(mp3dec_t));
 * on the target the emitted memset was observed with a ~22KB count while the
 * object is 6668B (potential BSS overrun). Wrap the decoder in a padded
 * container so any overrun lands in pad[] instead of neighbouring BSS. */
typedef struct {
  mp3dec_t dec;
  uint8_t pad[16384];
} mp3_dec_padded_t;
static mp3_dec_padded_t s_mp3;
#define s_mp3_dec (s_mp3.dec)
static uint8_t s_mp3_guard[1024];
static size_t s_mp3_in_len = 0;
static uint8_t *s_mp3_in = NULL;   /* PSRAM, MP3_IN_CAP bytes */
static int16_t *s_mp3_out = NULL;  /* PSRAM, MP3_OUT_FRAMES*2 samples */

/* ── FLAC (dr_flac, pull-style with an onRead that blocks on HTTP) ─────────── */
static drflac *s_flac = NULL;
static uint8_t s_flac_in[8192];    /* input staging: sniffed bytes + HTTP chunks */
static size_t s_flac_in_len = 0;
static size_t s_flac_in_pos = 0;
static volatile uint64_t s_flac_frames_total = 0; /* TEMP DIAG */
static volatile uint64_t s_flac_read_calls = 0;   /* TEMP DIAG */
static volatile int s_fmt_diag = -1;              /* TEMP DIAG: sniff result */
static volatile int s_flac_open_rc = -1;          /* TEMP DIAG: flac open rc */

/* ── AAC (libhelix-aac, feed-style ADTS) ───────────────────────────────────── */
#define AAC_IN_CAP (16384)
static uint8_t *s_aac_in = NULL; /* PSRAM staging: ADTS frames + partials */

/* ── AAC (libhelix-aac, feed-style ADTS) ───────────────────────────────────── */
/* helix_malloc/helix_free declared in utils/helix_memory.h. Use PSRAM so a
 * big SBR state never squeezes the internal heap; fall back to malloc.
 * TEMP DIAG: canaries around every helix allocation to catch out-of-bounds
 * writes (48000 Hz ADTS crash hunt). */
#define HELIX_CANARY 64
typedef struct {
  uint8_t *base;
  int size;
} canary_rec_t;
static canary_rec_t s_canary[8];
static int s_canary_n = 0;
static volatile int s_canary_hit = -1; /* TEMP DIAG: RTC via dlna_cp(90+n) */
static volatile int s_canary_phase = -1;

void *helix_malloc(int size) {
  uint8_t *p =
      heap_caps_malloc((size_t)size + 2 * HELIX_CANARY,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = malloc((size_t)size + 2 * HELIX_CANARY);
  }
  if (!p) {
    return NULL;
  }
  memset(p, 0xAA, HELIX_CANARY);
  memset(p + HELIX_CANARY + size, 0xAA, HELIX_CANARY);
  if (s_canary_n < 8) {
    s_canary[s_canary_n].base = p;
    s_canary[s_canary_n].size = size;
    s_canary_n++;
  }
  return p + HELIX_CANARY;
}
void helix_free(void *ptr) {
  if (!ptr) return;
  uint8_t *p = (uint8_t *)ptr - HELIX_CANARY;
  heap_caps_free(p);
}
int aac_canary_check(void) {
  /* returns index of smashed canary, or -1 */
  for (int i = 0; i < s_canary_n; i++) {
    for (int j = 0; j < HELIX_CANARY; j++) {
      if (s_canary[i].base[j] != 0xAA) return i;
      if (s_canary[i].base[HELIX_CANARY + s_canary[i].size + j] != 0xAA)
        return i;
    }
  }
  /* heap-buffer tail canaries: 200=mp3_out, 201=aac_in */
  if (s_mp3_out) {
    const uint8_t *t =
        (const uint8_t *)s_mp3_out + MP3_OUT_FRAMES * 2 * sizeof(int16_t);
    for (int j = 0; j < 64; j++)
      if (t[j] != 0xBB) return 200;
  }
  if (s_aac_in) {
    const uint8_t *t = (const uint8_t *)s_aac_in + AAC_IN_CAP;
    for (int j = 0; j < 64; j++)
      if (t[j] != 0xCC) return 201;
  }
  return -1;
}

#define AAC_IN_CAP (16384)
static NeAACDecHandle s_faad = NULL;
static bool s_faad_inited = false;
static size_t s_aac_in_len = 0;
static AACFrameInfo s_aac_info;
static volatile uint64_t s_aac_frames_total = 0; /* TEMP DIAG */
static volatile int s_aac_open_rc = -1;          /* TEMP DIAG */
static volatile uint32_t s_aac_feed_calls = 0;   /* TEMP DIAG */

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
/* PSRAM-backed task stack: 32KB in internal RAM repeatedly failed to
 * allocate (ESP_ERR_NO_MEM) once httpd/USB/FIFO are up, while a smaller
 * internal stack overflows minimp3's ~16KB decode scratch. */
static StaticTask_t s_stream_tcb;
static StackType_t *s_stream_stack = NULL;

static volatile bool s_active = false;   /* task is running a stream */
static volatile bool s_paused = false;   /* task is paused (HTTP idle) */
static char s_uri[DLNA_URI_MAX] = {0};
static uint32_t s_rate = 44100;
static uint32_t s_channels = 2;
static uint32_t s_bits = 16;

/* Update the stream sample rate.  Any PCM already queued in the USB FIFO was
 * fed at the previous rate — if the rate just jumped (e.g. the first MP3 frame
 * reports 48 kHz after the sniffed header said 44.1 kHz) that stale block
 * would be played back shifted, heard as a slowdown + pop.  Drop it so the
 * resampler restarts cleanly; the playback task re-prefills silence. */
static void dlna_set_rate(uint32_t rate) {
  if (rate > 0 && rate != s_rate) {
    s_rate = rate;
    audio_output_flush();
    ESP_LOGI(TAG, "Source rate %u Hz", rate);
  }
}

/* Decoded-PCM accounting for position/duration (WAV mode). */
static volatile uint64_t s_pcm_frames_played = 0;
static volatile uint32_t s_dlna_feed_calls = 0; /* TEMP DIAG: feed_pcm invocations */
static volatile int s_play_last_err = 0;        /* TEMP DIAG: last dlna_stream_play rc */
static volatile uint32_t s_read_calls = 0;      /* TEMP DIAG: stream-loop reads */
static volatile uint32_t s_mp3_diag = 0;        /* TEMP DIAG: mp3 decode probe */
static volatile uint32_t s_mp3_diag2 = 0;       /* TEMP DIAG: mp3 feed probe */
static volatile int s_http_status = 0;          /* TEMP DIAG: last HTTP status */
static volatile int s_http_err = 0;             /* TEMP DIAG: last http open err */
static volatile uint32_t s_mp3_frames_total = 0;/* TEMP DIAG: decoded MP3 frames */
static volatile uint32_t s_mp3_feed_calls = 0;  /* TEMP DIAG: mp3_feed calls */
static volatile int s_stream_end = 0;           /* TEMP DIAG: why stream loop exited */
static volatile uint64_t s_pcm_frames_total = 0;
static volatile double s_duration = 0.0;
static double s_seek_target = 0.0; /* WAV: drop frames until this time */

static int16_t *s_pcm_buf = NULL;

/* 鈹€鈹€ format sniffing 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

typedef enum {
  FMT_WAV = 0,
  FMT_MP3,
  FMT_AAC,
  FMT_FLAC,
  FMT_OGG,
  FMT_ALAC,
  FMT_UNKNOWN,
} stream_format_t;

static stream_format_t sniff_format(const uint8_t *h, size_t n) {
  if (n >= 12 && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0) {
    return FMT_WAV;
  }
  if (n >= 4 && memcmp(h, "fLaC", 4) == 0) {
    return FMT_FLAC;
  }
  if (n >= 4 && memcmp(h, "OggS", 4) == 0) {
    return FMT_OGG;
  }
  if (n >= 12 && memcmp(h + 4, "ftyp", 4) == 0) {
    return FMT_ALAC; /* M4A container (ALAC expected; validated on open) */
  }
  if (n >= 2 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) {
    return FMT_AAC; /* ADTS AAC: 12-bit sync 0xFFF */
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

/* 鈹€鈹€ WAV parsing 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

typedef struct {
  bool have_fmt;
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  uint64_t data_remaining; /* bytes of PCM still expected (0 = unknown) */
  uint64_t data_total;     /* full data-chunk length in bytes (duration) */
  uint64_t data_pos;       /* PCM bytes consumed so far */
} wav_ctx_t;

/* Parse RIFF chunks from a byte stream. Returns:
 *   >0 : PCM frames produced (caller feeds them)
 *    0 : need more bytes
 *   -1 : fatal error
 * Parses the fmt chunk (rate/channels/bits) and streams the data chunk.
 * Chunk state persists between calls via the static locals. */
static int wav_consume(wav_ctx_t *ctx, const uint8_t *buf, size_t len,
                       int16_t *pcm, size_t pcm_cap, size_t *pcm_frames) {
  static uint8_t header[8];
  static size_t header_off = 0;
  static uint32_t chunk_len = 0;
  static uint8_t fmt_buf[16];  /* fmt chunk body, first 16 bytes */
  static size_t fmt_off = 0;
  static uint32_t chunk_kind = 0; /* 0 none, 1 fmt, 2 data, 3 skip */
  static uint32_t skip_remaining = 0;

  size_t i = 0;
  *pcm_frames = 0;

  while (i < len) {
    if (chunk_kind == 0) {
      /* reading a chunk header: "XXXX" + u32 LE */
      size_t need = 8 - header_off;
      size_t take = len - i < need ? len - i : need;
      memcpy(header + header_off, buf + i, take);
      header_off += take;
      i += take;
      if (header_off == 8) {
        uint32_t id = header[0] | ((uint32_t)header[1] << 8) |
                      ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
        chunk_len = (uint32_t)header[4] | ((uint32_t)header[5] << 8) |
                    ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);
        header_off = 0;
        if (id == 0x20746D66) { /* 'fmt ' */
          chunk_kind = 1;
          fmt_off = 0;
          ctx->have_fmt = false;
        } else if (id == 0x61746164) { /* 'data' */
          chunk_kind = 2;
          ctx->data_remaining = chunk_len;
          ctx->data_total = chunk_len;
          if (!ctx->have_fmt) {
            ESP_LOGW(TAG, "WAV data chunk before fmt; assuming 44.1k/16/2");
            ctx->rate = 44100;
            ctx->channels = 2;
            ctx->bits = 16;
          }
        } else {
          chunk_kind = 3;
          skip_remaining = chunk_len;
        }
      }
      continue;
    }

    if (chunk_kind == 1) {
      /* collect fmt body (need 16 bytes) */
      size_t need = 16 - fmt_off;
      size_t take = len - i < need ? len - i : need;
      memcpy(fmt_buf + fmt_off, buf + i, take);
      fmt_off += take;
      i += take;
      if (fmt_off == 16) {
        uint16_t audio_format = fmt_buf[0] | (fmt_buf[1] << 8);
        uint16_t channels = fmt_buf[2] | (fmt_buf[3] << 8);
        uint32_t rate = (uint32_t)fmt_buf[4] | ((uint32_t)fmt_buf[5] << 8) |
                        ((uint32_t)fmt_buf[6] << 16) |
                        ((uint32_t)fmt_buf[7] << 24);
        uint16_t bits = fmt_buf[14] | (fmt_buf[15] << 8);
        if (audio_format != 1) { /* PCM only for now */
          ESP_LOGE(TAG, "WAV audio format %u unsupported (PCM=1)",
                   audio_format);
          return -1;
        }
        if (bits != 16 && bits != 24) {
          ESP_LOGE(TAG, "WAV bit depth %u unsupported (16/24)", bits);
          return -1;
        }
        ctx->rate = rate;
        ctx->channels = channels ? channels : 2;
        ctx->bits = bits;
        ctx->have_fmt = true;
        /* skip the rest of the fmt chunk body */
        skip_remaining = chunk_len > 16 ? chunk_len - 16 : 0;
        chunk_kind = 3;
      }
      continue;
    }

    if (chunk_kind == 2) {
      /* data chunk: convert PCM bytes to int16 stereo frames */
      size_t avail = chunk_len;
      if (avail > len - i) {
        avail = len - i;
      }
      if (avail == 0) {
        chunk_kind = 0; /* next header */
        continue;
      }
      const uint8_t *p = buf + i;
      size_t frame_bytes = ctx->channels * (ctx->bits / 8);
      if (frame_bytes == 0) {
        return -1;
      }
      size_t frames_in = avail / frame_bytes;
      size_t cap = pcm_cap / 2;
      if (frames_in > cap) {
        frames_in = cap;
      }
      if (frames_in > 0) {
        if (ctx->bits == 16) {
          memcpy(pcm, p, frames_in * frame_bytes);
        } else { /* 24-bit LE -> int16 (high bits) */
          for (size_t f = 0; f < frames_in * ctx->channels; f++) {
            int32_t v = (int32_t)p[f * 3] | ((int32_t)p[f * 3 + 1] << 8) |
                        ((int32_t)p[f * 3 + 2] << 16);
            pcm[f] = (int16_t)(v >> 8);
          }
        }
        *pcm_frames = frames_in;
        ctx->data_pos += frames_in * frame_bytes;
        chunk_len -= (uint32_t)(frames_in * frame_bytes);
        i += frames_in * frame_bytes;
        if (chunk_len == 0) {
          chunk_kind = 0;
        }
        return 1; /* caller feeds one PCM chunk */
      }
      /* frame-aligned but zero capacity: force progress */
      i = len;
      return 1;
    }

    /* chunk_kind == 3: skip body */
    size_t skip_n = skip_remaining;
    if (skip_n > len - i) {
      skip_n = len - i;
    }
    i += skip_n;
    skip_remaining -= (uint32_t)skip_n;
    if (skip_remaining == 0) {
      chunk_kind = 0;
    }
  }
  return 0;
}

/* 鈹€鈹€ HTTP stream task 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

static void feed_pcm(int16_t *pcm, size_t frames) {
  s_dlna_feed_calls++;
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

/* ── FLAC helpers ─────────────────────────────────────────────────────────── */

/* dr_flac pull callback: serve staged bytes first, then block-read HTTP.
 * pUserData is the http client handle. s_flac_in holds the sniffed prefix
 * plus every chunk fetched from the wire, consumed as dr_flac asks. */
static size_t flac_on_read(void *pUserData, void *pBufferOut,
                           size_t bytesToRead) {
  esp_http_client_handle_t hc = (esp_http_client_handle_t)pUserData;
  uint8_t *out = (uint8_t *)pBufferOut;
  size_t got = 0;
  while (got < bytesToRead) {
    if (s_flac_in_pos < s_flac_in_len) {
      size_t take = s_flac_in_len - s_flac_in_pos;
      if (take > bytesToRead - got) {
        take = bytesToRead - got;
      }
      memcpy(out + got, s_flac_in + s_flac_in_pos, take);
      s_flac_in_pos += take;
      got += take;
    } else {
      if (!hc) {
        break;
      }
      int n = esp_http_client_read(hc, (char *)s_flac_in, sizeof(s_flac_in));
      s_flac_read_calls++;
      if (n <= 0) {
        break; /* HTTP EOF */
      }
      s_flac_in_len = (size_t)n;
      s_flac_in_pos = 0;
    }
  }
  return got;
}

/* drflac_open requires a non-NULL seek callback even though streaming
 * cannot seek: return DRFLAC_FALSE ("seek unsupported"). */
static drflac_bool32 flac_on_seek(void *pUserData, int offset,
                                  drflac_seek_origin origin) {
  (void)pUserData;
  (void)offset;
  (void)origin;
  return DRFLAC_FALSE;
}

static void flac_stop(void) {
  if (s_flac) {
    drflac_close(s_flac);
    s_flac = NULL;
  }
  s_flac_in_len = 0;
  s_flac_in_pos = 0;
}

/* Decode as much FLAC as the wire provides; returns false at EOF/error. */
static bool flac_pump(esp_http_client_handle_t hc) {
  drflac_uint64 n = drflac_read_pcm_frames_s16(
      s_flac, PCM_CHUNK_FRAMES, (drflac_int16 *)s_pcm_buf);
  if (n > 0) {
    s_flac_frames_total += n;
    feed_pcm(s_pcm_buf, (size_t)n);
    return true;
  }
  ESP_LOGI(TAG, "FLAC end (read=0)");
  return false;
}

/* ── AAC (libhelix-aac, feed-style) ────────────────────────────────────────── */

static void aac_stop(void) {
  if (s_faad) {
    NeAACDecClose(s_faad);
    s_faad = NULL;
  }
  s_faad_inited = false;
  s_aac_in_len = 0;
}

/* Feed a raw HTTP chunk; parse ADTS framing ourselves and hand helix only
 * complete frames (it does not tolerate partial frames and will garbage-
 * decode them, which can walk out of bounds). s_mp3_out is reused as the
 * PCM sink (4608 int16 > 2048 frames * 2ch). */
static size_t aac_feed(const uint8_t *buf, size_t len) {
  if (s_aac_in_len + len > AAC_IN_CAP) {
    /* keep the newest bytes (drop the oldest partial data) */
    size_t keep = AAC_IN_CAP - len;
    if (s_aac_in_len > keep) {
      memmove(s_aac_in, s_aac_in + (s_aac_in_len - keep), keep);
      s_aac_in_len = keep;
    }
  }
  memcpy(s_aac_in + s_aac_in_len, buf, len);
  s_aac_in_len += len;

  size_t fed = 0;
  while (s_aac_in_len >= 7) {
    /* validate ADTS sync, else drop bytes up to the next sync word */
    if (s_aac_in[0] != 0xFF || (s_aac_in[1] & 0xF6) != 0xF0) {
      int sync = AACFindSyncWord(s_aac_in, (int)s_aac_in_len);
      if (sync <= 0) {
        s_aac_in_len = 0;
        break;
      }
      memmove(s_aac_in, s_aac_in + sync, s_aac_in_len - (size_t)sync);
      s_aac_in_len -= (size_t)sync;
      continue;
    }
    int flen = ((s_aac_in[3] & 0x03) << 11) | (s_aac_in[4] << 3) |
               (s_aac_in[5] >> 5);
    if (flen < 7) {
      s_aac_in_len = 0;
      break;
    }
    if ((int)s_aac_in_len < flen) {
      break; /* wait for the rest of this frame */
    }

    dlna_cp(11); /* before faad decode */
    if (!s_faad_inited) {
      unsigned long f_rate = 0;
      unsigned char f_ch = 0;
      long irc = NeAACDecInit(s_faad, s_aac_in, (unsigned long)s_aac_in_len,
                              &f_rate, &f_ch);
      s_faad_inited = true;
      s_aac_open_rc = (irc < 0) ? -1 : 1;
      if (irc < 0) {
        ESP_LOGE(TAG, "faad init rc=%ld (drop frame)", irc);
      } else {
        if (f_rate > 0) dlna_set_rate((uint32_t)f_rate);
        if (f_ch > 0) s_channels = f_ch;
        ESP_LOGI(TAG, "faad init: %lu Hz %u ch obj=%ld", f_rate, f_ch, irc);
      }
    }
    {
      NeAACDecFrameInfo fi;
      size_t consumed;
      memset(&fi, 0, sizeof(fi));
      void *pcm = NeAACDecDecode(s_faad, &fi, s_aac_in, (unsigned long)flen);
      dlna_cp(12); /* after faad decode */
      consumed = fi.bytesconsumed > 0 ? (size_t)fi.bytesconsumed
                                      : (size_t)flen;
      if (fi.error > 0) {
        ESP_LOGI(TAG, "faad err=%u consumed=%lu", (unsigned)fi.error,
                 (unsigned long)consumed);
      } else {
        uint32_t ch = fi.channels > 0 ? (uint32_t)fi.channels : 2;
        uint32_t total = (uint32_t)fi.samples; /* interleaved total */
        uint32_t frames = ch > 0 ? total / ch : 0;
        if (frames > 0 && pcm) {
          if (fi.samplerate > 0) dlna_set_rate((uint32_t)fi.samplerate);
          s_channels = ch;
          dlna_cp(14); /* before feed_pcm */
          feed_pcm((const int16_t *)pcm, frames);
          dlna_cp(15); /* after feed_pcm */
          s_aac_frames_total += frames;
          fed += frames;
        }
      }
      s_aac_feed_calls++;
      dlna_cp(16); /* before consume */
      if (consumed < (size_t)flen) {
        consumed = (size_t)flen;
      }
      if (consumed >= s_aac_in_len) {
        s_aac_in_len = 0;
        break;
      }
      memmove(s_aac_in, s_aac_in + consumed, s_aac_in_len - consumed);
      s_aac_in_len -= consumed;
    }
  }
  return fed;
}

/* Feed HTTP bytes into the MP3 decoder; decode and feed as many frames as
 * the input allows. Returns frames fed (0 = need more input). */
static size_t mp3_feed(const uint8_t *buf, size_t len) {
  s_mp3_feed_calls++;
  dlna_cp(20); /* mp3_feed entry */
  if (s_mp3_in_len + len > MP3_IN_CAP) {
    /* Keep the newest bytes, drop the oldest (should not happen once frames
     * are consumed, but protects against a pathological header stream). */
    size_t overflow = s_mp3_in_len + len - MP3_IN_CAP;
    size_t keep = MP3_IN_CAP - len;
    memmove(s_mp3_in, s_mp3_in + overflow, keep);
    s_mp3_in_len = keep;
  }
  memcpy(s_mp3_in + s_mp3_in_len, buf, len);
  s_mp3_in_len += len;
  dlna_cp(21); /* input buffered */
  if ((s_mp3_diag2++ % 25) == 0) {
    ESP_LOGI(TAG, "feed: len=%u in=%u b0=%02X%02X%02X%02X %02X%02X%02X%02X",
             (unsigned)len, (unsigned)s_mp3_in_len, s_mp3_in[0],
             s_mp3_in[1], s_mp3_in[2], s_mp3_in[3], s_mp3_in[4],
             s_mp3_in[5], s_mp3_in[6], s_mp3_in[7]);
  }

  size_t fed = 0;
  while (s_mp3_in_len > 0) {
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    dlna_cp(22); /* before decode_frame */
    int n = mp3dec_decode_frame(&s_mp3_dec, s_mp3_in, (int)s_mp3_in_len,
                                s_mp3_out, &info);
    dlna_cp(23); /* after decode_frame */
    if ((s_mp3_diag++ % 25) == 0) {
      ESP_LOGI(TAG,
               "mp3dec: n=%d off=%d fb=%d in=%d "
               "b0=%02X%02X%02X%02X %02X%02X%02X%02X",
               n, info.frame_offset, info.frame_bytes, (int)s_mp3_in_len,
               s_mp3_in[0], s_mp3_in[1], s_mp3_in[2], s_mp3_in[3],
               s_mp3_in[4], s_mp3_in[5], s_mp3_in[6], s_mp3_in[7]);
    }
    if (n <= 0) {
      size_t skip = (size_t)info.frame_offset;
      if (skip > 0) {
        /* Skipped data (ID3 / junk): consume it and continue. */
        if (skip > s_mp3_in_len) {
          skip = s_mp3_in_len;
        }
        memmove(s_mp3_in, s_mp3_in + skip, s_mp3_in_len - skip);
        s_mp3_in_len -= skip;
        continue;
      }
      break; /* need more input */
    }
    if (info.hz > 0 && (uint32_t)info.hz != s_rate) {
      dlna_set_rate((uint32_t)info.hz);
    }
    if (info.channels > 0 && (uint32_t)info.channels != s_channels) {
      s_channels = (uint32_t)info.channels;
    }
    size_t frames = (size_t)n;
    dlna_cp(24); /* before feed_pcm */
    feed_pcm(s_mp3_out, frames);
    dlna_cp(25); /* after feed_pcm */
    fed += frames;
    s_mp3_frames_total += frames;
    size_t consumed = info.frame_offset > 0 ? (size_t)info.frame_offset
                                             : (size_t)info.frame_bytes;
    if (consumed == 0 || consumed > s_mp3_in_len) {
      break;
    }
    memmove(s_mp3_in, s_mp3_in + consumed, s_mp3_in_len - consumed);
    s_mp3_in_len -= consumed;
    if (s_mp3_in_len < 2048) {
      break; /* refill from HTTP before more decoding */
    }
  }
  return fed;
}

/* ── OGG Vorbis (stb_vorbis pushdata streaming) ──────────────────────────── */
#define OGG_IN_CAP 16384
static uint8_t s_ogg_in[OGG_IN_CAP];
static size_t s_ogg_in_len = 0;
static stb_vorbis *s_ogg = NULL;
static bool s_ogg_opened = false;
static volatile uint64_t s_ogg_frames_total = 0;
static volatile int s_ogg_open_rc = -1;

static void ogg_stop(void) {
  if (s_ogg) {
    stb_vorbis_close(s_ogg);
    s_ogg = NULL;
  }
  s_ogg_in_len = 0;
  s_ogg_opened = false;
}

/* Feed a raw HTTP chunk: accumulate, open the decoder once the three header
 * packets have arrived, then decode as many frames as the staged bytes allow.
 * stb_vorbis pushdata is the no-seek streaming API. */
static void ogg_feed(const uint8_t *buf, size_t len) {
  if (s_ogg_in_len + len > OGG_IN_CAP) {
    size_t keep = OGG_IN_CAP - len;
    if (s_ogg_in_len > keep) {
      memmove(s_ogg_in, s_ogg_in + (s_ogg_in_len - keep), keep);
      s_ogg_in_len = keep;
    }
  }
  memcpy(s_ogg_in + s_ogg_in_len, buf, len);
  s_ogg_in_len += len;

  if (!s_ogg && !s_ogg_opened) {
    int used = 0;
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_pushdata(s_ogg_in, (int)s_ogg_in_len,
                                             &used, &err, NULL);
    if (v) {
      s_ogg = v;
      s_ogg_opened = true;
      s_ogg_open_rc = 1;
      stb_vorbis_info info = stb_vorbis_get_info(s_ogg);
      dlna_set_rate(info.sample_rate ? (uint32_t)info.sample_rate : 44100);
      s_channels = (uint32_t)info.channels;
      s_bits = 16;
      ESP_LOGI(TAG, "OGG opened: %u Hz %u ch (header used=%d)", s_rate,
               s_channels, used);
      if (used > 0 && (size_t)used < s_ogg_in_len) {
        memmove(s_ogg_in, s_ogg_in + used, s_ogg_in_len - (size_t)used);
        s_ogg_in_len -= (size_t)used;
      } else if (used > 0) {
        s_ogg_in_len = 0;
      }
    } else if (err == VORBIS_need_more_data) {
      s_ogg_open_rc = 0; /* keep accumulating */
    } else {
      ESP_LOGE(TAG, "OGG open failed (err=%d)", err);
      s_ogg_open_rc = -1;
      s_ogg_in_len = 0;
      return;
    }
  }

  if (!s_ogg) {
    return;
  }
  size_t pos = 0;
  while (pos < s_ogg_in_len) {
    int ch = 0;
    float **outs = NULL;
    int samples = 0;
    int used2 = stb_vorbis_decode_frame_pushdata(
        s_ogg, s_ogg_in + pos, (int)(s_ogg_in_len - pos), &ch, &outs,
        &samples);
    if (used2 <= 0) {
      break; /* need more data */
    }
    pos += (size_t)used2;
    if (samples > 0 && outs) {
      if (ch > 2) {
        ch = 2;
      }
      int done = 0;
      while (done < samples) {
        int chunk = samples - done;
        if (chunk > (int)PCM_CHUNK_FRAMES) {
          chunk = (int)PCM_CHUNK_FRAMES;
        }
        int16_t *dst = s_pcm_buf;
        for (int f = 0; f < chunk; f++) {
          for (int c = 0; c < ch; c++) {
            float v = outs[c][done + f];
            if (v < -1.0f) {
              v = -1.0f;
            } else if (v > 1.0f) {
              v = 1.0f;
            }
            dst[f * ch + c] = (int16_t)(v * 32767.0f);
          }
        }
        s_ogg_frames_total += (uint64_t)chunk;
        feed_pcm(dst, (size_t)chunk);
        done += chunk;
      }
    }
  }
  if (pos > 0) {
    memmove(s_ogg_in, s_ogg_in + pos, s_ogg_in_len - pos);
    s_ogg_in_len -= pos;
  }
}

/* ── ALAC (Apple Lossless, M4A container) ──────────────────────────────── */
extern void *alac_dec_create(const uint8_t *cookie, uint32_t cookie_len);
extern void alac_dec_get_info(void *h, uint32_t *rate, uint32_t *channels,
                              uint32_t *bits);
extern int alac_dec_decode(void *h, const uint8_t *frame, uint32_t frame_len,
                           int16_t *out, uint32_t *out_frames);
extern void alac_dec_destroy(void *h);

#define ALAC_MAX_MOOV (128 * 1024)
static uint8_t *s_alac_moov = NULL;
static size_t s_alac_moov_len = 0;
static void *s_alac = NULL;
static uint8_t s_alac_cookie[32];
static uint32_t s_alac_cookie_len = 0;
static bool s_alac_have_moov = false;
static uint32_t s_alac_rate = 0, s_alac_channels = 0, s_alac_bits = 0;
static uint32_t s_alac_frame_length = 4096;
static int s_alac_top = 0; /* 0 atom hdr, 1 skip, 2 moov, 3 mdat */
static uint32_t s_alac_payload_left = 0;
static uint8_t s_alac_ah[8];
static size_t s_alac_ah_off = 0;
static uint32_t s_alac_flen = 0;
static uint8_t s_alac_fhdr[4];
static size_t s_alac_fhdr_off = 0;
static uint8_t *s_alac_fbuf = NULL;
static uint32_t s_alac_fbuf_cap = 0;
static uint32_t s_alac_fbuf_len = 0;
static int16_t *s_alac_pcm = NULL;
static uint32_t s_alac_pcm_cap = 0;
static volatile uint64_t s_alac_frames_total = 0;
static uint32_t s_alac_feed_count = 0;
static volatile int s_alac_open_rc = -1;
static int s_alac_init_rc = 0;
static uint32_t s_alac_init_fl = 0;
static uint32_t s_alac_init_bd = 0;
static uint32_t s_alac_init_ch = 0;
static uint32_t s_alac_init_sr = 0;
static uint32_t s_alac_stsz_uniform = 0;
static uint32_t s_alac_stsz_count = 0;
static uint32_t s_alac_stsz_idx = 0;
static uint32_t *s_alac_stsz = NULL;
static uint32_t s_alac_stsz_cap = 0;

static uint32_t alac_be32(const uint8_t *pp) {
  return ((uint32_t)pp[0] << 24) | ((uint32_t)pp[1] << 16) |
         ((uint32_t)pp[2] << 8) | (uint32_t)pp[3];
}

static int alac_find_atom(const uint8_t *pp, uint32_t size, const char *want,
                          const uint8_t **out, uint32_t *out_len) {
  uint32_t off = 0;
  while (off + 8 <= size) {
    uint32_t asz = alac_be32(pp + off);
    if (asz < 8 || off + asz > size) {
      break;
    }
    if (memcmp(pp + off + 4, want, 4) == 0) {
      *out = pp + off;
      *out_len = asz;
      return 0;
    }
    if (memcmp(pp + off + 4, "moov", 4) == 0 ||
        memcmp(pp + off + 4, "trak", 4) == 0 ||
        memcmp(pp + off + 4, "mdia", 4) == 0 ||
        memcmp(pp + off + 4, "minf", 4) == 0 ||
        memcmp(pp + off + 4, "stbl", 4) == 0 ||
        memcmp(pp + off + 4, "mp4a", 4) == 0) {
      if (alac_find_atom(pp + off + 8, asz - 8, want, out, out_len) == 0) {
        return 0;
      }
    } else if (memcmp(pp + off + 4, "stsd", 4) == 0 && asz >= 16) {
      /* stsd payload: version/flags(4) + entry_count(4) + entries */
      if (alac_find_atom(pp + off + 16, asz - 16, want, out, out_len) == 0) {
        return 0;
      }
    }
    off += asz;
  }
  return -1;
}

static int alac_parse_moov(const uint8_t *moov, size_t moov_len) {
  const uint8_t *alac_atom = NULL;
  uint32_t alac_len = 0;
  if (alac_find_atom(moov, (uint32_t)moov_len, "alac", &alac_atom,
                     &alac_len) != 0) {
    ESP_LOGE(TAG, "ALAC: no alac atom in moov (AAC/M4A not supported)");
    return -1;
  }
  /* AudioSampleEntry layout:
     [0:8] size+type, [8:36] fixed fields, then codec-specific data.
     ffmpeg nests a full 'alac' atom there (size@36, type@40, version@44,
     config@48); iTunes writes the 24-byte ALACSpecificConfig directly at 36. */
  if (alac_len < 36) {
    ESP_LOGE(TAG, "ALAC: alac atom too small");
    return -1;
  }
  const uint8_t *cfg = NULL;
  if (alac_len >= 48 && memcmp(alac_atom + 40, "alac", 4) == 0) {
    cfg = alac_atom + 48;
  } else if (alac_len >= 60) {
    cfg = alac_atom + 36;
  } else {
    ESP_LOGE(TAG, "ALAC: cookie not found in stsd entry");
    return -1;
  }
  s_alac_cookie_len = 24;
  memcpy(s_alac_cookie, cfg, 24);
  s_alac_frame_length = alac_be32(cfg);
  if (s_alac_frame_length == 0 || s_alac_frame_length > 8192) {
    s_alac_frame_length = 4096;
  }
  /* stsz: per-sample sizes (M4A mdat has no length prefix) */
  {
    const uint8_t *sz_atom = NULL;
    uint32_t sz_len = 0;
    s_alac_stsz_uniform = 0;
    s_alac_stsz_count = 0;
    s_alac_stsz_idx = 0;
    if (alac_find_atom(moov, (uint32_t)moov_len, "stsz", &sz_atom,
                       &sz_len) != 0) {
      ESP_LOGE(TAG, "ALAC: stsz atom not found");
      return -1;
    }
    if (sz_len < 20) {
      ESP_LOGE(TAG, "ALAC: stsz too small");
      return -1;
    }
    uint32_t ss = alac_be32(sz_atom + 12);
    uint32_t cnt = alac_be32(sz_atom + 16);
    s_alac_stsz_uniform = ss;
    s_alac_stsz_count = cnt;
    if (ss == 0) {
      if (cnt == 0 || sz_len < 20 + cnt * 4) {
        ESP_LOGE(TAG, "ALAC: stsz table missing");
        return -1;
      }
      if (s_alac_stsz_cap < cnt) {
        uint32_t *np = heap_caps_realloc(s_alac_stsz, (size_t)cnt * 4,
                                         MALLOC_CAP_SPIRAM);
        if (!np) {
          ESP_LOGE(TAG, "ALAC: stsz alloc failed");
          return -1;
        }
        s_alac_stsz = np;
        s_alac_stsz_cap = cnt;
      }
      for (uint32_t i = 0; i < cnt; i++) {
        s_alac_stsz[i] = alac_be32(sz_atom + 20 + i * 4);
      }
    }
  }
  ESP_LOGI(TAG, "ALAC: stsz uniform=%u count=%u", s_alac_stsz_uniform,
           s_alac_stsz_count);
  return 0;
}

static void alac_stop(void) {
  if (s_alac) {
    alac_dec_destroy(s_alac);
    s_alac = NULL;
  }
  s_alac_top = 0;
  s_alac_ah_off = 0;
  s_alac_have_moov = false;
  s_alac_moov_len = 0;
  s_alac_fhdr_off = 0;
  s_alac_fbuf_len = 0;
  s_alac_flen = 0;
  s_alac_stsz_idx = 0;
  s_alac_stsz_count = 0;
  s_alac_stsz_uniform = 0;
}

static void alac_feed(const uint8_t *buf, size_t len) {
  size_t i = 0;
  while (i < len) {
    if (s_alac_top == 3) {
      /* ---- inside mdat: ALAC frames sized by stsz (no length prefix) ---- */
      if (s_alac_stsz_idx >= s_alac_stsz_count) {
        return; /* all frames consumed; ignore any trailing bytes */
      }
      if (s_alac_fbuf_len == 0) {
        s_alac_flen = s_alac_stsz_uniform
                          ? s_alac_stsz_uniform
                          : s_alac_stsz[s_alac_stsz_idx];
        if (s_alac_flen == 0 || s_alac_flen > 1024 * 1024) {
          ESP_LOGE(TAG, "ALAC: bad frame len %u (idx %u)", s_alac_flen,
                   s_alac_stsz_idx);
          s_alac_open_rc = -4;
          return;
        }
        if (s_alac_fbuf_cap < s_alac_flen) {
          uint8_t *nb = heap_caps_realloc(s_alac_fbuf, s_alac_flen,
                                          MALLOC_CAP_SPIRAM);
          if (!nb) {
            s_alac_open_rc = -5;
            return;
          }
          s_alac_fbuf = nb;
          s_alac_fbuf_cap = s_alac_flen;
        }
      }
      {
        size_t needf = s_alac_flen - s_alac_fbuf_len;
        size_t takef = (len - i < needf) ? len - i : needf;
        memcpy(s_alac_fbuf + s_alac_fbuf_len, buf + i, takef);
        s_alac_fbuf_len += (uint32_t)takef;
        i += takef;
        if (s_alac_fbuf_len < s_alac_flen) {
          break; /* frame spans next chunk */
        }
      }
      s_alac_stsz_idx++;
      uint32_t outn = 0;
      s_alac_feed_count++;
      if (alac_dec_decode(s_alac, s_alac_fbuf, s_alac_flen, s_alac_pcm,
                          &outn) == 0 && outn > 0) {
        s_alac_frames_total += outn;
        uint32_t done = 0;
        while (done < outn) {
          uint32_t chunk = outn - done;
          if (chunk > (uint32_t)PCM_CHUNK_FRAMES) {
            chunk = (uint32_t)PCM_CHUNK_FRAMES;
          }
          feed_pcm(s_alac_pcm + (size_t)done * s_alac_channels, chunk);
          done += chunk;
        }
      }
      s_alac_fbuf_len = 0;
      s_alac_fhdr_off = 0;
      continue;
    }
    if (s_alac_top == 0) {
      /* ---- atom header ---- */
      size_t need = 8 - s_alac_ah_off;
      size_t take = (len - i < need) ? len - i : need;
      memcpy(s_alac_ah + s_alac_ah_off, buf + i, take);
      s_alac_ah_off += take;
      i += take;
      if (s_alac_ah_off < 8) {
        break;
      }
      uint32_t sz = alac_be32(s_alac_ah);
      const char *tp = (const char *)s_alac_ah + 4;
      uint32_t payload = 0;
      if (sz == 1) {
        ESP_LOGE(TAG, "ALAC: 64-bit atoms unsupported");
        s_alac_open_rc = -2;
        return;
      } else if (sz == 0) {
        payload = 0xFFFFFFFFu;
      } else if (sz >= 8) {
        payload = sz - 8;
      }
      s_alac_ah_off = 0;
      if (memcmp(tp, "moov", 4) == 0) {
        s_alac_top = 2;
        s_alac_moov_len = 0;
        s_alac_payload_left = payload;
        if (!s_alac_moov) {
          s_alac_moov = heap_caps_malloc(ALAC_MAX_MOOV, MALLOC_CAP_SPIRAM);
          if (!s_alac_moov) {
            s_alac_open_rc = -7;
            return;
          }
        }
      } else if (memcmp(tp, "mdat", 4) == 0) {
        if (!s_alac_have_moov) {
          ESP_LOGE(TAG, "ALAC: mdat before moov (moov-at-end unsupported)");
          s_alac_open_rc = -3;
          return;
        }
        s_alac_top = 3;
        s_alac_fhdr_off = 0;
      } else {
        s_alac_top = 1;
        s_alac_payload_left = payload;
      }
      continue;
    }
    if (s_alac_top == 1) {
      /* ---- skip atom payload ---- */
      size_t take = (len - i < s_alac_payload_left) ? len - i
                                                    : s_alac_payload_left;
      i += take;
      s_alac_payload_left -= (uint32_t)take;
      if (s_alac_payload_left == 0) {
        s_alac_top = 0;
      }
      continue;
    }
    if (s_alac_top == 2) {
      /* ---- collect moov payload ---- */
      size_t take = (len - i < s_alac_payload_left) ? len - i
                                                    : s_alac_payload_left;
      if (s_alac_moov_len + take > ALAC_MAX_MOOV) {
        ESP_LOGE(TAG, "ALAC: moov too large");
        s_alac_open_rc = -7;
        return;
      }
      memcpy(s_alac_moov + s_alac_moov_len, buf + i, take);
      s_alac_moov_len += take;
      i += take;
      s_alac_payload_left -= (uint32_t)take;
      if (s_alac_payload_left == 0) {
        s_alac_top = 0;
        if (alac_parse_moov(s_alac_moov, s_alac_moov_len) != 0) {
          s_alac_open_rc = -8;
          return;
        }
        s_alac_have_moov = true;
        s_alac = alac_dec_create(s_alac_cookie, s_alac_cookie_len);
        if (!s_alac) {
          s_alac_open_rc = -9;
          alac_wrap_get_init_diag(&s_alac_init_rc, &s_alac_init_fl,
                                  &s_alac_init_bd, &s_alac_init_ch,
                                  &s_alac_init_sr);
          ESP_LOGE(TAG, "ALAC create failed: diag rc=%d fl=%u bd=%u ch=%u sr=%u",
                   s_alac_init_rc, s_alac_init_fl, s_alac_init_bd,
                   s_alac_init_ch, s_alac_init_sr);
          return;
        }
        s_alac_open_rc = 1;
        alac_dec_get_info(s_alac, &s_alac_rate, &s_alac_channels,
                          &s_alac_bits);
        dlna_set_rate(s_alac_rate ? s_alac_rate : 44100);
        s_channels = s_alac_channels ? s_alac_channels : 2;
        s_bits = 16;
        uint32_t pcm_frames = s_alac_frame_length * s_alac_channels;
        if (s_alac_pcm_cap < pcm_frames) {
          int16_t *np = heap_caps_realloc(s_alac_pcm, (size_t)pcm_frames * 2,
                                          MALLOC_CAP_SPIRAM);
          if (!np) {
            s_alac_open_rc = -10;
            return;
          }
          s_alac_pcm = np;
          s_alac_pcm_cap = pcm_frames;
        }
        ESP_LOGI(TAG, "ALAC opened: %u Hz %u ch %u bit (frameLen=%u)",
                 s_alac_rate, s_alac_channels, s_alac_bits,
                 s_alac_frame_length);
      }
      continue;
    }
  }
}

static void stream_task(void *arg) {
  esp_http_client_handle_t client = NULL;
  wav_ctx_t wav;
  stream_msg_t msg;
  bool in_stream = false;
  stream_format_t fmt = FMT_UNKNOWN;

  memset(&wav, 0, sizeof(wav));
  s_pcm_buf = heap_caps_malloc(PCM_CHUNK_FRAMES * 2 * sizeof(int16_t),
                               MALLOC_CAP_SPIRAM);
  if (!s_pcm_buf) {
    ESP_LOGW(TAG, "SPIRAM alloc failed, falling back to internal RAM");
    s_pcm_buf = malloc(PCM_CHUNK_FRAMES * 2 * sizeof(int16_t));
  }
  if (!s_pcm_buf) {
    ESP_LOGE(TAG, "pcm buf alloc failed");
    s_active = false;
    vTaskDelete(NULL);
    return;
  }
  /* MP3 decode buffers live in PSRAM so a decode overrun can never corrupt
   * BSS neighbours (s_mp3_in_len etc). Allocated once for the task lifetime.
   * TEMP DIAG: 64-byte tail canaries on out/aac buffers to catch overruns. */
  if (!s_mp3_in) {
    s_mp3_in = heap_caps_malloc(MP3_IN_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!s_mp3_out) {
    s_mp3_out = heap_caps_malloc(MP3_OUT_FRAMES * 2 * sizeof(int16_t) + 64,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memset((uint8_t *)s_mp3_out + MP3_OUT_FRAMES * 2 * sizeof(int16_t), 0xBB,
           64);
  }
  if (!s_aac_in) {
    s_aac_in = heap_caps_malloc(AAC_IN_CAP + 64,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memset((uint8_t *)s_aac_in + AAC_IN_CAP, 0xCC, 64);
  }
  /* HTTP read buffer on PSRAM heap instead of the 32KB task stack; the
   * esp_http_client_read chain (transport->LWIP recv) adds several KB of
   * frames on top of the 4KB buffer. */
  uint8_t *s_http_buf = heap_caps_malloc(HTTP_BUF_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_mp3_in || !s_mp3_out || !s_aac_in || !s_http_buf) {
    ESP_LOGE(TAG, "buf alloc failed in=%p out=%p aac=%p http=%p",
             (void *)s_mp3_in, (void *)s_mp3_out, (void *)s_aac_in,
             (void *)s_http_buf);
    s_active = false;
    vTaskDelete(NULL);
    return;
  }

  while (xQueueReceive(s_cmd_q, &msg, portMAX_DELAY)) {
    switch (msg.cmd) {
    case CMD_PLAY: {
      dlna_cp(35); /* CMD_PLAY received */
      if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
      }
      snprintf(s_uri, sizeof(s_uri), "%s", msg.uri);
      dlna_set_rate(msg.arg > 0 ? (uint32_t)msg.arg : 44100);
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
      s_http_err = (int)err;
      dlna_cp(1); /* http open attempted */
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed (%s): %s", esp_err_to_name(err),
                 s_uri);
        esp_http_client_cleanup(client);
        client = NULL;
        s_active = false;
        continue;
      }
      int status = esp_http_client_fetch_headers(client);
      s_http_status = status;
      dlna_cp(2); /* headers fetched */
      ESP_LOGI(TAG, "HTTP %d, content-length=%d", status,
               esp_http_client_get_content_length(client));

      /* Sniff the first bytes for format detection. */
      uint8_t sniff[HEADER_SNIFF];
      int got = esp_http_client_read(client, (char *)sniff, sizeof(sniff));
      dlna_cp(3); /* sniff read done */
      if (got <= 0) {
        ESP_LOGW(TAG, "no data from stream");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
        s_active = false;
        continue;
      }
      fmt = sniff_format(sniff, (size_t)got);
      s_fmt_diag = (int)fmt;
      dlna_cp(4); /* format detected */
      ESP_LOGI(TAG, "stream format: %s",
               fmt == FMT_WAV ? "WAV" : fmt == FMT_MP3 ? "MP3"
               : fmt == FMT_AAC ? "AAC" : fmt == FMT_FLAC ? "FLAC"
               : fmt == FMT_OGG ? "OGG" : fmt == FMT_ALAC ? "ALAC/M4A"
                                                          : "UNKNOWN");
      if (fmt != FMT_WAV && fmt != FMT_MP3 && fmt != FMT_FLAC &&
          fmt != FMT_AAC && fmt != FMT_OGG && fmt != FMT_ALAC) {
        ESP_LOGE(TAG, "format not supported yet (WAV/MP3/FLAC/AAC/OGG/ALAC)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
        s_active = false;
        continue;
      }

      if (fmt == FMT_MP3) {
        mp3dec_init(&s_mp3_dec);
        /* Keep sniffed bytes as a prefix; do NOT decode them yet (the first
         * decode_frame on the 64-byte sniff input was a panic source). The
         * main read loop will decode once enough real frames are buffered. */
        memcpy(s_mp3_in, sniff, (size_t)got);
        s_mp3_in_len = (size_t)got;
        dlna_cp(5); /* mp3 init done */
      } else if (fmt == FMT_FLAC) {
        /* Stage the sniffed bytes, then let drflac_open pull the header
         * (and more) through flac_on_read -> HTTP. */
        flac_stop();
        s_flac_in_len = (size_t)got;
        s_flac_in_pos = 0;
        memcpy(s_flac_in, sniff, (size_t)got);
        s_flac_frames_total = 0;
        s_flac_read_calls = 0;
        s_flac = drflac_open(flac_on_read, flac_on_seek, NULL, client, NULL);
        s_flac_open_rc = s_flac ? 1 : 0;
        if (!s_flac) {
          ESP_LOGE(TAG, "FLAC open failed");
          esp_http_client_close(client);
          esp_http_client_cleanup(client);
          client = NULL;
          s_active = false;
          continue;
        }
        dlna_set_rate((uint32_t)s_flac->sampleRate);
        s_channels = (uint32_t)s_flac->channels;
        s_bits = 16;
        ESP_LOGI(TAG, "FLAC opened: %u Hz %u ch", s_rate, s_channels);
        dlna_cp(9); /* flac opened */
      } else if (fmt == FMT_OGG) {
        ogg_stop();
        s_ogg_frames_total = 0;
        s_ogg_open_rc = 0;
        ogg_feed(sniff, (size_t)got);
        dlna_cp(26); /* ogg fed */
      } else if (fmt == FMT_ALAC) {
        alac_stop();
        s_alac_frames_total = 0;
        s_alac_open_rc = 0;
        alac_feed(sniff, (size_t)got);
        dlna_cp(28); /* alac init */
      } else if (fmt == FMT_AAC) {
        aac_stop();
        ESP_LOGI(TAG, "AAC: faad open (heap=%lu)",
                 (unsigned long)esp_get_free_heap_size());
        s_faad = NeAACDecOpen();
        if (!s_faad) {
          ESP_LOGE(TAG, "faad open failed (no memory)");
          esp_http_client_close(client);
          esp_http_client_cleanup(client);
          client = NULL;
          s_active = false;
          continue;
        }
        {
          NeAACDecConfigurationPtr cfg =
              NeAACDecGetCurrentConfiguration(s_faad);
          cfg->defObjectType = LC;
          cfg->defSampleRate = 44100;
          cfg->outputFormat = FAAD_FMT_16BIT;
          cfg->downMatrix = 0;
          cfg->dontUpSampleImplicitSBR = 1;
          NeAACDecSetConfiguration(s_faad, cfg);
        }
        s_aac_open_rc = 1;
        s_faad_inited = false;
        memset(&s_aac_info, 0, sizeof(s_aac_info));
        s_aac_in_len = 0;
        s_aac_frames_total = 0;
        s_aac_feed_calls = 0;
        ESP_LOGI(TAG, "AAC (faad2) ready (heap=%lu)",
                 (unsigned long)esp_get_free_heap_size());
        dlna_cp(10); /* aac init done */
        /* Feed the sniffed bytes; they may already contain a frame start. */
        aac_feed(sniff, (size_t)got);
      } else {
        /* First chunk into the WAV parser. */
        size_t frames = 0;
        int r = wav_consume(&wav, sniff, (size_t)got, s_pcm_buf,
                            PCM_CHUNK_FRAMES * 2, &frames);
        if (r > 0 && frames > 0) {
          dlna_set_rate(wav.rate);
          s_channels = wav.channels;
          s_bits = wav.bits;
          feed_pcm(s_pcm_buf, frames);
          if (wav.data_total > 0) {
            double bps = (double)s_rate * s_channels * (double)(s_bits / 8);
            if (bps > 0) {
              s_duration = (double)wav.data_total / bps;
            }
          }
        }
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
      flac_stop();
      aac_stop();
      ogg_stop();
      alac_stop();
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
      if (fmt == FMT_FLAC) {
        /* dr_flac drives the wire: its onRead pulls HTTP directly. */
        if (!flac_pump(client)) {
          break;
        }
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "flac: frames=%llu",
                   (unsigned long long)s_flac_frames_total);
        }
        continue;
      }
      int64_t http_t0 = esp_timer_get_time();
      int n = esp_http_client_read(client, (char *)s_http_buf, HTTP_BUF_SIZE);
      int64_t http_read_us = esp_timer_get_time() - http_t0;
      if (n <= 0) {
        ESP_LOGI(TAG, "stream ended (read=%d)", n);
        break;
      }
      /* Diagnose slow HTTP pulls: if the server trickles data, read blocks for
       * a long time and the FIFO underruns regardless of decode speed. */
      if (s_read_calls < 10 || (s_read_calls % 100) == 0) {
        ESP_LOGI(TAG, "http: n=%d took=%lld ms",
                 n, (long long)(http_read_us / 1000));
      }
      if (fmt == FMT_WAV) {
        size_t frames = 0;
        int r = wav_consume(&wav, s_http_buf, (size_t)n, s_pcm_buf,
                            PCM_CHUNK_FRAMES * 2, &frames);
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "wav: n=%d r=%d frames=%u", n, r,
                   (unsigned)frames);
        }
        if (r < 0) {
          ESP_LOGE(TAG, "WAV parse error");
          break;
        }
        if (r > 0 && frames > 0) {
          feed_pcm(s_pcm_buf, frames);
        }
      } else if (fmt == FMT_MP3) {
        size_t f = mp3_feed(s_http_buf, (size_t)n);
        (void)f;
        dlna_cp(8); /* mp3_feed returned */
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "mp3: n=%d fed=%u", n, (unsigned)f);
        }
      } else if (fmt == FMT_OGG) {
        ogg_feed(s_http_buf, (size_t)n);
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "ogg: frames=%llu",
                   (unsigned long long)s_ogg_frames_total);
        }
      } else if (fmt == FMT_ALAC) {
        alac_feed(s_http_buf, (size_t)n);
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "alac: frames=%llu",
                   (unsigned long long)s_alac_frames_total);
        }
      } else if (fmt == FMT_AAC) {
        size_t f = aac_feed(s_http_buf, (size_t)n);
        (void)f;
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "aac: n=%d fed=%u", n, (unsigned)f);
        }
      }
    }
    s_stream_end = 1;

    flac_stop();
    aac_stop();
    ogg_stop();
    alac_stop();

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

/* 鈹€鈹€ public API 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

esp_err_t dlna_stream_init(void) {
  if (s_cmd_q) {
    return ESP_OK;
  }
  s_cmd_q = xQueueCreate(8, sizeof(stream_msg_t));
  if (!s_cmd_q) {
    return ESP_ERR_NO_MEM;
  }
  /* Task is created lazily on first play: the 8K task stack + 4K PCM buffer
   * must not compete with AirPlay service startup (ensure_audio_output was
   * failing with ESP_ERR_NO_MEM while this task existed at boot). */
  ESP_LOGI(TAG, "DLNA stream queue ready (task lazy-created on play)");
  return ESP_OK;
}

static esp_err_t ensure_stream_task(void) {
  if (s_task) {
    return ESP_OK;
  }
  if (!s_stream_stack) {
    s_stream_stack =
        heap_caps_malloc(STREAM_TASK_STACK * sizeof(StackType_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stream_stack) {
      ESP_LOGE(TAG, "PSRAM stream stack alloc failed (%u bytes)",
               (unsigned)(STREAM_TASK_STACK * sizeof(StackType_t)));
      return ESP_ERR_NO_MEM;
    }
  }
  s_task = xTaskCreateStaticPinnedToCore(
      stream_task, "dlna_stream", STREAM_TASK_STACK, NULL, STREAM_TASK_PRIO,
      s_stream_stack, &s_stream_tcb, 0);
  if (!s_task) {
    ESP_LOGE(TAG, "stream task static create FAILED");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "DLNA stream task started (lazy, PSRAM stack)");
  return ESP_OK;
}

esp_err_t dlna_stream_play(const char *uri) {
  dlna_cp(33); /* stream_play entry */
  if (!uri || !uri[0]) {
    s_play_last_err = ESP_ERR_INVALID_ARG;
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = ensure_stream_task();
  if (err != ESP_OK) {
    s_play_last_err = err;
    return err;
  }
  dlna_cp(34); /* task ensured */
  stream_msg_t msg;
  memset(&msg, 0, sizeof(msg));
  msg.cmd = CMD_PLAY;
  snprintf(msg.uri, sizeof(msg.uri), "%s", uri);
  if (xQueueSend(s_cmd_q, &msg, pdMS_TO_TICKS(200)) != pdPASS) {
    s_play_last_err = ESP_ERR_TIMEOUT;
    return ESP_ERR_TIMEOUT;
  }
  s_play_last_err = ESP_OK;
  return ESP_OK;
}

/* TEMP DIAG */
int dlna_stream_get_last_err(void) {
  return s_play_last_err;
}
bool dlna_stream_task_alive(void) {
  return s_task != NULL;
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
  }  double bytes_per_sec =
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

uint32_t dlna_stream_get_feed_count(void) { /* TEMP DIAG */
  return s_dlna_feed_calls;
}

/* TEMP DIAG */
int dlna_stream_get_http_status(void) { return s_http_status; }
int dlna_stream_get_http_err(void) { return s_http_err; }
uint32_t dlna_stream_get_mp3_frames(void) { return s_mp3_frames_total; }
uint32_t dlna_stream_get_mp3_feed_calls(void) { return s_mp3_feed_calls; }
uint64_t dlna_stream_get_flac_frames(void) { return s_flac_frames_total; }
uint64_t dlna_stream_get_flac_read_calls(void) { return s_flac_read_calls; }
int dlna_stream_get_fmt_diag(void) { return s_fmt_diag; }
int dlna_stream_get_flac_open_rc(void) { return s_flac_open_rc; }
uint64_t dlna_stream_get_aac_frames(void) { return s_aac_frames_total; }
int dlna_stream_get_aac_open_rc(void) { return s_aac_open_rc; }
uint32_t dlna_stream_get_aac_feed_calls(void) { return s_aac_feed_calls; }
uint64_t dlna_stream_get_ogg_frames(void) { return s_ogg_frames_total; }
uint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }
uint32_t dlna_stream_get_alac_feed_count(void) { return s_alac_feed_count; }
uint32_t dlna_stream_get_alac_stsz_count(void) { return s_alac_stsz_count; }
uint32_t dlna_stream_get_alac_stsz_idx(void) { return s_alac_stsz_idx; }
int dlna_stream_get_alac_open_rc(void) { return s_alac_open_rc; }
int dlna_stream_get_stream_end(void) { return s_stream_end; }