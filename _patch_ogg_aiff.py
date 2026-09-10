# -*- coding: utf-8 -*-
"""Add OGG Vorbis (stb_vorbis) + AIFF support to dlna_stream.c."""
import io

p = r'main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()

# 1. include stb_vorbis (declarations only; impl compiled from codecs/stb_vorbis.c)
old_inc = '#include "codecs/libfaad2/include/neaacdec.h"'
new_inc = ('#include "codecs/libfaad2/include/neaacdec.h"\n'
           '#define STB_VORBIS_HEADER_ONLY\n'
           '#include "codecs/stb_vorbis.c"')
assert old_inc in s
s = s.replace(old_inc, new_inc, 1)

# 2. enum
old_enum = """  FMT_WAV = 0,
  FMT_MP3,
  FMT_AAC,
  FMT_FLAC,
  FMT_UNKNOWN,"""
new_enum = """  FMT_WAV = 0,
  FMT_MP3,
  FMT_AAC,
  FMT_FLAC,
  FMT_OGG,
  FMT_AIFF,
  FMT_UNKNOWN,"""
assert old_enum in s
s = s.replace(old_enum, new_enum, 1)

# 3. sniff
old_sniff = """  if (n >= 4 && memcmp(h, "fLaC", 4) == 0) {
    return FMT_FLAC;
  }
  if (n >= 2 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) {"""
new_sniff = """  if (n >= 4 && memcmp(h, "fLaC", 4) == 0) {
    return FMT_FLAC;
  }
  if (n >= 4 && memcmp(h, "OggS", 4) == 0) {
    return FMT_OGG;
  }
  if (n >= 12 && memcmp(h, "FORM", 4) == 0 && memcmp(h + 8, "AIFF", 4) == 0) {
    return FMT_AIFF;
  }
  if (n >= 2 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) {"""
assert old_sniff in s
s = s.replace(old_sniff, new_sniff, 1)

