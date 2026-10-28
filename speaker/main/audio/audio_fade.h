#pragma once

/* Fades around a stop (pure C, no IDF calls, host-tested).
 *
 * Fade-out: on pause, FLUSH, seek and track change the audio already queued
 * in the I2S DMA is faded to exactly zero in place, then zeros follow. The TX
 * ISR tracks the DMA ring (audio_dma_ring_track) so the playout task knows
 * which descriptor plays now and which ones are still waiting.
 *
 * Fade-in: every start after a stop rises from 0 over a fixed length.
 *
 * Both use the S-curve smoothstep(t) = t^2 (3 - 2t): flat at both ends, so
 * neither the start nor the end of a fade makes a corner in the waveform. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_out_sample.h"

#if defined(ESP_PLATFORM)
#include "esp_attr.h" /* IRAM_ATTR: the ring tracker runs in the TX ISR */
#elif !defined(IRAM_ATTR)
#define IRAM_ATTR /* host test */
#endif

static inline float audio_fade_smoothstep(float t) {
  return t * t * (3.0f - 2.0f * t);
}

/* Gain of frame j (1-based, 1..total) of a fade-in, inv = 1 / total: the
 * last frame is exactly 1. A fade-out uses 1 - this: its last frame is
 * exactly 0. The loops pass inv computed once: the ESP32-S3 FPU has no
 * divide, a float division per frame is a library call. */
static inline float audio_fade_gain_inv(uint32_t j, uint32_t total, float inv) {
  return j >= total ? 1.0f : audio_fade_smoothstep((float)j * inv);
}

static inline float audio_fade_in_gain(uint32_t j, uint32_t total) {
  return audio_fade_gain_inv(j, total, 1.0f / (float)total);
}

/* Fade-in on float stereo (1.0 = full scale) in place: frames pos+1 ..
 * pos+frames of a fade of `total` frames; frames after the end are left
 * untouched (bit-exact). Returns the new position. */
static inline uint32_t audio_fade_in_f32(float *pcm, uint32_t frames,
                                         uint32_t pos, uint32_t total) {
  const float inv = 1.0f / (float)total;
  for (uint32_t f = 0; f < frames && pos < total; ++f) {
    const float g = audio_fade_gain_inv(++pos, total, inv);
    pcm[2U * f] *= g;
    pcm[2U * f + 1U] *= g;
  }
  return pos;
}

/* Leading digital silence before a fade-in: count the frames at the start
 * of pcm that are exactly zero, at most *wait of them, and take them from
 * *wait. The first AAC block of a play sequence is muted to zeros (decoder
 * start-up) and a start can fall inside it: the fade-in must begin where the
 * sound begins, or the music enters at a gain the fade has already reached
 * (a click). */
static inline uint32_t audio_fade_in_skip_silence(const float *pcm,
                                                  uint32_t frames,
                                                  uint32_t *wait) {
  uint32_t f = 0;
  while (f < frames && *wait > 0U && pcm[2U * f] == 0.0f &&
         pcm[2U * f + 1U] == 0.0f) {
    ++f;
    --*wait;
  }
  return f;
}

/* One block of a fade-in after a start: *pos = frames already faded (0: not
 * started), *wait = leading zero frames still allowed before the fade starts.
 * Until the fade starts, exact zeros pass through and only cost *wait; the
 * fade then starts at the first sound (or when *wait runs out). */
static inline void audio_fade_in_block(float *pcm, uint32_t frames,
                                       uint32_t *pos, uint32_t *wait,
                                       uint32_t total) {
  if (*pos >= total) return;
  uint32_t f = 0U;
  if (*pos == 0U) f = audio_fade_in_skip_silence(pcm, frames, wait);
  *pos = audio_fade_in_f32(pcm + 2U * f, frames - f, *pos, total);
}

/* Fade-out on 32-bit stereo I2S slots in place: frames j+1 .. j+frames of a
 * fade of `total` frames, gain 1 - smoothstep; the last frame of the fade is
 * exactly 0. Returns the new j. */
static inline uint32_t audio_fade_out_s32(audio_out_sample_t *x,
                                          uint32_t frames, uint32_t j,
                                          uint32_t total) {
  const float inv = 1.0f / (float)total;
  for (uint32_t f = 0; f < frames && j < total; ++f) {
    const float g = (1.0f - audio_fade_gain_inv(++j, total, inv)) *
                    (1.0f / 2147483648.0f);
    x[2U * f] = audio_out_from_float((float)x[2U * f] * g);
    x[2U * f + 1U] = audio_out_from_float((float)x[2U * f + 1U] * g);
  }
  return j;
}

