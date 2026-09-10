# -*- coding: utf-8 -*-
"""Wire ALAC init diagnostics into dlna_stream.c + web_server.c."""
import io

base = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver'

p = base + r'\main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()
old = """        s_alac = alac_dec_create(s_alac_cookie, s_alac_cookie_len);
        if (!s_alac) {
          s_alac_open_rc = -9;
          return;
        }"""
new = """        s_alac = alac_dec_create(s_alac_cookie, s_alac_cookie_len);
        if (!s_alac) {
          s_alac_open_rc = -9;
          alac_wrap_get_init_diag(&s_alac_init_rc, &s_alac_init_fl,
                                  &s_alac_init_bd, &s_alac_init_ch,
                                  &s_alac_init_sr);
          ESP_LOGE(TAG, "ALAC create failed: diag rc=%d fl=%u bd=%u ch=%u sr=%u",
                   s_alac_init_rc, s_alac_init_fl, s_alac_init_bd,
                   s_alac_init_ch, s_alac_init_sr);
          return;
        }"""
assert old in s, 'create block not found'
s = s.replace(old, new, 1)

old2 = 'static volatile int s_alac_open_rc = -1;'
new2 = """static volatile int s_alac_open_rc = -1;
static int s_alac_init_rc = 0;
static uint32_t s_alac_init_fl = 0;
static uint32_t s_alac_init_bd = 0;
static uint32_t s_alac_init_ch = 0;
static uint32_t s_alac_init_sr = 0;"""
assert old2 in s, 'open_rc var not found'
s = s.replace(old2, new2, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)

h = base + r'\main\dlna\dlna_stream.h'
hs = io.open(h, encoding='utf-8').read()
if 'alac_wrap_get_init_diag' not in hs:
    hs = hs.replace(
        '#include <stdint.h>',
        '#include <stdint.h>\n'
        'extern "C" void alac_wrap_get_init_diag(int *rc, uint32_t *fl, '
        'uint32_t *bd, uint32_t *ch, uint32_t *sr);', 1)
io.open(h, 'w', encoding='utf-8', newline='').write(hs)

w = base + r'\main\network\web_server.c'
ws = io.open(w, encoding='utf-8').read()
old3 = '''    cJSON_AddNumberToObject(json, "dlna_alac_open_rc",
                            dlna_stream_get_alac_open_rc());'''
new3 = '''    cJSON_AddNumberToObject(json, "dlna_alac_open_rc",
                            dlna_stream_get_alac_open_rc());
    { int rc; uint32_t fl, bd, ch, sr;
      alac_wrap_get_init_diag(&rc, &fl, &bd, &ch, &sr);
      cJSON_AddNumberToObject(json, "alac_init_rc", rc);
      cJSON_AddNumberToObject(json, "alac_init_fl", fl);
      cJSON_AddNumberToObject(json, "alac_init_bd", bd);
      cJSON_AddNumberToObject(json, "alac_init_ch", ch);
      cJSON_AddNumberToObject(json, "alac_init_sr", sr); }'''
assert old3 in ws, 'web_server block not found'
ws = ws.replace(old3, new3, 1)
io.open(w, 'w', encoding='utf-8', newline='').write(ws)
print('diag wired')
