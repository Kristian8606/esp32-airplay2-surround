#pragma once

/* Sample formats of the playout chain.
 *
 * AAC delivers signed32 PCM directly to float; buffered ALAC16/24 also
 * decodes directly to float. Realtime ALAC16 retains its int16 staging.
 * Every sample becomes a float with 1.0 = full scale, and the chain stays
 * float: EQ, the PCM FIFO, output volume and loudness. There is ample headroom, so
 * there is no clipping or requantisation anywhere in between.
 *
 * The one and only conversion with saturation is the last step before DMA:
 * float -> signed 32-bit, full scale 2^31, sent in 32-bit I2S slots (32-bit
 * data width, 64 BCLK per stereo frame). Nothing is rounded to 24 bits: the
 * DAC takes as many MSBs as it has (I2S is MSB first).
 *
 * At unity gain with EQ and loudness off the chain is bit-exact: a 16-bit
 * sample x arrives at the DAC as exactly x << 16.
 *
 * This header is pure C (no IDF calls) so the math can be host-tested. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One I2S slot value: signed 32-bit, full scale 2^31. */
typedef int32_t audio_out_sample_t;

#define AUDIO_PCM16_TO_FLOAT (1.0f / 32768.0f)
#define AUDIO_PCM32_TO_FLOAT (1.0f / 2147483648.0f)

/* Preserve float32's precision from signed32, with no 16-bit intermediate.
 * S32 has more mantissa bits than float32; this is not lossless S32 PCM. */
static inline float audio_pcm32_to_float(int32_t x) {
  return (float)x * AUDIO_PCM32_TO_FLOAT;
}

/* 16-bit PCM -> float, exact (every int16 value is representable). */
static inline float audio_pcm16_to_float(int16_t x) {
  return (float)x * AUDIO_PCM16_TO_FLOAT;
}

static inline void audio_pcm16_to_float_block(const int16_t *in, float *out,
                                              size_t samples) {
  for (size_t i = 0; i < samples; ++i) out[i] = audio_pcm16_to_float(in[i]);
}

/* Largest float below 1.0 (1 - 2^-24); times 2^31 it is 0x7FFFFF80, the
 * largest float that still fits an int32. (float)INT32_MAX would round up to
 * 2^31 and overflow. */
#define AUDIO_OUT_FLOAT_MAX 0.99999994f
#define AUDIO_OUT_S32_MAX   ((int32_t)0x7FFFFF80)

/* The single saturation of the chain: float (1.0 = full scale) -> 32-bit
 * slot. No rounding: the conversion truncates below the 32nd bit, far below
 * any DAC. NaN (a broken filter) becomes silence instead of full scale. */
static inline int32_t audio_out_from_float(float x) {
  if (x < AUDIO_OUT_FLOAT_MAX && x > -1.0f) return (int32_t)(x * 2147483648.0f);
  if (x >= AUDIO_OUT_FLOAT_MAX) return AUDIO_OUT_S32_MAX;
  if (x <= -1.0f) return INT32_MIN;
  return 0; /* NaN */
}

/* Convert a float block to 32-bit slots in place and return it as the I2S
 * block (same memory, same 4 bytes per sample). The int32 results are stored
 * through a may_alias type, so the compiler knows each store may overwrite
 * floats of the block: no strict-aliasing problem, and one load plus one
 * store per sample (a memcpy per sample stays a library call in the IDF
 * build). */
typedef int32_t __attribute__((may_alias)) audio_out_sample_alias_t;

static inline const audio_out_sample_t *audio_out_from_float_block_inplace(
    float *buf, size_t samples) {
  audio_out_sample_alias_t *out = (audio_out_sample_alias_t *)(void *)buf;
  for (size_t i = 0; i < samples; ++i) out[i] = audio_out_from_float(buf[i]);
  return (const audio_out_sample_t *)(const void *)buf;
}

/* 16-bit PCM -> 32-bit slot at unity gain (bit-exact; latency chirp only). */
static inline audio_out_sample_t audio_out_from_pcm16(int16_t x) {
  return (int32_t)x * 65536;
}

static inline void audio_out_from_pcm16_block(const int16_t *in,
                                              audio_out_sample_t *out,
                                              size_t samples) {
  for (size_t i = 0; i < samples; ++i) out[i] = audio_out_from_pcm16(in[i]);
}

/* 32-bit slot -> 16-bit view (LED/VU meter only). */
static inline int16_t audio_out_to_pcm16(audio_out_sample_t x) {
  return (int16_t)(x >> 16);
}
