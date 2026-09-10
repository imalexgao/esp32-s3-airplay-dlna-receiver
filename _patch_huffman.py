# -*- coding: utf-8 -*-
"""Add write-guard clamps to helix huffman.c DecodeSpectrumLong/Short."""
import io

p = r'main\dlna\codecs\libhelix-aac\huffman.c'
s = io.open(p, encoding='utf-8').read()

# --- DecodeSpectrumLong guard ---
old_long = """	for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
		cb = *sfbCodeBook++;
		nVals = sfbTab[sfb+1] - sfbTab[sfb];
		
		if (cb == 0)
			UnpackZeros(nVals, coef);
		else if (cb <= 4)
			UnpackQuads(bsi, cb, nVals, coef);
		else if (cb <= 10)
			UnpackPairsNoEsc(bsi, cb, nVals, coef);
		else if (cb == 11)
			UnpackPairsEsc(bsi, cb, nVals, coef);
		else
			UnpackZeros(nVals, coef);

		coef += nVals;
	}

	/* fill with zeros above maxSFB */
	nVals = NSAMPS_LONG - sfbTab[sfb];
	UnpackZeros(nVals, coef);
"""
new_long = """	for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
		cb = *sfbCodeBook++;
		nVals = sfbTab[sfb+1] - sfbTab[sfb];
		/* guard: never write past the 1024 coefficient array */
		{
			int room = NSAMPS_LONG - (int)(coef - psi->coef[ch]);
			if (nVals > room) nVals = room;
		}
		if (nVals <= 0)
			break;
		
		if (cb == 0)
			UnpackZeros(nVals, coef);
		else if (cb <= 4)
			UnpackQuads(bsi, cb, nVals, coef);
		else if (cb <= 10)
			UnpackPairsNoEsc(bsi, cb, nVals, coef);
		else if (cb == 11)
			UnpackPairsEsc(bsi, cb, nVals, coef);
		else
			UnpackZeros(nVals, coef);

		coef += nVals;
	}

	/* fill with zeros above maxSFB */
	{
		int rem = NSAMPS_LONG - (int)(coef - psi->coef[ch]);
		if (rem < 0) rem = 0;
		UnpackZeros(rem, coef);
	}
"""
if old_long in s:
    s = s.replace(old_long, new_long, 1)
    print('DecodeSpectrumLong guard added')
else:
    print('WARN long pattern not found')

# --- DecodeSpectrumShort guard ---
old_short = """		for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
			nVals = sfbTab[sfb+1] - sfbTab[sfb];
			cb = *sfbCodeBook++;

			for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
				offset = win*NSAMPS_SHORT;
				if (cb == 0)
					UnpackZeros(nVals, coef + offset);
				else if (cb <= 4)
					UnpackQuads(bsi, cb, nVals, coef + offset);
				else if (cb <= 10)
					UnpackPairsNoEsc(bsi, cb, nVals, coef + offset);
				else if (cb == 11)
					UnpackPairsEsc(bsi, cb, nVals, coef + offset);
				else 
					UnpackZeros(nVals, coef + offset);
			}
			coef += nVals;
		}

		/* fill with zeros above maxSFB */
		for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
			offset = win*NSAMPS_SHORT;
			nVals = NSAMPS_SHORT - sfbTab[sfb];
			UnpackZeros(nVals, coef + offset);
		}
		coef += nVals;
		coef += (icsInfo->winGroupLen[gp] - 1)*NSAMPS_SHORT;
"""
new_short = """		for (sfb = 0; sfb < icsInfo->maxSFB; sfb++) {
			nVals = sfbTab[sfb+1] - sfbTab[sfb];
			cb = *sfbCodeBook++;

			for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
				offset = win*NSAMPS_SHORT;
				{
					int room = NSAMPS_LONG - ((int)(coef - psi->coef[ch]) + offset);
					int v = nVals;
					if (v > room) v = room;
					if (v <= 0) continue;
					if (cb == 0)
						UnpackZeros(v, coef + offset);
					else if (cb <= 4)
						UnpackQuads(bsi, cb, v, coef + offset);
					else if (cb <= 10)
						UnpackPairsNoEsc(bsi, cb, v, coef + offset);
					else if (cb == 11)
						UnpackPairsEsc(bsi, cb, v, coef + offset);
					else 
						UnpackZeros(v, coef + offset);
				}
			}
			coef += nVals;
		}

		/* fill with zeros above maxSFB */
		for (win = 0; win < icsInfo->winGroupLen[gp]; win++) {
			offset = win*NSAMPS_SHORT;
			{
				int rem = NSAMPS_LONG - ((int)(coef - psi->coef[ch]) + offset);
				if (rem < 0) rem = 0;
				UnpackZeros(rem, coef + offset);
			}
		}
		coef += nVals;
		coef += (icsInfo->winGroupLen[gp] - 1)*NSAMPS_SHORT;
"""
if old_short in s:
    s = s.replace(old_short, new_short, 1)
    print('DecodeSpectrumShort guard added')
else:
    print('WARN short pattern not found')

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('done')
