# -*- coding: utf-8 -*-
"""Add temporary dlna_cp crash locator calls into helix aacdec.c."""
import re

p = r'main\dlna\codecs\libhelix-aac\aacdec.c'
s = open(p, encoding='utf-8').read()

# 1) extern declaration after the includes
old = '#include "aaccommon.h"'
new = '#include "aaccommon.h"\nextern void dlna_cp(unsigned int s);'
if 'extern void dlna_cp' not in s:
    assert old in s, 'include pattern not found'
    s = s.replace(old, new, 1)
    print('extern added')
else:
    print('extern already present')

# 2) crash locator points inside AACDecode body
marks = [
    ('\tif (!aacDecInfo)\n\t\treturn ERR_AAC_NULL_POINTER;\n',
     '\tif (!aacDecInfo)\n\t\treturn ERR_AAC_NULL_POINTER;\n\tdlna_cp(40); /* AACDecode entry */\n', '40'),
    ('\tif (aacDecInfo->format == AAC_FF_ADTS) {\n',
     '\tdlna_cp(41); /* format=%d */\n\tif (aacDecInfo->format == AAC_FF_ADTS) {\n', '41'),
    ('\t\terr = UnpackADTSHeader(aacDecInfo, &inptr, &bitOffset, &bitsAvail);\n\t\tif (err)\n\t\t\treturn err;\n',
     '\t\terr = UnpackADTSHeader(aacDecInfo, &inptr, &bitOffset, &bitsAvail);\n\t\tdlna_cp(42); /* ADTS header ok */\n\t\tif (err)\n\t\t\treturn err;\n', '42'),
    ('\t\t/* parse next syntactic element */\n\t\terr = DecodeNextElement(aacDecInfo, &inptr, &bitOffset, &bitsAvail);\n\t\tif (err)\n\t\t\treturn err;\n',
     '\t\t/* parse next syntactic element */\n\t\tdlna_cp(43); /* before DecodeNextElement */\n\t\terr = DecodeNextElement(aacDecInfo, &inptr, &bitOffset, &bitsAvail);\n\t\tdlna_cp(44); /* after DecodeNextElement */\n\t\tif (err)\n\t\t\treturn err;\n', '43/44'),
    ('\t\t\terr = DecodeNoiselessData(aacDecInfo, &inptr, &bitOffset, &bitsAvail, ch);\n',
     '\t\t\tdlna_cp(45); /* before DecodeNoiselessData */\n\t\t\terr = DecodeNoiselessData(aacDecInfo, &inptr, &bitOffset, &bitsAvail, ch);\n', '45'),
    ('\t\t\tif (IMDCT(aacDecInfo, ch, baseChan + ch, outbuf))\n',
     '\t\t\tdlna_cp(46); /* before IMDCT */\n\t\t\tif (IMDCT(aacDecInfo, ch, baseChan + ch, outbuf))\n', '46'),
]

for old, new, tag in marks:
    if s.count(old) == 0:
        print(f'WARN mark {tag} not found, skipping')
        continue
    if s.count(old) > 1:
        print(f'WARN mark {tag} found {s.count(old)} times, applying first')
    s = s.replace(old, new, 1)
    print(f'mark {tag} added')

open(p, 'w', encoding='utf-8', newline='').write(s)
print('done')
