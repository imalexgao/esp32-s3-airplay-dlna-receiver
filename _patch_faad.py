# -*- coding: utf-8 -*-
"""Replace helix AAC path with FAAD2 in dlna_stream.c (v2, clean consume)."""
import io

p = r'main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()

# 1. include faad2 header
old_inc = '#include "codecs/libhelix-aac/aacdec.h"'
new_inc = '#include "codecs/libhelix-aac/aacdec.h"\n#include "codecs/libfaad2/include/neaacdec.h"'
assert old_inc in s
s = s.replace(old_inc, new_inc, 1)

# 2. declarations
old_dec = "static HAACDecoder s_aac = NULL;"
new_dec = "static NeAACDecHandle s_faad = NULL;\nstatic bool s_faad_inited = false;"
assert old_dec in s
s = s.replace(old_dec, new_dec, 1)

# 3. aac_stop
old_stop = """static void aac_stop(void) {
  if (s_aac) {
    AACFreeDecoder(s_aac);
    s_aac = NULL;
  }
  s_aac_in_len = 0;
}"""
new_stop = """static void aac_stop(void) {
  if (s_faad) {
    NeAACDecClose(s_faad);
    s_faad = NULL;
  }
  s_faad_inited = false;
  s_aac_in_len = 0;
}"""
assert old_stop in s
s = s.replace(old_stop, new_stop, 1)

# 4. aac_feed decode loop
start_marker = "    unsigned char *in = s_aac_in;"
end_marker = "    memmove(s_aac_in, s_aac_in + consumed, s_aac_in_len - consumed);\n    s_aac_in_len -= consumed;"
i0 = s.index(start_marker)
i1 = s.index(end_marker, i0) + len(end_marker)
new_block = """    dlna_cp(11); /* before faad decode */
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
        if (f_rate > 0) s_rate = f_rate;
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
          if (fi.samplerate > 0) s_rate = fi.samplerate;
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
    }"""
s = s[:i0] + new_block + s[i1:]

# 5. CMD_PLAY AAC init
old_cmd = """        aac_stop();
        ESP_LOGI(TAG, "AAC: heap before init=%lu",
                 (unsigned long)esp_get_free_heap_size());
        s_aac = AACInitDecoder();
        s_aac_open_rc = s_aac ? 1 : 0;
        ESP_LOGI(TAG, "AAC: init rc=%d heap=%lu",
                 s_aac_open_rc, (unsigned long)esp_get_free_heap_size());
        if (!s_aac) {
          ESP_LOGE(TAG, "AAC init failed (no memory)");
          esp_http_client_close(client);
          esp_http_client_cleanup(client);
          client = NULL;
          s_active = false;
          continue;
        }
        memset(&s_aac_info, 0, sizeof(s_aac_info));
        s_aac_in_len = 0;
        s_aac_frames_total = 0;
        s_aac_feed_calls = 0;
        ESP_LOGI(TAG, "AAC decoder ready (heap=%lu)",
                 (unsigned long)esp_get_free_heap_size());
        dlna_cp(10); /* aac init done */
        /* Feed the sniffed bytes; they may already contain a frame start. */
        aac_feed(sniff, (size_t)got);"""
new_cmd = """        aac_stop();
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
        aac_feed(sniff, (size_t)got);"""
assert old_cmd in s
s = s.replace(old_cmd, new_cmd, 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('faad integration done')
