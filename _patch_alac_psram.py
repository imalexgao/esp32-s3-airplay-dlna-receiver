# -*- coding: utf-8 -*-
"""Patch Apple ALACDecoder to allocate its internal buffers from PSRAM."""
import io

p = r'main\dlna\codecs\libalac\codec\ALACDecoder.cpp'
s = io.open(p, encoding='utf-8').read()

# include esp heap caps after existing includes
if '#include "esp_heap_caps.h"' not in s:
    s = s.replace(
        '#include "ALACAudioTypes.h"',
        '#include "ALACAudioTypes.h"\n#include "esp_heap_caps.h"', 1)

# calloc -> heap_caps_calloc SPIRAM
s = s.replace(
    'mMixBufferU = (int32_t *) calloc( mConfig.frameLength * sizeof(int32_t), 1 );',
    'mMixBufferU = (int32_t *) heap_caps_calloc( mConfig.frameLength * sizeof(int32_t), 1, MALLOC_CAP_SPIRAM );\n        if (!mMixBufferU) mMixBufferU = (int32_t *) calloc( mConfig.frameLength * sizeof(int32_t), 1 );', 1)
s = s.replace(
    'mMixBufferV = (int32_t *) calloc( mConfig.frameLength * sizeof(int32_t), 1 );',
    'mMixBufferV = (int32_t *) heap_caps_calloc( mConfig.frameLength * sizeof(int32_t), 1, MALLOC_CAP_SPIRAM );\n        if (!mMixBufferV) mMixBufferV = (int32_t *) calloc( mConfig.frameLength * sizeof(int32_t), 1 );', 1)
s = s.replace(
    'mPredictor = (int32_t *) calloc( mConfig.frameLength * sizeof(int32_t), 1 );',
    'mPredictor = (int32_t *) heap_caps_calloc( mConfig.frameLength * sizeof(int32_t), 1, MALLOC_CAP_SPIRAM );\n        if (!mPredictor) mPredictor = (int32_t *) calloc( mConfig.frameLength * sizeof(int32_t), 1 );', 1)

# free -> heap_caps_free (with malloc fallback tracked? simple: free works on SPIRAM ptr via heap_caps_free; use heap_caps_free if possible else free)
s = s.replace(
    '        if ( mMixBufferU )\n    {\n\t\tfree(mMixBufferU);\n        mMixBufferU = NULL;\n    }',
    '        if ( mMixBufferU )\n    {\n\t\theap_caps_free(mMixBufferU);\n        mMixBufferU = NULL;\n    }', 1)
s = s.replace(
    '        if ( mMixBufferV )\n    {\n\t\tfree(mMixBufferV);\n        mMixBufferV = NULL;\n    }',
    '        if ( mMixBufferV )\n    {\n\t\theap_caps_free(mMixBufferV);\n        mMixBufferV = NULL;\n    }', 1)
s = s.replace(
    '        if ( mPredictor )\n    {\n\t\tfree(mPredictor);\n        mPredictor = NULL;',
    '        if ( mPredictor )\n    {\n\t\theap_caps_free(mPredictor);\n        mPredictor = NULL;', 1)

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('ALACDecoder.cpp patched for PSRAM')
# verify
left = [ln for ln in s.splitlines() if 'calloc' in ln or 'free(' in ln]
print('remaining alloc refs:')
for l in left:
    print(' ', l.strip())
