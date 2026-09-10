# -*- coding: utf-8 -*-
"""Fix extern-C guard in dlna_stream.h."""
import io

h = r'C:\Users\gaufu\Desktop\airplay2\esp32-s3-airplay-dlna-receiver\main\dlna\dlna_stream.h'
s = io.open(h, encoding='utf-8').read()
old = '#include <stdint.h>\nextern "C" void alac_wrap_get_init_diag(int *rc, uint32_t *fl, uint32_t *bd, uint32_t *ch, uint32_t *sr);'
new = ('#include <stdint.h>\n'
       '#ifdef __cplusplus\nextern "C" {\n#endif\n'
       'void alac_wrap_get_init_diag(int *rc, uint32_t *fl, uint32_t *bd, '
       'uint32_t *ch, uint32_t *sr);\n'
       '#ifdef __cplusplus\n}\n#endif')
if old in s:
    s = s.replace(old, new, 1)
    print('fixed header')
else:
    print('pattern not found; lines with alac_wrap:')
    for ln in s.splitlines():
        if 'alac_wrap' in ln:
            print(repr(ln))
io.open(h, 'w', encoding='utf-8', newline='').write(s)
