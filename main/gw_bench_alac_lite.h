#pragma once

/* USB player, phase 0b: a light mono ALAC encoder, measured against
 * esp_alac_enc by gw_bench.c (CONFIG_GATEWAY_PLAYER_BENCH, off by default).
 * Not used by any feature. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int bits;          /* 16 or 24 */
  int order;         /* predictor order: 4 or 8 */
  int16_t coefs[8];  /* carried from frame to frame */
} alac_lite_t;

void alac_lite_init(alac_lite_t *e, int bits, int order);

/* One frame of n mono samples (n <= 1024; int32, sign-extended to `bits`)
 * into one ALAC frame (an SCE element and ID_END, byte aligned). out needs
 * n * bits / 8 + 64 bytes. Returns the bytes written; *escaped is set when
 * the frame went out uncompressed. */
size_t alac_lite_encode(alac_lite_t *e, const int32_t *pcm, uint32_t n, uint8_t *out,
                        size_t cap, bool *escaped);
