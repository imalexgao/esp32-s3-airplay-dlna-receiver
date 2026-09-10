# -*- coding: utf-8 -*-
"""Integrate ALAC (M4A container) into dlna_stream.c."""
import io

p = r'main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()

# 1. enum: add FMT_ALAC after FMT_OGG
s = s.replace("  FMT_OGG,\n  FMT_UNKNOWN,\n",
              "  FMT_OGG,\n  FMT_ALAC,\n  FMT_UNKNOWN,\n", 1)

# 2. sniff: ftyp -> FMT_ALAC (after OggS)
s = s.replace("""  if (n >= 4 && memcmp(h, "OggS", 4) == 0) {
    return FMT_OGG;
  }
""",
"""  if (n >= 4 && memcmp(h, "OggS", 4) == 0) {
    return FMT_OGG;
  }
  if (n >= 12 && memcmp(h + 4, "ftyp", 4) == 0) {
    return FMT_ALAC; /* M4A container (ALAC expected; validated on open) */
  }
""", 1)

# 3. insert the ALAC block before "static void stream_task"
alac_block = r"""/* ── ALAC (Apple Lossless, M4A container) ──────────────────────────────── */
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
static volatile int s_alac_open_rc = -1;

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
        memcmp(pp + off + 4, "stsd", 4) == 0 ||
        memcmp(pp + off + 4, "mp4a", 4) == 0) {
      if (alac_find_atom(pp + off + 8, asz - 8, want, out, out_len) == 0) {
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
  /* atom: size(4) type(4) version/flags(4) config(24) */
  if (alac_len < 8 + 4 + 24) {
    ESP_LOGE(TAG, "ALAC: alac atom too small");
    return -1;
  }
  s_alac_cookie_len = 24;
  memcpy(s_alac_cookie, alac_atom + 8 + 4, 24);
  s_alac_frame_length = alac_be32(alac_atom + 8 + 4);
  if (s_alac_frame_length == 0 || s_alac_frame_length > 8192) {
    s_alac_frame_length = 4096;
  }
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
}

static void alac_feed(const uint8_t *buf, size_t len) {
  size_t i = 0;
  while (i < len) {
    if (s_alac_top == 3) {
      /* ---- inside mdat: ALAC frame stream (4-byte BE len + payload) ---- */
      if (s_alac_fhdr_off < 4) {
        size_t need = 4 - s_alac_fhdr_off;
        size_t take = (len - i < need) ? len - i : need;
        memcpy(s_alac_fhdr + s_alac_fhdr_off, buf + i, take);
        s_alac_fhdr_off += take;
        i += take;
        if (s_alac_fhdr_off < 4) {
          break;
        }
        s_alac_flen = alac_be32(s_alac_fhdr);
        if (s_alac_flen == 0 || s_alac_flen > 1024 * 1024) {
          ESP_LOGE(TAG, "ALAC: bad frame len %u", s_alac_flen);
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
        s_alac_fbuf_len = 0;
      }
      size_t needf = s_alac_flen - s_alac_fbuf_len;
      size_t takef = (len - i < needf) ? len - i : needf;
      memcpy(s_alac_fbuf + s_alac_fbuf_len, buf + i, takef);
      s_alac_fbuf_len += (uint32_t)takef;
      i += takef;
      if (s_alac_fbuf_len < s_alac_flen) {
        break;
      }
      uint32_t outn = 0;
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
          return;
        }
        s_alac_open_rc = 1;
        alac_dec_get_info(s_alac, &s_alac_rate, &s_alac_channels,
                          &s_alac_bits);
        s_rate = s_alac_rate ? s_alac_rate : 44100;
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

"""
anchor = "static void stream_task(void *arg) {"
s = s.replace(anchor, alac_block + anchor, 1)

# 4. CMD_PLAY ALAC branch (after OGG branch)
old = """      } else if (fmt == FMT_OGG) {
        ogg_stop();
        s_ogg_frames_total = 0;
        s_ogg_open_rc = 0;
        ogg_feed(sniff, (size_t)got);
        dlna_cp(26); /* ogg fed */
      } else if (fmt == FMT_AAC) {"""
new = """      } else if (fmt == FMT_OGG) {
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
      } else if (fmt == FMT_AAC) {"""
assert old in s
s = s.replace(old, new, 1)

