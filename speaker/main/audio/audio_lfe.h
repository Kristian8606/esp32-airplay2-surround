#pragma once

/* Subwoofer LFE gain and its limiter (pure C, no IDF calls, host-tested).
 *
 * The LFE channel of 5.1/7.1 is mixed 10 dB below the other channels and a
 * playback system adds the 10 dB back (Dolby/DTS "in-band gain"). A stereo
 * source has no LFE channel, so the gain is for 5.1/7.1 blocks only.
 *
 * Gain: applied per decoded block, before the PCM FIFO; a change ramps
 * linearly over one block, so a new setting or a stereo <-> 5.1 change makes
 * no step. Float PCM keeps the boosted samples above 1.0 until the end of
 * the chain.
 *
 * Limiter: the last step before the float -> int32 conversion, after the
 * volume, so it sees the level the DAC gets. It has no lookahead (no delay
 * added to the subwoofer): a sample that would pass the threshold sets the
 * gain at once so that it lands exactly on it, the gain is then held for
 * at least one period of the lowest bass and released slowly. Bass is
 * periodic: only the first peak of an over gets the instant gain change;
 * the following ones are already at the held gain and keep their shape.
 * Below the threshold the samples pass bit-exact. */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Threshold -0.26 dBFS; hold 30 ms (above one period of 40 Hz); release
 * 200 ms (time constant). */
#define AUDIO_LFE_LIMIT_THRESHOLD 0.97f
#define AUDIO_LFE_LIMIT_HOLD_S    0.030f
#define AUDIO_LFE_LIMIT_RELEASE_S 0.200f

static inline float audio_lfe_gain_linear(uint32_t db) {
  return db ? powf(10.0f, (float)db / 20.0f) : 1.0f;
}

/* Multiply the block by the gain, ramping from *current to target. */
static inline void audio_lfe_gain_apply(float *pcm, uint32_t frames,
                                        float *current, float target) {
  const float start = *current;
  if (start == target) {
    if (target == 1.0f) return;
    for (uint32_t i = 0; i < frames * 2U; ++i) pcm[i] *= target;
    return;
  }
  const float step = (target - start) / (float)(frames ? frames : 1U);
  float g = start;
  for (uint32_t f = 0; f < frames; ++f) {
    g += step;
    pcm[2U * f] *= g;
    pcm[2U * f + 1U] *= g;
  }
  *current = target;
}

typedef struct {
  float gain;    /* 1.0 = not limiting */
  uint32_t hold; /* samples left before the release starts */
} audio_lfe_limiter_t;

static inline void audio_lfe_limiter_reset(audio_lfe_limiter_t *l) {
  l->gain = 1.0f;
  l->hold = 0U;
}

/* Stereo interleaved, both channels at one gain (the subwoofer plays one
 * signal on both). */
static inline void audio_lfe_limiter_process(audio_lfe_limiter_t *l,
                                             float *pcm, uint32_t frames,
                                             uint32_t rate) {
  if (rate == 0U) rate = 44100U;
  const uint32_t hold_n = (uint32_t)(AUDIO_LFE_LIMIT_HOLD_S * (float)rate);
  const float rel = 1.0f - expf(-1.0f / (AUDIO_LFE_LIMIT_RELEASE_S * (float)rate));
  const float thr = AUDIO_LFE_LIMIT_THRESHOLD;
  float g = l->gain;
  uint32_t hold = l->hold;
  for (uint32_t f = 0; f < frames; ++f) {
    float *p = pcm + 2U * f;
    const float a = fabsf(p[0]), b = fabsf(p[1]);
    const float peak = a > b ? a : b;
    if (peak * g > thr) {
      g = thr / peak; /* only while limiting: no division otherwise */
      hold = hold_n;
    } else if (hold > 0U) {
      --hold;
    } else if (g < 1.0f) {
      g += (1.0f - g) * rel;
      if (g > 0.99999f) g = 1.0f;
    }
    if (g < 1.0f) {
      p[0] *= g;
      p[1] *= g;
    }
  }
  l->gain = g;
  l->hold = hold;
}
