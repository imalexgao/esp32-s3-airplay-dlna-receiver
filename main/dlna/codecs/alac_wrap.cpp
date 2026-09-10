/*
 * ALAC decoder thin C wrapper around Apple's ALACDecoder (Apache-2.0).
 * Buffers owned by the caller; the decoder keeps its internal mix buffers
 * in PSRAM (patched in ALACDecoder.cpp).
 */
#include "libalac/codec/ALACDecoderPs.h"
#include "libalac/codec/ALACBitUtilities.h"
#include "libalac/codec/ALACAudioTypes.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "ALAC";

static int s_init_diag[5] = {0, 0, 0, 0, 0}; /* rc, fl, bd, ch, sr */
static uint32_t s_dec_fail = 0;
static int32_t s_dec_last_rc = 0;

extern "C" void alac_wrap_get_dec_diag(uint32_t *fail, int32_t *last) {
  if (fail) *fail = s_dec_fail;
  if (last) *last = s_dec_last_rc;
}

extern "C" void alac_wrap_get_init_diag(int *rc, uint32_t *fl, uint32_t *bd,
                                        uint32_t *ch, uint32_t *sr) {
  if (rc) *rc = s_init_diag[0];
  if (fl) *fl = (uint32_t)s_init_diag[1];
  if (bd) *bd = (uint32_t)s_init_diag[2];
  if (ch) *ch = (uint32_t)s_init_diag[3];
  if (sr) *sr = (uint32_t)s_init_diag[4];
}

struct AlacDec {
  ALACDecoderPs *dec;
  uint32_t channels;
  uint32_t bit_depth;
  uint32_t frame_length;
};

extern "C" {

void *alac_dec_create(const uint8_t *cookie, uint32_t cookie_len) {
  AlacDec *c = (AlacDec *)calloc(1, sizeof(AlacDec));
  if (!c) {
    return NULL;
  }
  ALACDecoderPs *d = new ALACDecoderPs();
  if (!d) {
    free(c);
    return NULL;
  }
  if (d->Init((void *)cookie, cookie_len) != 0) {
    s_init_diag[0] = -1;
    s_init_diag[1] = (int)d->mConfig.frameLength;
    s_init_diag[2] = (int)d->mConfig.bitDepth;
    s_init_diag[3] = (int)d->mConfig.numChannels;
    s_init_diag[4] = (int)d->mConfig.sampleRate;
    ESP_LOGE(TAG, "Init failed: fl=%u bd=%u ch=%u sr=%u",
             d->mConfig.frameLength, d->mConfig.bitDepth,
             d->mConfig.numChannels, d->mConfig.sampleRate);
    delete d;
    free(c);
    return NULL;
  }
  s_init_diag[0] = 0;
  s_init_diag[1] = (int)d->mConfig.frameLength;
  s_init_diag[2] = (int)d->mConfig.bitDepth;
  s_init_diag[3] = (int)d->mConfig.numChannels;
  s_init_diag[4] = (int)d->mConfig.sampleRate;
  c->dec = d;
  c->channels = d->mConfig.numChannels ? d->mConfig.numChannels : 2;
  c->bit_depth = d->mConfig.bitDepth ? d->mConfig.bitDepth : 16;
  c->frame_length = d->mConfig.frameLength ? d->mConfig.frameLength : 4096;
  return c;
}

void alac_dec_get_info(void *h, uint32_t *rate, uint32_t *channels,
                       uint32_t *bits) {
  AlacDec *c = (AlacDec *)h;
  if (rate) {
    *rate = c->dec->mConfig.sampleRate;
  }
  if (channels) {
    *channels = c->channels;
  }
  if (bits) {
    *bits = c->bit_depth;
  }
}

/* out must hold at least frame_length*channels int16 samples (caller
 * guarantees it). Decodes one ALAC frame, converts to int16 interleaved. */
int alac_dec_decode(void *h, const uint8_t *frame, uint32_t frame_len,
                    int16_t *out, uint32_t *out_frames) {
  AlacDec *c = (AlacDec *)h;
  BitBuffer bits;
  BitBufferInit(&bits, (uint8_t *)frame, frame_len);

  uint32_t nf = c->frame_length;
  uint32_t outn = 0;
  uint8_t *tmp = (uint8_t *)out;
  uint8_t *scratch = NULL;
  if (c->bit_depth != 16) {
    size_t raw_bytes = (size_t)nf * c->channels *
                       (c->bit_depth <= 24 ? 3 : 4);
    scratch = (uint8_t *)malloc(raw_bytes);
    if (!scratch) {
      return -1;
    }
    tmp = scratch;
  }

  int32_t rc = c->dec->Decode(&bits, tmp, nf, c->channels, &outn);
  if (rc != 0) {
    s_dec_fail++;
    s_dec_last_rc = rc;
    if (scratch) {
      free(scratch);
    }
    return -1;
  }

  uint32_t total = outn * c->channels;
  if (c->bit_depth <= 16) {
    const int16_t *s = (const int16_t *)tmp;
    for (uint32_t i = 0; i < total; i++) {
      out[i] = s[i];
    }
  } else if (c->bit_depth <= 24) {
    const uint8_t *b = tmp;
    for (uint32_t i = 0; i < total; i++) {
      int32_t v = (int32_t)b[i * 3] | ((int32_t)b[i * 3 + 1] << 8) |
                  ((int32_t)b[i * 3 + 2] << 16);
      if (v & 0x800000) {
        v |= ~0xFFFFFF;
      }
      out[i] = (int16_t)(v >> 8);
    }
  } else {
    const int32_t *s = (const int32_t *)tmp;
    for (uint32_t i = 0; i < total; i++) {
      out[i] = (int16_t)(s[i] >> 16);
    }
  }
  if (scratch) {
    free(scratch);
  }
  *out_frames = outn;
  return 0;
}

void alac_dec_destroy(void *h) {
  if (!h) {
    return;
  }
  AlacDec *c = (AlacDec *)h;
  if (c->dec) {
    delete c->dec;
  }
  free(c);
}

} /* extern "C" */