# 5. stream loop: add ALAC branch after OGG
old = """      } else if (fmt == FMT_OGG) {
        ogg_feed(s_http_buf, (size_t)n);
        if ((s_read_calls++ % 100) == 0) {
          ESP_LOGI(TAG, "ogg: frames=%llu",
                   (unsigned long long)s_ogg_frames_total);
        }
      } else if (fmt == FMT_AAC) {"""
new = """      } else if (fmt == FMT_OGG) {
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
      } else if (fmt == FMT_AAC) {"""
assert old in s
s = s.replace(old, new, 1)

# 6. cleanup points: add alac_stop()
s = s.replace("""      flac_stop();
      aac_stop();
      ogg_stop();
      in_stream = false;""",
"""      flac_stop();
      aac_stop();
      ogg_stop();
      alac_stop();
      in_stream = false;""", 1)
s = s.replace("""    flac_stop();
    aac_stop();
    ogg_stop();

    if (!in_stream && client) {""",
"""    flac_stop();
    aac_stop();
    ogg_stop();
    alac_stop();

    if (!in_stream && client) {""", 1)

# 7. getter
s = s.replace("uint64_t dlna_stream_get_aiff_frames(void) { return s_aiff_frames_total; }\n", "", 1)  # no-op safety
s = s.replace("uint64_t dlna_stream_get_ogg_frames(void) { return s_ogg_frames_total; }",
              "uint64_t dlna_stream_get_ogg_frames(void) { return s_ogg_frames_total; }\nuint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }", 1)

# 8. format log line
s = s.replace('''               : fmt == FMT_OGG ? "OGG" : "UNKNOWN");''',
'''               : fmt == FMT_OGG ? "OGG" : fmt == FMT_ALAC ? "ALAC/M4A"
                                                          : "UNKNOWN");''', 1)
s = s.replace('''      if (fmt != FMT_WAV && fmt != FMT_MP3 && fmt != FMT_FLAC &&
          fmt != FMT_AAC && fmt != FMT_OGG) {
        ESP_LOGE(TAG, "format not supported yet (WAV/MP3/FLAC/AAC/OGG)");''',
'''      if (fmt != FMT_WAV && fmt != FMT_MP3 && fmt != FMT_FLAC &&
          fmt != FMT_AAC && fmt != FMT_OGG && fmt != FMT_ALAC) {
        ESP_LOGE(TAG, "format not supported yet (WAV/MP3/FLAC/AAC/OGG/ALAC)");''', 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('dlna_stream.c ALAC integrated')

# header
h = r'main\dlna\dlna_stream.h'
hs = io.open(h, encoding='utf-8').read()
if 'dlna_stream_get_alac_frames' not in hs:
    hs = hs.replace("uint64_t dlna_stream_get_ogg_frames(void);",
                    "uint64_t dlna_stream_get_ogg_frames(void);\nuint64_t dlna_stream_get_alac_frames(void);", 1)
io.open(h, 'w', encoding='utf-8', newline='').write(hs)
print('dlna_stream.h updated')

# web_server
w = r'main\network\web_server.c'
ws = io.open(w, encoding='utf-8').read()
if 'dlna_alac_frames' not in ws:
    ws = ws.replace('''    cJSON_AddNumberToObject(json, "dlna_ogg_frames",
                            dlna_stream_get_ogg_frames());''',
'''    cJSON_AddNumberToObject(json, "dlna_ogg_frames",
                            dlna_stream_get_ogg_frames());
    cJSON_AddNumberToObject(json, "dlna_alac_frames",
                            dlna_stream_get_alac_frames());''', 1)
io.open(w, 'w', encoding='utf-8', newline='').write(ws)
print('web_server.c updated')

# UPnP SinkProtocolInfo: add ALAC
u = r'main\dlna\dlna_upnp.c'
us = io.open(u, encoding='utf-8').read()
if 'audio/x-alac' not in us:
    us = us.replace('  "http-get:*:audio/ogg:DLNA.ORG_PN=OGG"',
                    '  "http-get:*:audio/ogg:DLNA.ORG_PN=OGG," \\\n  "http-get:*:audio/x-alac:DLNA.ORG_PN=ALAC"', 1)
io.open(u, 'w', encoding='utf-8', newline='').write(us)
print('dlna_upnp.c SinkProtocolInfo updated')
