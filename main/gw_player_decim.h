#pragma once

/* Hi-res down to 44.1 or 48 kHz for the Gateway's player (phase 3e,
 * ROADMAP_USB_PLAYER.md point 3): 88.2 and 96 kHz by 2, 176.4 and 192 kHz
 * by 4 (two half-band stages). Flat to 20 kHz within 0.0001 dB; what would
 * fold into 0..20 kHz is down by at least 109 dB. The result is computed:
 * 24-bit lanes. Every other rate goes through unchanged (factor 1). */

#include <stdbool.h>
#include <stdint.h>

#define DECIM_SLOTS 8

#define DECIM_CHUNK 256 /* input frames per decim_run call, at most */

typedef struct {
  const float *odd; /* h[c +- (2k+1)], k < K; h[c] = 0.5 */
  int K, N;         /* N = 4K - 1 taps */
  float *hist[DECIM_SLOTS]; /* the last N - 1 inputs of each channel */
  float *work;      /* N - 1 + DECIM_CHUNK, internal RAM when there is room */
  int phase;        /* 1: the next input gives an output */
} decim_stage_t;

typedef struct {
  int factor;       /* 1, 2 or 4 */
  int out_rate;
  float in_scale;   /* to the 24-bit range: 256 for 16-bit files */
  uint8_t present;
  decim_stage_t s1, s2; /* s1 only for 4 */
  float *mid, *y;       /* per channel: s1's output, s2's output */
  /* the caller's block: in[slot][i] (already times in_scale), out[slot][j];
   * internal RAM when there is room */
  float *in[DECIM_SLOTS];
  int32_t *out[DECIM_SLOTS];
} decim_t;

/* The factor for in_rate: 2 for 88.2/96 kHz, 4 for 176.4/192 kHz, 1 for
 * 44.1/48, 0 for anything else. */
int decim_factor(int in_rate);

/* Sets d up for in_rate (bits of the input samples, the slots present).
 * False when out of memory. A factor of 1 needs nothing. */
bool decim_init(decim_t *d, int in_rate, int bits, uint8_t present);
void decim_reset(decim_t *d); /* a seek: the filters start empty */
void decim_free(decim_t *d);

/* n <= DECIM_CHUNK input frames in d->in[slot][0..n) for the present
 * slots; the output frames (24-bit range) in d->out[slot]. Returns how
 * many came out. */
int decim_run(decim_t *d, int n);
