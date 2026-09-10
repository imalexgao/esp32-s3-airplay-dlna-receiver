# -*- coding: utf-8 -*-
"""Fix ALAC M4A frame parsing: sample sizes come from stsz, not 4-byte prefix."""
import io

p = r'main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()

# 1. add stsz state vars after s_alac_open_rc
old = """static volatile uint64_t s_alac_frames_total = 0;
static volatile int s_alac_open_rc = -1;
"""
new = """static volatile uint64_t s_alac_frames_total = 0;
static volatile int s_alac_open_rc = -1;
static uint32_t s_alac_stsz_uniform = 0;
static uint32_t s_alac_stsz_count = 0;
static uint32_t s_alac_stsz_idx = 0;
static uint32_t *s_alac_stsz = NULL;
static uint32_t s_alac_stsz_cap = 0;
"""
assert old in s
s = s.replace(old, new, 1)

# 2. extend alac_parse_moov: also parse stsz for sample sizes
old = """  s_alac_cookie_len = 24;
  memcpy(s_alac_cookie, alac_atom + 8 + 4, 24);
  s_alac_frame_length = alac_be32(alac_atom + 8 + 4);
  if (s_alac_frame_length == 0 || s_alac_frame_length > 8192) {
    s_alac_frame_length = 4096;
  }
  return 0;
}
"""
new = """  s_alac_cookie_len = 24;
  memcpy(s_alac_cookie, alac_atom + 8 + 4, 24);
  s_alac_frame_length = alac_be32(alac_atom + 8 + 4);
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
"""
assert old in s
s = s.replace(old, new, 1)

# 3. rewrite the mdat frame-parsing part: use stsz sizes instead of 4-byte prefix
old = """      /* ---- inside mdat: ALAC frame stream (4-byte BE len + payload) ---- */
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
"""
new = """      /* ---- inside mdat: ALAC frames sized by stsz (no length prefix) ---- */
      if (s_alac_stsz_idx >= s_alac_stsz_count) {
        return; /* all frames consumed; ignore any trailing bytes */
      }
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
      s_alac_fbuf_len = 0;
      {
        size_t needf = s_alac_flen - s_alac_fbuf_len;
        size_t takef = (len - i < needf) ? len - i : needf;
        memcpy(s_alac_fbuf + s_alac_fbuf_len, buf + i, takef);
        s_alac_fbuf_len += (uint32_t)takef;
        i += takef;
        if (s_alac_fbuf_len < s_alac_flen) {
          break;
        }
      }
      s_alac_stsz_idx++;
"""
assert old in s
s = s.replace(old, new, 1)

# 4. alac_stop: reset stsz index too
old = """  s_alac_top = 0;
  s_alac_ah_off = 0;
  s_alac_have_moov = false;
  s_alac_moov_len = 0;
  s_alac_fhdr_off = 0;
  s_alac_fbuf_len = 0;
  s_alac_flen = 0;
}
"""
new = """  s_alac_top = 0;
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
"""
assert old in s
s = s.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('ALAC stsz-based frame parsing applied')
