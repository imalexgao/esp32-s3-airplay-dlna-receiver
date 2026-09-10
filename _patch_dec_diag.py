# -*- coding: utf-8 -*-
"""Add ALAC decode failure counters for diagnosis."""
import io

base = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver'

# 1. alac_wrap.cpp: counters + getter
w = base + r'\main\dlna\codecs\alac_wrap.cpp'
ws = io.open(w, encoding='utf-8').read()
old = 'static int s_init_diag[5] = {0, 0, 0, 0, 0}; /* rc, fl, bd, ch, sr */'
new = ('static int s_init_diag[5] = {0, 0, 0, 0, 0}; /* rc, fl, bd, ch, sr */\n'
       'static uint32_t s_dec_fail = 0;\n'
       'static int32_t s_dec_last_rc = 0;\n'
       '\n'
       'extern "C" void alac_wrap_get_dec_diag(uint32_t *fail, int32_t *last) {\n'
       '  if (fail) *fail = s_dec_fail;\n'
       '  if (last) *last = s_dec_last_rc;\n'
       '}')
assert old in ws
ws = ws.replace(old, new, 1)

old2 = '''  int32_t rc = c->dec->Decode(&bits, tmp, nf, c->channels, &outn);
  if (rc != 0) {
    if (scratch) {
      free(scratch);
    }
    return -1;
  }'''
new2 = '''  int32_t rc = c->dec->Decode(&bits, tmp, nf, c->channels, &outn);
  if (rc != 0) {
    s_dec_fail++;
    s_dec_last_rc = rc;
    if (scratch) {
      free(scratch);
    }
    return -1;
  }'''
assert old2 in ws
ws = ws.replace(old2, new2, 1)
io.open(w, 'w', encoding='utf-8', newline='').write(ws)

# 2. dlna_stream.c: hook into ALAC decode call site + getter
p = base + r'\main\dlna\dlna_stream.c'
s = io.open(p, encoding='utf-8').read()
old3 = '''      uint32_t outn = 0;
      if (alac_dec_decode(s_alac, s_alac_fbuf, s_alac_flen, s_alac_pcm,
                          &outn) == 0 && outn > 0) {'''
new3 = '''      uint32_t outn = 0;
      s_alac_feed_count++;
      if (alac_dec_decode(s_alac, s_alac_fbuf, s_alac_flen, s_alac_pcm,
                          &outn) == 0 && outn > 0) {'''
assert old3 in s
s = s.replace(old3, new3, 1)

old4 = 'static volatile uint64_t s_alac_frames_total = 0;'
new4 = 'static volatile uint64_t s_alac_frames_total = 0;\nstatic uint32_t s_alac_feed_count = 0;'
assert old4 in s
s = s.replace(old4, new4, 1)

old5 = 'uint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }'
new5 = ('uint64_t dlna_stream_get_alac_frames(void) { return s_alac_frames_total; }\n'
        'uint32_t dlna_stream_get_alac_feed_count(void) { return s_alac_feed_count; }')
assert old5 in s
s = s.replace(old5, new5, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)

# 3. dlna_stream.h decls
h = base + r'\main\dlna\dlna_stream.h'
hs = io.open(h, encoding='utf-8').read()
add = ('uint32_t dlna_stream_get_alac_feed_count(void);\n'
       'void alac_wrap_get_dec_diag(uint32_t *fail, int32_t *last);\n'
       '#ifdef __cplusplus\nextern "C" {\n#endif\n')
if 'alac_wrap_get_dec_diag' not in hs:
    # insert into the extern C block
    oldh = '''void alac_wrap_get_init_diag(int *rc, uint32_t *fl, uint32_t *bd,
                             uint32_t *ch, uint32_t *sr);'''
    newh = oldh + '\nvoid alac_wrap_get_dec_diag(uint32_t *fail, int32_t *last);'
    assert oldh in hs
    hs = hs.replace(oldh, newh, 1)
    oldh2 = 'uint32_t dlna_stream_get_alac_stsz_idx(void);'
    newh2 = oldh2 + '\nuint32_t dlna_stream_get_alac_feed_count(void);'
    assert oldh2 in hs
    hs = hs.replace(oldh2, newh2, 1)
io.open(h, 'w', encoding='utf-8', newline='').write(hs)

# 4. web_server.c output
w2 = base + r'\main\network\web_server.c'
ws2 = io.open(w2, encoding='utf-8').read()
old6 = '''    cJSON_AddNumberToObject(json, "dlna_alac_stsz_idx",
                            dlna_stream_get_alac_stsz_idx());'''
new6 = old6 + '''
    cJSON_AddNumberToObject(json, "dlna_alac_feed_count",
                            dlna_stream_get_alac_feed_count());
    { uint32_t df; int32_t dl; alac_wrap_get_dec_diag(&df, &dl);
      cJSON_AddNumberToObject(json, "alac_dec_fail", df);
      cJSON_AddNumberToObject(json, "alac_dec_last_rc", dl); }'''
assert old6 in ws2
ws2 = ws2.replace(old6, new6, 1)
io.open(w2, 'w', encoding='utf-8', newline='').write(ws2)
print('dec diag wired')
