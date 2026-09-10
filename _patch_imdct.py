# -*- coding: utf-8 -*-
"""Add dlna_cp points inside IMDCT."""
import io

p = r'main\dlna\codecs\libhelix-aac\imdct.c'
s = io.open(p, encoding='utf-8').read()

old = "int IMDCT(AACDecInfo *aacDecInfo, int ch, int chOut, short *outbuf)\n{\n\tint i;"
new = "int IMDCT(AACDecInfo *aacDecInfo, int ch, int chOut, short *outbuf)\n{\n\textern void dlna_cp(unsigned int s);\n\tint i;"
if old in s:
    s = s.replace(old, new, 1)
    print('entry extern ok')
else:
    print('WARN entry pattern')

old2 = "\t/* optimized type-IV DCT (operates inplace) */\n\tif (icsInfo->winSequence == 2) {"
new2 = "\tdlna_cp(60); /* IMDCT entry */\n\t/* optimized type-IV DCT (operates inplace) */\n\tif (icsInfo->winSequence == 2) {"
if old2 in s:
    s = s.replace(old2, new2, 1)
    print('cp60 ok')
else:
    print('WARN cp60 pattern')

old3 = "\t}\n\n#ifdef AAC_ENABLE_SBR"
new3 = "\t}\n\tdlna_cp(61); /* DCT4 done */\n\n#ifdef AAC_ENABLE_SBR"
if old3 in s:
    s = s.replace(old3, new3, 1)
    print('cp61 ok')
else:
    print('WARN cp61 pattern')

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('done')
