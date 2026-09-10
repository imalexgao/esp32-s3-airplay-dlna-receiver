# -*- coding: utf-8 -*-
"""Add temporary dlna_cp crash locators into helix noiseless.c."""
import io

p = r'main\dlna\codecs\libhelix-aac\noiseless.c'
s = io.open(p, encoding='utf-8').read()

old_inc = '#include "coder.h"'
new_inc = '#include "coder.h"\nextern void dlna_cp(unsigned int s);'
if 'extern void dlna_cp' not in s:
    assert old_inc in s, 'include not found'
    s = s.replace(old_inc, new_inc, 1)
    print('extern added')

marks = [
    ('\tpsi = (PSInfoBase *)(aacDecInfo->psInfoBase);',
     '\tpsi = (PSInfoBase *)(aacDecInfo->psInfoBase);\n\tdlna_cp(47); /* noiseless entry */'),
    ('\tDecodeICS(psi, &bsi, ch);',
     '\tDecodeICS(psi, &bsi, ch);\n\tdlna_cp(48); /* ICS done */'),
    ('\tif (icsInfo->winSequence == 2)\n\t\tDecodeSpectrumShort(psi, &bsi, ch);\n\telse\n\t\tDecodeSpectrumLong(psi, &bsi, ch);',
     '\tif (icsInfo->winSequence == 2)\n\t\tDecodeSpectrumShort(psi, &bsi, ch);\n\telse\n\t\tDecodeSpectrumLong(psi, &bsi, ch);\n\tdlna_cp(49); /* spectrum done */'),
    ('\tbitsUsed = CalcBitsUsed(&bsi, *buf, *bitOffset);',
     '\tdlna_cp(50); /* before CalcBitsUsed */\n\tbitsUsed = CalcBitsUsed(&bsi, *buf, *bitOffset);'),
]
for old, new in marks:
    n = s.count(old)
    if n == 0:
        print('WARN not found:', repr(old[:50]))
        continue
    s = s.replace(old, new, 1)
    print('added:', repr(old[:40]))

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('done')
