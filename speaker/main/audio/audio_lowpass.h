#pragma once

/* Subwoofer crossover (pure C, no IDF calls, host-tested).
 *
 * A Linkwitz-Riley low-pass of 4th order (24 dB/octave), as AV receivers and
 * active crossovers use: two identical 2nd-order Butterworth sections
 * (Q = 1/sqrt(2)) in a row, -6 dB at the crossover frequency, so a main
 * speaker's matching high-pass would sum flat with it. Float PCM, stereo
 * interleaved, in place; each channel has its own state.
 *
 * The coefficients are designed in double once per frequency/rate change.
 * A changed frequency keeps the filter state (transposed direct form II
 * tolerates new coefficients: no step in the signal); only a stop clears
 * it (audio_lowpass_reset). */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  float b0, b1, b2, a1, a2; /* a0 = 1 */
} audio_lowpass_coeff_t;

typedef struct {
  uint32_t hz;   /* the coefficients are for this frequency; 0 = off */
  uint32_t rate; /* ... and this sample rate */
  audio_lowpass_coeff_t c;
  float z[2][2][2]; /* [channel][section][z1, z2] */
} audio_lowpass_t;

/* 2nd-order Butterworth low-pass (RBJ cookbook, Q = 1/sqrt(2)). False when
 * the frequency is not below Nyquist. */
static inline bool audio_lowpass_design(audio_lowpass_coeff_t *c, double hz,
                                        double rate) {
  if (!c || hz <= 0.0 || rate <= 0.0 || hz >= rate * 0.5) return false;
  const double w0 = 2.0 * M_PI * hz / rate;
  const double cw = cos(w0);
  const double alpha = sin(w0) / (2.0 * M_SQRT1_2);
  const double a0 = 1.0 + alpha;
  c->b0 = (float)((1.0 - cw) * 0.5 / a0);
  c->b1 = (float)((1.0 - cw) / a0);
  c->b2 = c->b0;
  c->a1 = (float)(-2.0 * cw / a0);
  c->a2 = (float)((1.0 - alpha) / a0);
  return true;
}

static inline void audio_lowpass_reset(audio_lowpass_t *lp) {
  for (int ch = 0; ch < 2; ++ch)
    for (int s = 0; s < 2; ++s) lp->z[ch][s][0] = lp->z[ch][s][1] = 0.0f;
}

/* Set the frequency (0 = off) for this sample rate. Returns false when the
 * filter is off (or the frequency is impossible at this rate): the caller
 * then leaves the block untouched. */
static inline bool audio_lowpass_prepare(audio_lowpass_t *lp, uint32_t hz,
                                         uint32_t rate) {
  if (hz == 0U || rate == 0U) {
    if (lp->hz != 0U) audio_lowpass_reset(lp); /* next start: from silence */
    lp->hz = 0U;
    return false;
  }
  if (hz != lp->hz || rate != lp->rate) {
    audio_lowpass_coeff_t c;
    if (!audio_lowpass_design(&c, (double)hz, (double)rate)) {
      if (lp->hz != 0U) audio_lowpass_reset(lp);
      lp->hz = 0U;
      return false;
    }
    if (lp->hz == 0U || rate != lp->rate) audio_lowpass_reset(lp);
    lp->c = c;
    lp->hz = hz;
    lp->rate = rate;
  }
  return true;
}

/* Both sections over the block, one channel at a time, with the state in
 * locals: the coefficients and state stay in registers. */
static inline void audio_lowpass_process(audio_lowpass_t *lp, float *pcm,
                                         uint32_t frames) {
  const audio_lowpass_coeff_t c = lp->c;
  for (int ch = 0; ch < 2; ++ch) {
    float s1 = lp->z[ch][0][0], s2 = lp->z[ch][0][1];
    float t1 = lp->z[ch][1][0], t2 = lp->z[ch][1][1];
    float *p = pcm + ch;
    for (uint32_t f = 0; f < frames; ++f, p += 2) {
      const float x = *p;
      const float y = c.b0 * x + s1;
      s1 = c.b1 * x - c.a1 * y + s2;
      s2 = c.b2 * x - c.a2 * y;
      const float v = c.b0 * y + t1;
      t1 = c.b1 * y - c.a1 * v + t2;
      t2 = c.b2 * y - c.a2 * v;
      *p = v;
    }
    /* After silence the state decays toward denormals: clear what is far
     * below the 32-bit LSB (as audio_eq.c does). */
    if (fabsf(s1) < 1e-20f) s1 = 0.0f;
    if (fabsf(s2) < 1e-20f) s2 = 0.0f;
    if (fabsf(t1) < 1e-20f) t1 = 0.0f;
    if (fabsf(t2) < 1e-20f) t2 = 0.0f;
    lp->z[ch][0][0] = s1;
    lp->z[ch][0][1] = s2;
    lp->z[ch][1][0] = t1;
    lp->z[ch][1][1] = t2;
  }
}