/* ---- DMA ring tracking (written by the TX ISR only) ---- */

#define AUDIO_DMA_RING_MAX     8U
#define AUDIO_DMA_IDX_UNKNOWN  0xFFFFFFFFU

typedef struct {
  void *buf[AUDIO_DMA_RING_MAX]; /* descriptor buffers, in ring order */
  uint32_t count;                /* learned so far */
  uint32_t size;                 /* bytes per buffer */
  uint32_t last;                 /* ring index that finished last */
  uint32_t bad;                  /* 1: ring order broken once, never fade */
  int64_t last_us;               /* its EOF time */
} audio_dma_ring_t;

/* After a (re)start the first EOF may be any descriptor. */
static inline void audio_dma_ring_restart(audio_dma_ring_t *r) {
  r->last = AUDIO_DMA_IDX_UNKNOWN;
}

/* One EOF of buffer p. Learns the n descriptors in ring order (consecutive
 * EOFs) and from then on checks that every EOF is the next one in the ring;
 * one mismatch sets bad for good. */
static inline void IRAM_ATTR audio_dma_ring_track(audio_dma_ring_t *r,
                                                  uint32_t n_ring, void *p,
                                                  uint32_t size,
                                                  int64_t eof_us) {
  if (!p || n_ring == 0U || n_ring > AUDIO_DMA_RING_MAX) return;
  const uint32_t n = r->count;
  const uint32_t last = r->last;
  uint32_t idx = AUDIO_DMA_IDX_UNKNOWN;
  if (last < n_ring && n == n_ring && r->buf[(last + 1U) % n_ring] == p) {
    idx = (last + 1U) % n_ring; /* the normal case: one compare */
  } else {
    for (uint32_t i = 0; i < n; ++i) {
      if (r->buf[i] == p) {
        idx = i;
        break;
      }
    }
    if (idx == AUDIO_DMA_IDX_UNKNOWN) {
      if (n >= n_ring || size == 0U) { /* a buffer too many: never expected */
        r->bad = 1U;
        r->last = AUDIO_DMA_IDX_UNKNOWN;
        return;
      }
      r->buf[n] = p; /* learn the next one in ring order */
      if (n == 0U) r->size = size;
      idx = n;
      __atomic_store_n(&r->count, n + 1U, __ATOMIC_RELEASE);
    }
    if (last != AUDIO_DMA_IDX_UNKNOWN && idx != (last + 1U) % n_ring)
      r->bad = 1U;
  }
  r->last = idx;
  r->last_us = eof_us;
}

/* Which queued descriptors can still be faded. queued = tagged descriptors
 * in flight, the one playing now included (it is ring index last+1).
 * elapsed_us = time since the last EOF. The playing one, and any the DMA
 * starts within guard_us, play as they are. Returns how many descriptors to
 * fade; *first = offset from last of the first one to fade. */
static inline uint32_t audio_fade_out_plan(uint32_t queued, int64_t elapsed_us,
                                           int64_t block_us, int64_t guard_us,
                                           uint32_t *first) {
  /* In flight: offsets 1..queued from the last finished descriptor; offset 1
   * is the one the DMA reads now and plays as it is, like any further one
   * the DMA starts within guard_us. The fade covers *first..queued: the
   * newest queued block always ends in exact zero. */
  if (block_us <= 0 || queued < 2U) return 0U;
  int64_t skip = 0;
  if (elapsed_us > 0) skip = (elapsed_us + guard_us) / block_us;
  if (skip > (int64_t)queued) return 0U;
  const uint32_t f = 2U + (uint32_t)skip;
  if (f > queued) return 0U;
  if (first) *first = f;
  return queued - f + 1U;
}

/* Fade the planned descriptors in place: offsets first .. first+count-1
 * from `last` in a ring of n_ring buffers of `frames` 32-bit stereo frames,
 * nearest first, as one continuous fade. Returns the frames faded. */
static inline uint32_t audio_fade_out_ring(void *const *bufs, uint32_t n_ring,
                                           uint32_t last, uint32_t first,
                                           uint32_t count, uint32_t frames) {
  const uint32_t total = count * frames;
  uint32_t j = 0U;
  for (uint32_t b = 0; b < count; ++b) {
    j = audio_fade_out_s32(
        (audio_out_sample_t *)bufs[(last + first + b) % n_ring], frames, j,
        total);
  }
  return j;
}