# 4. OGG + AIFF helpers: insert after aac_feed() end (before stream_task)
anchor = "static void stream_task(void *arg) {"
ogg_block = r"""/* ── OGG Vorbis (stb_vorbis, streaming callbacks) ─────────────────────────── */
#define OGG_IN_CAP 8192
static uint8_t s_ogg_in[OGG_IN_CAP];
static size_t s_ogg_in_len = 0;
static size_t s_ogg_in_pos = 0;
static size_t s_ogg_tell = 0;
static stb_vorbis *s_ogg = NULL;
static volatile uint64_t s_ogg_frames_total = 0;
static volatile int s_ogg_open_rc = -1;

static unsigned int ogg_on_read(void *opaque, char *buf, int len) {
  esp_http_client_handle_t hc = (esp_http_client_handle_t)opaque;
  size_t got = 0;
  while (got < (size_t)len) {
    if (s_ogg_in_pos < s_ogg_in_len) {
      size_t take = s_ogg_in_len - s_ogg_in_pos;
      if (take > (size_t)len - got) {
        take = (size_t)len - got;
      }
      memcpy(buf + got, s_ogg_in + s_ogg_in_pos, take);
      s_ogg_in_pos += take;
      got += take;
    } else {
      if (!hc) {
        break;
      }
      int n = esp_http_client_read(hc, (char *)s_ogg_in, sizeof(s_ogg_in));
      if (n <= 0) {
        break; /* HTTP EOF */
      }
      s_ogg_in_len = (size_t)n;
      s_ogg_in_pos = 0;
    }
  }
  s_ogg_tell += got;
  return (unsigned int)got;
}

static void ogg_on_skip(void *opaque, int n) {
  char tmp[256];
  while (n > 0) {
    int c = n > (int)sizeof(tmp) ? (int)sizeof(tmp) : n;
    unsigned int r = ogg_on_read(opaque, tmp, c);
    if (r == 0) {
      break;
    }
    n -= (int)r;
  }
}

static int ogg_on_seek(void *opaque, int offset) {
  (void)opaque;
  (void)offset;
  return 0; /* streaming: seek unsupported */
}

static int ogg_on_tell(void *opaque) {
  (void)opaque;
  return (int)s_ogg_tell;
}

static void ogg_stop(void) {
  if (s_ogg) {
    stb_vorbis_close(s_ogg);
    s_ogg = NULL;
  }
  s_ogg_in_len = 0;
  s_ogg_in_pos = 0;
}

static bool ogg_open(esp_http_client_handle_t hc, const uint8_t *sniff,
                     size_t sniff_len) {
  ogg_stop();
  memcpy(s_ogg_in, sniff, sniff_len);
  s_ogg_in_len = sniff_len;
  s_ogg_in_pos = 0;
  s_ogg_tell = sniff_len;
  s_ogg_frames_total = 0;
  static stb_vorbis_callbacks cb = {ogg_on_read, ogg_on_skip, ogg_on_seek,
                                    ogg_on_tell};
  int err = 0;
  s_ogg = stb_vorbis_open_callbacks(&cb, hc, NULL, &err, NULL, 0);
  s_ogg_open_rc = s_ogg ? 1 : 0;
  if (!s_ogg) {
    ESP_LOGE(TAG, "OGG open failed (err=%d)", err);
    return false;
  }
  stb_vorbis_info info = stb_vorbis_get_info(s_ogg);
  s_rate = info.sample_rate ? (uint32_t)info.sample_rate : 44100;
  s_channels = (uint32_t)info.channels;
  s_bits = 16;
  ESP_LOGI(TAG, "OGG opened: %u Hz %u ch", s_rate, s_channels);
  return true;
}

/* Decode one Vorbis frame; false at EOF/error. Float->s16 interleave into
 * s_pcm_buf in PCM_CHUNK_FRAMES chunks (Vorbis blocks can exceed it). */
static bool ogg_pump(esp_http_client_handle_t hc) {
  int ch = 0;
  float **outs = NULL;
  int frames = stb_vorbis_get_frame_float(s_ogg, &ch, &outs);
  if (frames <= 0 || !outs) {
    ESP_LOGI(TAG, "OGG end (frames=%d)", frames);
    return false;
  }
  if (ch < 1) {
    ch = 1;
  }
  if (ch > 2) {
    ch = 2; /* keep only first two channels */
  }
  int done = 0;
  while (done < frames) {
    int chunk = frames - done;
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
  return true;
}

/* ── AIFF (big-endian PCM, streaming chunk parser) ──────────────────────── */
typedef struct {
  bool have_comm;
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  uint64_t data_remaining;
  uint64_t data_total;
  uint64_t data_pos;
} aiff_ctx_t;

/* 80-bit IEEE 754 extended float -> double (AIFF sample rate). */
static double aiff_extended80(const uint8_t *p) {
  int sign = (p[0] & 0x80) ? -1 : 1;
  int exp = ((p[0] & 0x7F) << 8) | p[1];
  uint64_t mant = 0;
  for (int i = 0; i < 8; i++) {
    mant = (mant << 8) | p[2 + i];
  }
  if (exp == 0 && mant == 0) {
    return 0.0;
  }
  return sign * (1.0 + (double)mant / 9223372036854775808.0) *
         ldexp(1.0, exp - 16383);
}

static int aiff_consume(aiff_ctx_t *ctx, const uint8_t *buf, size_t len,
                        int16_t *pcm, size_t pcm_cap, size_t *pcm_frames) {
  static uint8_t header[8];
  static size_t header_off = 0;
  static uint32_t chunk_len = 0;
  static uint32_t chunk_kind = 0; /* 0 none, 1 comm, 2 ssnd, 3 skip */
  static uint32_t skip_remaining = 0;
  static size_t comm_off = 0;
  static uint8_t comm_buf[18];
  static uint8_t ssnd_hdr[8];
  static size_t ssnd_hdr_off = 0;

  size_t i = 0;
  *pcm_frames = 0;
  while (i < len) {
    if (chunk_kind == 0) {
      size_t need = 8 - header_off;
      size_t take = len - i < need ? len - i : need;
      memcpy(header + header_off, buf + i, take);
      header_off += take;
      i += take;
      if (header_off == 8) {
        uint32_t id = ((uint32_t)header[0] << 24) |
                      ((uint32_t)header[1] << 16) |
                      ((uint32_t)header[2] << 8) | header[3];
        chunk_len = ((uint32_t)header[4] << 24) |
                    ((uint32_t)header[5] << 16) |
                    ((uint32_t)header[6] << 8) | header[7];
        header_off = 0;
        if (id == 0x434F4D4D) { /* 'COMM' */
          chunk_kind = 1;
          comm_off = 0;
          ctx->have_comm = false;
        } else if (id == 0x53534E44) { /* 'SSND' */
          chunk_kind = 2;
          ctx->data_remaining = chunk_len;
          ctx->data_total = chunk_len;
          ssnd_hdr_off = 0;
          if (!ctx->have_comm) {
            ESP_LOGW(TAG, "AIFF SSND before COMM; assuming 44.1k/16/2");
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
      /* COMM body: channels(2) numFrames(4) sampleSize(2) rate(10) */
      size_t need = 18 - comm_off;
      size_t take = len - i < need ? len - i : need;
      memcpy(comm_buf + comm_off, buf + i, take);
      comm_off += take;
      i += take;
      if (comm_off == 18) {
        ctx->channels = (uint16_t)((comm_buf[0] << 8) | comm_buf[1]);
        ctx->bits = (uint16_t)((comm_buf[8] << 8) | comm_buf[9]);
        ctx->rate = (uint32_t)(aiff_extended80(comm_buf + 10) + 0.5);
        if (ctx->channels == 0) {
          ctx->channels = 2;
        }
        if (ctx->bits != 16 && ctx->bits != 24) {
          ESP_LOGE(TAG, "AIFF bit depth %u unsupported (16/24)", ctx->bits);
          return -1;
        }
        ctx->have_comm = true;
        skip_remaining = chunk_len > 18 ? chunk_len - 18 : 0;
        chunk_kind = 3;
      }
      continue;
    }
    if (chunk_kind == 2) {
      /* SSND: 8-byte offset/blockSize header, then big-endian PCM */
      if (ssnd_hdr_off < 8) {
        size_t need = 8 - ssnd_hdr_off;
        size_t take = len - i < need ? len - i : need;
        memcpy(ssnd_hdr + ssnd_hdr_off, buf + i, take);
        ssnd_hdr_off += take;
        i += take;
        if (ssnd_hdr_off < 8) {
          continue;
        }
        ctx->data_remaining =
            ctx->data_remaining > 8 ? ctx->data_remaining - 8 : 0;
      }
      size_t avail = (size_t)ctx->data_remaining;
      if (avail > len - i) {
        avail = len - i;
      }
      if (avail == 0) {
        chunk_kind = 0;
        continue;
      }
      const uint8_t *p = buf + i;
      size_t frame_bytes = (size_t)ctx->channels * (ctx->bits / 8);
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
          for (size_t f = 0; f < frames_in * ctx->channels; f++) {
            pcm[f] = (int16_t)((p[f * 2] << 8) | p[f * 2 + 1]);
          }
        } else { /* 24-bit BE -> int16 (high bits) */
          for (size_t f = 0; f < frames_in * ctx->channels; f++) {
            int32_t v = ((int32_t)p[f * 3] << 16) |
                        ((int32_t)p[f * 3 + 1] << 8) | p[f * 3 + 2];
            pcm[f] = (int16_t)(v >> 8);
          }
        }
        *pcm_frames = frames_in;
        size_t consumed = frames_in * frame_bytes;
        ctx->data_pos += consumed;
        ctx->data_remaining -= consumed;
        i += consumed;
        return 1;
      }
      i += avail;
      ctx->data_remaining -= avail;
      if (ctx->data_remaining == 0) {
        chunk_kind = 0;
      }
      continue;
    }
    if (chunk_kind == 3) {
      size_t take = len - i < skip_remaining ? len - i : skip_remaining;
      i += take;
      skip_remaining -= (uint32_t)take;
      if (skip_remaining == 0) {
        chunk_kind = 0;
      }
    }
  }
  return 0;
}

"""
assert anchor in s
s = s.replace(anchor, ogg_block + anchor, 1)

