# -*- coding: utf-8 -*-
"""Remove AIFF support (user decision: keep OGG, drop AIFF)."""
import io

base = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver'
p = base + r'\main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()

# 1. Remove the whole AIFF block (aiff_ctx_t .. aiff_consume end)
start = "/* ── AIFF (big-endian PCM, streaming chunk parser) ──────────────────────── */"
i0 = s.index(start)
# end at the closing brace right before "static void stream_task"
i1 = s.index("static void stream_task(void *arg) {", i0)
# trim to end of aiff_consume block (the line before stream_task comment)
i1 = s.rindex("\n", 0, i1) + 1
s = s[:i0] + s[i1:]

# 2. enum: drop FMT_AIFF
s = s.replace("  FMT_OGG,\n  FMT_AIFF,\n  FMT_UNKNOWN,\n",
              "  FMT_OGG,\n  FMT_UNKNOWN,\n", 1)

# 3. sniff: drop FORM/AIFF detection
s = s.replace("""  if (n >= 12 && memcmp(h, "FORM", 4) == 0 && memcmp(h + 8, "AIFF", 4) == 0) {
    return FMT_AIFF;
  }
""", "", 1)

# 4. log lines
s = s.replace('''               : fmt == FMT_OGG ? "OGG" : fmt == FMT_AIFF ? "AIFF"
                                                          : "UNKNOWN");''',
'''               : fmt == FMT_OGG ? "OGG" : "UNKNOWN");''', 1)
s = s.replace('''      if (fmt != FMT_WAV && fmt != FMT_MP3 && fmt != FMT_FLAC &&
          fmt != FMT_AAC && fmt != FMT_OGG && fmt != FMT_AIFF) {
        ESP_LOGE(TAG, "format not supported yet (WAV/MP3/FLAC/AAC/OGG/AIFF)");''',
'''      if (fmt != FMT_WAV && fmt != FMT_MP3 && fmt != FMT_FLAC &&
          fmt != FMT_AAC && fmt != FMT_OGG) {
        ESP_LOGE(TAG, "format not supported yet (WAV/MP3/FLAC/AAC/OGG)");''', 1)

# 5. stream_task local var
s = s.replace("  wav_ctx_t wav;\n  aiff_ctx_t aiff;\n", "  wav_ctx_t wav;\n", 1)

# 6. CMD_PLAY AIFF branch
s = s.replace('''      } else if (fmt == FMT_AIFF) {
        memset(&aiff, 0, sizeof(aiff));
        s_aiff_frames_total = 0;
        size_t frames = 0;
        int r = aiff_consume(&aiff, sniff, (size_t)got, s_pcm_buf,
                             PCM_CHUNK_FRAMES * 2, &frames);
        if (r > 0 && frames > 0) {
          s_rate = aiff.rate;
          s_channels = aiff.channels;
          s_bits = aiff.bits;
          s_aiff_frames_total += frames;
          feed_pcm(s_pcm_buf, frames);
        }
        dlna_cp(27); /* aiff init done */
      } else if (fmt == FMT_AAC) {''',
'''      } else if (fmt == FMT_AAC) {''', 1)

# 7. stream loop AIFF branch
s = s.replace('''      } else if (fmt == FMT_AIFF) {
        size_t frames = 0;
        int r = aiff_consume(&aiff, s_http_buf, (size_t)n, s_pcm_buf,
                             PCM_CHUNK_FRAMES * 2, &frames);
        if (r < 0) {
          ESP_LOGE(TAG, "AIFF parse error");
          break;
        }
        if (r > 0 && frames > 0) {
          s_aiff_frames_total += frames;
          feed_pcm(s_pcm_buf, frames);
        }
      } else if (fmt == FMT_OGG) {''',
'''      } else if (fmt == FMT_OGG) {''', 1)

# 8. counter var
s = s.replace("static volatile uint64_t s_aiff_frames_total = 0;\n", "", 1)

# 9. getter
s = s.replace("uint64_t dlna_stream_get_aiff_frames(void) { return s_aiff_frames_total; }\n", "", 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('dlna_stream.c AIFF removed')

# 10. header
h = base + r'\main\dlna\dlna_stream.h'
hs = io.open(h, encoding='utf-8').read()
hs = hs.replace("uint64_t dlna_stream_get_aiff_frames(void);\n", "", 1)
io.open(h, 'w', encoding='utf-8', newline='').write(hs)
print('dlna_stream.h cleaned')

# 11. web_server
w = base + r'\main\network\web_server.c'
ws = io.open(w, encoding='utf-8').read()
ws = ws.replace("""    cJSON_AddNumberToObject(json, "dlna_aiff_frames",
                            dlna_stream_get_aiff_frames());
""", "", 1)
io.open(w, 'w', encoding='utf-8', newline='').write(ws)
print('web_server.c cleaned')

# 12. UPnP SinkProtocolInfo: drop audio/x-aiff
u = base + r'\main\dlna\dlna_upnp.c'
us = io.open(u, encoding='utf-8').read()
import re
us2 = re.sub(r',\s*audio/x-aiff', '', us)
if us2 != us:
    us = us2
    io.open(u, 'w', encoding='utf-8', newline='').write(us)
    print('dlna_upnp.c cleaned')
else:
    print('dlna_upnp.c: audio/x-aiff not found (check manually)')

# verify no AIFF remains in stream.c
left = [ln for ln in io.open(p, encoding='utf-8').read().splitlines() if 'AIFF' in ln or 'aiff' in ln]
print('remaining AIFF refs in dlna_stream.c:', left if left else 'none')
