# -*- coding: utf-8 -*-
"""Expose stsz count/idx diagnostics."""
import io

base = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver'

p = base + r'\main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()
old = 'uint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }'
new = ('uint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }\n'
       'uint32_t dlna_stream_get_alac_stsz_count(void) { return s_alac_stsz_count; }\n'
       'uint32_t dlna_stream_get_alac_stsz_idx(void) { return s_alac_stsz_idx; }')
assert old in s
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)

h = base + r'\main\dlna\dlna_stream.h'
hs = io.open(h, encoding='utf-8').read()
hs = hs.replace(
    'int dlna_stream_get_alac_open_rc(void);',
    'int dlna_stream_get_alac_open_rc(void);\n'
    'uint32_t dlna_stream_get_alac_stsz_count(void);\n'
    'uint32_t dlna_stream_get_alac_stsz_idx(void);', 1)
io.open(h, 'w', encoding='utf-8', newline='').write(hs)

w = base + r'\main\network\web_server.c'
ws = io.open(w, encoding='utf-8').read()
old2 = '''    cJSON_AddNumberToObject(json, "dlna_alac_open_rc",'''
new2 = '''    cJSON_AddNumberToObject(json, "dlna_alac_stsz_count",
                            dlna_stream_get_alac_stsz_count());
    cJSON_AddNumberToObject(json, "dlna_alac_stsz_idx",
                            dlna_stream_get_alac_stsz_idx());
    cJSON_AddNumberToObject(json, "dlna_alac_open_rc",'''
assert old2 in ws
ws = ws.replace(old2, new2, 1)
io.open(w, 'w', encoding='utf-8', newline='').write(ws)
print('stsz diag added')