# 5. stream_task: aiff_ctx_t local
old_var = "  wav_ctx_t wav;\n  stream_msg_t msg;"
new_var = "  wav_ctx_t wav;\n  aiff_ctx_t aiff;\n  stream_msg_t msg;"
assert old_var in s
s = s.replace(old_var, new_var, 1)

# 6. format name print
old_fmt = """  ESP_LOGI(TAG, "stream format: %s",
               fmt == FMT_WAV ? "WAV" : fmt == FMT_MP3 ? "MP3"
               : fmt == FMT_AAC ? "AAC" : fmt == FMT_FLAC ? "FLAC"
                                                          : "UNKNOWN");
      if (fmt != FMT_WAV && fmt != FMT_MP3 && fmt != FMT_FLAC &&
          fmt != FMT_AAC) {
        ESP_LOGE(TAG, "format not supported yet (WAV/MP3/FLAC/AAC)");"""
new_fmt = """  ESP_LOGI(TAG, "stream format: %s",
               fmt == FMT_WAV ? "WAV" : fmt == FMT_MP3 ? "MP3"
               : fmt == FMT_AAC ? "AAC" : fmt == FMT_FLAC ? "FLAC"
               : fmt == FMT_OGG ? "OGG" : fmt == FMT_AIFF ? "AIFF"
                                                          : "UNKNOWN");
      if (fmt != FMT_WAV && fmt != FMT_MP3 && fmt != FMT_FLAC &&
          fmt != FMT_AAC && fmt != FMT_OGG && fmt != FMT_AIFF) {
        ESP_LOGE(TAG, "format not supported yet (WAV/MP3/FLAC/AAC/OGG/AIFF)");"""
