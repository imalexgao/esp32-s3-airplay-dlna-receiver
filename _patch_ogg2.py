# -*- coding: utf-8 -*-
"""Replace OGG callbacks with pushdata feed mode in dlna_stream.c."""
import io

p = r'main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()

# 1. Replace the whole OGG callbacks block (from OGG marker to before AIFF marker)
start_marker = "/* ── OGG Vorbis (stb_vorbis, streaming callbacks) ─────────────────────────── */"
end_marker = "/* ── AIFF (big-endian PCM, streaming chunk parser) ──────────────────────── */"
i0 = s.index(start_marker)
i1 = s.index(end_marker, i0)
new_ogg = r"""/* ── OGG Vorbis (stb_vorbis pushdata streaming) ──────────────────────────── */
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
      s_rate = info.sample_rate ? (uint32_t)info.sample_rate : 44100;
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

"""
s = s[:i0] + new_ogg + s[i1:]

# 2. CMD_PLAY OGG branch: ogg_open -> ogg_stop+ogg_feed
old_open = """      } else if (fmt == FMT_OGG) {
        if (!ogg_open(client, sniff, (size_t)got)) {
          esp_http_client_close(client);
          esp_http_client_cleanup(client);
          client = NULL;
          s_active = false;
          continue;
        }
        dlna_cp(26); /* ogg opened */
      } else if (fmt == FMT_AIFF) {"""
new_open = """      } else if (fmt == FMT_OGG) {
        ogg_stop();
        s_ogg_frames_total = 0;
        s_ogg_open_rc = 0;
        ogg_feed(sniff, (size_t)got);
        dlna_cp(26); /* ogg fed */
      } else if (fmt == FMT_AIFF) {"""
assert old_open in s
s = s.replace(old_open, new_open, 1)

# 3. Streaming loop: OGG now goes through the HTTP read branch; remove the
#    dedicated pump branch, add OGG to the per-chunk dispatch.
old_pump = """      if (fmt == FMT_OGG) {
        /* stb_vorbis drives the wire through its read callback. */
        if (!ogg_pump(client)) {
          break;
        }
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "ogg: frames=%llu",
                   (unsigned long long)s_ogg_frames_total);
        }
        continue;
      }
"""
assert old_pump in s
s = s.replace(old_pump, "", 1)

old_aac_loop = """      } else if (fmt == FMT_AAC) {
        size_t f = aac_feed(s_http_buf, (size_t)n);
        (void)f;
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "aac: n=%d fed=%u", n, (unsigned)f);
        }
      }"""
new_aac_loop = """      } else if (fmt == FMT_OGG) {
        ogg_feed(s_http_buf, (size_t)n);
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "ogg: frames=%llu",
                   (unsigned long long)s_ogg_frames_total);
        }
      } else if (fmt == FMT_AAC) {
        size_t f = aac_feed(s_http_buf, (size_t)n);
        (void)f;
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "aac: n=%d fed=%u", n, (unsigned)f);
        }
      }"""
assert old_aac_loop in s
s = s.replace(old_aac_loop, new_aac_loop, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('OGG pushdata mode applied')
