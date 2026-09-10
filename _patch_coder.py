# -*- coding: utf-8 -*-
"""Pad PSInfoBase arrays to absorb small OOB writes."""
import io

p = r'main\dlna\codecs\libhelix-aac\coder.h'
s = io.open(p, encoding='utf-8').read()

s = s.replace(
    "short                 scaleFactors[MAX_NCHANS_ELEM][MAX_SF_BANDS];",
    "short                 scaleFactors[MAX_NCHANS_ELEM][MAX_SF_BANDS + 16];", 1)
s = s.replace(
    "unsigned char         sfbCodeBook[MAX_NCHANS_ELEM][MAX_SF_BANDS];",
    "unsigned char         sfbCodeBook[MAX_NCHANS_ELEM][MAX_SF_BANDS + 16];", 1)
s = s.replace(
    "int                   coef[MAX_NCHANS_ELEM][AAC_MAX_NSAMPS];",
    "int                   coef[MAX_NCHANS_ELEM][AAC_MAX_NSAMPS + 16];", 1)
s = s.replace(
    "int                   overlap[AAC_MAX_NCHANS][AAC_MAX_NSAMPS];",
    "int                   overlap[AAC_MAX_NCHANS][AAC_MAX_NSAMPS + 16];", 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('padded')
# verify
t = io.open(p, encoding='utf-8').read()
for line in t.splitlines():
    if 'MAX_SF_BANDS + 16' in line or 'AAC_MAX_NSAMPS + 16' in line:
        print(line.strip())