assert old_fmt in s
s = s.replace(old_fmt, new_fmt, 1)

# 7. CMD_PLAY: OGG/AIFF branches after FLAC branch
old_aac = """      } else if (fmt == FMT_AAC) {"""
new_ogg_aiff = """      } else if (fmt == FMT_OGG) {
        if (!ogg_open(client, sniff, (size_t)got)) {
          esp_http_client_close(client);
          esp_http_client_cleanup(client);
          client = NULL;
          s_active = false;
          continue;
        }
        dlna_cp(26); /* ogg opened */
      } else if (fmt == FMT_AIFF) {
        memset(&aiff, 0, sizeof(aiff));
        size_t frames = 0;
        int r = aiff_consume(&aiff, sniff, (size_t)got, s_pcm_buf,
                             PCM_CHUNK_FRAMES * 2, &frames);
        if (r > 0 && frames > 0) {
          s_rate = aiff.rate;
          s_channels = aiff.channels;
          s_bits = aiff.bits;
          feed_pcm(s_pcm_buf, frames);
        }
        dlna_cp(27); /* aiff init done */
      } else if (fmt == FMT_AAC) {"""
assert old_aac in s
s = s.replace(old_aac, new_ogg_aiff, 1)

# 8. streaming loop: OGG pump + AIFF consume
old_loop = """      if (fmt == FMT_FLAC) {
        /* dr_flac drives the wire: its onRead pulls HTTP directly. */
        if (!flac_pump(client)) {
          break;
        }
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "flac: frames=%llu",
                   (unsigned long long)s_flac_frames_total);
        }
        continue;
      }"""
new_loop = """      if (fmt == FMT_FLAC) {
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
      if (fmt == FMT_OGG) {
        /* stb_vorbis drives the wire through its read callback. */
        if (!ogg_pump(client)) {
          break;
        }
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "ogg: frames=%llu",
                   (unsigned long long)s_ogg_frames_total);
        }
        continue;
      }"""
assert old_loop in s
s = s.replace(old_loop, new_loop, 1)

old_aac_loop = """      } else if (fmt == FMT_AAC) {
        size_t f = aac_feed(s_http_buf, (size_t)n);
        (void)f;
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "aac: n=%d fed=%u", n, (unsigned)f);
        }
      }"""
new_aiff_loop = """      } else if (fmt == FMT_AIFF) {
        size_t frames = 0;
        int r = aiff_consume(&aiff, s_http_buf, (size_t)n, s_pcm_buf,
                             PCM_CHUNK_FRAMES * 2, &frames);
        if (r < 0) {
          ESP_LOGE(TAG, "AIFF parse error");
          break;
        }
        if (r > 0 && frames > 0) {
          feed_pcm(s_pcm_buf, frames);
        }
      } else if (fmt == FMT_AAC) {
        size_t f = aac_feed(s_http_buf, (size_t)n);
        (void)f;
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "aac: n=%d fed=%u", n, (unsigned)f);
        }
      }"""
assert old_aac_loop in s
s = s.replace(old_aac_loop, new_aiff_loop, 1)

# 9. STOP: ogg_stop
old_stop = """      flac_stop();
      aac_stop();
      in_stream = false;
      s_active = false;
      s_paused = false;
      s_duration = 0.0;
      ESP_LOGI(TAG, "DLNA stream stopped");"""
new_stop = """      flac_stop();
      aac_stop();
      ogg_stop();
      in_stream = false;
      s_active = false;
      s_paused = false;
      s_duration = 0.0;
      ESP_LOGI(TAG, "DLNA stream stopped");"""
assert old_stop in s
s = s.replace(old_stop, new_stop, 1)

# 10. post-stream cleanup
old_clean = """    flac_stop();
    aac_stop();

    if (!in_stream && client) {"""
new_clean = """    flac_stop();
    aac_stop();
    ogg_stop();

    if (!in_stream && client) {"""
assert old_clean in s
s = s.replace(old_clean, new_clean, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('OGG+AIFF added to dlna_stream.c')
