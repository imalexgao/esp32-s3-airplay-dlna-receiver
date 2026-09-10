# -*- coding: utf-8 -*-
"""Expose dlna_alac_open_rc."""
import io

base = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver'

p = base + r'\main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()
s = s.replace(
    'uint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }',
    'uint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }\n'
    'int dlna_stream_get_alac_open_rc(void) { return s_alac_open_rc; }', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)

h = base + r'\main\dlna\dlna_stream.h'
hs = io.open(h, encoding='utf-8').read()
hs = hs.replace(
    'uint64_t dlna_stream_get_alac_frames(void);',
    'uint64_t dlna_stream_get_alac_frames(void);\n'
    'int dlna_stream_get_alac_open_rc(void);', 1)
io.open(h, 'w', encoding='utf-8', newline='').write(hs)

w = base + r'\main\network\web_server.c'
ws = io.open(w, encoding='utf-8').read()
ws = ws.replace(
    '''    cJSON_AddNumberToObject(json, "dlna_alac_frames",
                            dlna_stream_get_alac_frames());''',
    '''    cJSON_AddNumberToObject(json, "dlna_alac_frames",
                            dlna_stream_get_alac_frames());
    cJSON_AddNumberToObject(json, "dlna_alac_open_rc",
                            dlna_stream_get_alac_open_rc());''', 1)
io.open(w, 'w', encoding='utf-8', newline='').write(ws)
print('open_rc exposed')
