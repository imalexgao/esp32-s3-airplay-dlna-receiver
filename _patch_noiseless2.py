# -*- coding: utf-8 -*-
"""Add write guards to noiseless.c DecodeSectionData/DecodeScaleFactors."""
import io

p = r'main\dlna\codecs\libhelix-aac\noiseless.c'
s = io.open(p, encoding='utf-8').read()

# --- DecodeSectionData guard ---
old1 = """	for (g = 0; g < numWinGrp; g++) {
		sfb = 0;
		while (sfb < maxSFB) {
			cb = GetBits(bsi, 4);	/* next section codebook */
			sectLen = 0;
			do {
				sectLenIncr = GetBits(bsi, sectLenBits);
				sectLen += sectLenIncr;
			} while (sectLenIncr == sectEscapeVal);

			sfb += sectLen;
			while (sectLen--)
				*sfbCodeBook++ = (unsigned char)cb;
		}
		ASSERT(sfb == maxSFB);
	}
"""
new1 = """	for (g = 0; g < numWinGrp; g++) {
		unsigned char *cbStart = sfbCodeBook;
		int cbCap = numWinGrp * maxSFB;
		sfb = 0;
		while (sfb < maxSFB) {
			cb = GetBits(bsi, 4);	/* next section codebook */
			sectLen = 0;
			do {
				sectLenIncr = GetBits(bsi, sectLenBits);
				sectLen += sectLenIncr;
			} while (sectLenIncr == sectEscapeVal);

			sfb += sectLen;
			while (sectLen--) {
				if ((int)(sfbCodeBook - cbStart) >= cbCap) break;
				*sfbCodeBook++ = (unsigned char)cb;
			}
		}
	}
"""
if old1 in s:
    s = s.replace(old1, new1, 1)
    print('DecodeSectionData guard added')
else:
    print('WARN section pattern not found')

# --- DecodeScaleFactors guard ---
old2 = """	for (g = 0; g < numWinGrp * maxSFB; g++) {
		sfbCB = *sfbCodeBook++;
"""
new2 = """	for (g = 0; g < numWinGrp * maxSFB; g++) {
		if (g >= MAX_SF_BANDS) break;
		sfbCB = *sfbCodeBook++;
"""
if old2 in s:
    s = s.replace(old2, new2, 1)
    print('DecodeScaleFactors guard added')
else:
    print('WARN scalefactor pattern not found')

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('done')
