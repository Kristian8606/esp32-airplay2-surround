#include "pcm_rtp_ring.h"

#include <stdlib.h>
#include <string.h>

#include "audio_diag.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PCM_SLOT_SAMPLES (PCM_RTP_SLOT_FRAMES * PCM_RTP_CHANNELS)
#define VALID_WORDS      (PCM_RTP_SLOT_FRAMES / 32U)

_Static_assert((PCM_RTP_SLOT_COUNT & (PCM_RTP_SLOT_COUNT - 1U)) == 0,
               "PCM_RTP_SLOT_COUNT must be power of two");
_Static_assert((PCM_RTP_FINAL_SLOT_COUNT & (PCM_RTP_FINAL_SLOT_COUNT - 1U)) == 0,
               "PCM_RTP_FINAL_SLOT_COUNT must be power of two");
_Static_assert((PCM_RTP_SLOT_FRAMES & (PCM_RTP_SLOT_FRAMES - 1U)) == 0,
               "PCM_RTP_SLOT_FRAMES must be power of two");
_Static_assert((PCM_RTP_SLOT_FRAMES % 32U) == 0,
               "PCM_RTP_SLOT_FRAMES must be multiple of 32");

typedef struct {
  /* Seqlock: odd while writer changes PCM/tag/validity, even when stable. */
  volatile uint32_t seq;
  uint32_t page_rtp;            /* absolute RTP aligned down to 1024 */
  uint32_t generation;
  uint32_t valid[VALID_WORDS];  /* one bit per stereo PCM frame */
} pcm_slot_tag_t;

struct pcm_rtp_ring {
  SemaphoreHandle_t writer_mutex; /* writers/control only; I2S reads never lock */
  int16_t *pcm;
  bool owns_pcm;
  pcm_slot_tag_t *tags;
  uint32_t slot_count; /* immutable; each ring has its own address mask */
  uint32_t generation;
};

static inline uint32_t page_base(uint32_t rtp) {
  return rtp & ~(PCM_RTP_SLOT_FRAMES - 1U);
}

static inline uint32_t slot_for_page(const pcm_rtp_ring_t *r, uint32_t page_rtp) {
  return (page_rtp >> 10) & (r->slot_count - 1U);
}

static inline int32_t rtp_delta(uint32_t a, uint32_t b) {
  return (int32_t)(a - b);
}

static void validity_clear(uint32_t valid[VALID_WORDS]) {
  memset(valid, 0, VALID_WORDS * sizeof(valid[0]));
}

static void validity_set_range(uint32_t valid[VALID_WORDS], uint32_t off,
                               uint32_t count) {
  while (count) {
    uint32_t word = off >> 5;
    uint32_t bit = off & 31U;
    uint32_t n = 32U - bit;
    if (n > count) {
      n = count;
    }
    uint32_t mask =
        (n == 32U) ? 0xFFFFFFFFU : (((1U << n) - 1U) << bit);
    valid[word] |= mask;
    off += n;
    count -= n;
  }
}


static void validity_clear_range(uint32_t valid[VALID_WORDS], uint32_t off,
                                 uint32_t count) {
  while (count) {
    uint32_t word = off >> 5;
    uint32_t bit = off & 31U;
    uint32_t n = 32U - bit;
    if (n > count) {
      n = count;
    }
    uint32_t mask =
        (n == 32U) ? 0xFFFFFFFFU : (((1U << n) - 1U) << bit);
    valid[word] &= ~mask;
    off += n;
    count -= n;
  }
}

static bool validity_any(const uint32_t valid[VALID_WORDS]) {
  for (uint32_t i = 0; i < VALID_WORDS; ++i) {
    if (valid[i] != 0U) return true;
  }
  return false;
}

static bool validity_any_range(const uint32_t valid[VALID_WORDS], uint32_t off,
                               uint32_t count) {
  while (count) {
    uint32_t word = off >> 5;
    uint32_t bit = off & 31U;
    uint32_t n = 32U - bit;
    if (n > count) n = count;
    uint32_t mask =
        (n == 32U) ? 0xFFFFFFFFU : (((1U << n) - 1U) << bit);
    if (valid[word] & mask) return true;
    off += n;
    count -= n;
  }
  return false;
}

static bool validity_has_range(const uint32_t valid[VALID_WORDS], uint32_t off,
                               uint32_t count) {
  while (count) {
    uint32_t word = off >> 5;
    uint32_t bit = off & 31U;
    uint32_t n = 32U - bit;
    if (n > count) {
      n = count;
    }
    uint32_t mask =
        (n == 32U) ? 0xFFFFFFFFU : (((1U << n) - 1U) << bit);
    if ((valid[word] & mask) != mask) {
      return false;
    }
    off += n;
    count -= n;
  }
  return true;
}

size_t pcm_rtp_ring_storage_bytes(void) {
  return (size_t)PCM_RTP_RING_FRAMES * PCM_RTP_CHANNELS * sizeof(int16_t);
}

static esp_err_t pcm_rtp_ring_create_internal(
    pcm_rtp_ring_t **out, void *storage, size_t storage_bytes,
    uint32_t slot_count) {
  if (!out) {
    return ESP_ERR_INVALID_ARG;
  }
  *out = NULL;

  const size_t pcm_bytes =
      (size_t)slot_count * PCM_SLOT_SAMPLES * sizeof(int16_t);
  if (storage && (storage_bytes < pcm_bytes ||
                  ((uintptr_t)storage % _Alignof(int16_t)) != 0U)) {
    return ESP_ERR_INVALID_ARG;
  }

  pcm_rtp_ring_t *r = calloc(1, sizeof(*r));
  if (!r) {
    return ESP_ERR_NO_MEM;
  }

  r->slot_count = slot_count;
  if (storage) {
    r->pcm = (int16_t *)storage;
    r->owns_pcm = false;
  } else {
    r->pcm = heap_caps_malloc(pcm_bytes,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->owns_pcm = true;
  }
  r->tags = heap_caps_calloc(slot_count, sizeof(pcm_slot_tag_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!r->tags) {
    r->tags = calloc(slot_count, sizeof(pcm_slot_tag_t));
  }
  r->writer_mutex = xSemaphoreCreateMutex();
  if (!r->pcm || !r->tags || !r->writer_mutex) {
    pcm_rtp_ring_destroy(r);
    return ESP_ERR_NO_MEM;
  }

  *out = r;
  return ESP_OK;
}

esp_err_t pcm_rtp_ring_create(pcm_rtp_ring_t **out) {
  return pcm_rtp_ring_create_internal(out, NULL, 0U, PCM_RTP_FINAL_SLOT_COUNT);
}

esp_err_t pcm_rtp_ring_create_with_storage(pcm_rtp_ring_t **out,
                                           void *storage,
                                           size_t storage_bytes) {
  if (!storage) return ESP_ERR_INVALID_ARG;
  return pcm_rtp_ring_create_internal(out, storage, storage_bytes, PCM_RTP_SLOT_COUNT);
}

void pcm_rtp_ring_destroy(pcm_rtp_ring_t *r) {
  if (!r) {
    return;
  }
  if (r->writer_mutex) vSemaphoreDelete(r->writer_mutex);
  if (r->owns_pcm) free(r->pcm);
  free(r->tags);
  free(r);
}

void pcm_rtp_ring_set_generation(pcm_rtp_ring_t *r, uint32_t generation) {
  if (!r) {
    return;
  }
  __atomic_store_n(&r->generation, generation, __ATOMIC_RELEASE);
}

/* A direct-mapped cache may retain tags from an older RTP neighbourhood even
 * after a seek/track switch.  Only valid samples inside the finite addressable
 * future window of the *current* playhead are protected from replacement.
 * Anything empty, behind the cursor, or more than one ring-span ahead is cache
 * history and may be evicted immediately.  This prevents an unrelated RTP
 * universe from looking "future forever" merely because signed 32-bit RTP
 * ordering happens to put its page above wanted_rtp. */
static bool page_has_protected_future(const pcm_rtp_ring_t *r,
                                      const pcm_slot_tag_t *tag,
                                      uint32_t wanted_rtp) {
  if (!validity_any(tag->valid)) return false;

  const int32_t start_delta = rtp_delta(tag->page_rtp, wanted_rtp);
  const int64_t end_delta =
      (int64_t)start_delta + (int64_t)PCM_RTP_SLOT_FRAMES;
  if (end_delta <= 0) return false;
  if ((int64_t)start_delta >= ((int64_t)r->slot_count * PCM_RTP_SLOT_FRAMES)) return false;

  uint32_t off = 0U;
  if (start_delta < 0) off = (uint32_t)(-start_delta);

  uint32_t end_off = PCM_RTP_SLOT_FRAMES;
  const int64_t horizon_left =
      ((int64_t)r->slot_count * PCM_RTP_SLOT_FRAMES) - (int64_t)start_delta;
  if (horizon_left <= 0) return false;
  if (horizon_left < (int64_t)end_off) end_off = (uint32_t)horizon_left;
  if (off >= end_off) return false;
  return validity_any_range(tag->valid, off, end_off - off);
}

/* Acquire a slot writer token with CAS. Readers only trust even sequence
 * values; invalidate_range() uses the same odd/even ownership protocol.
 * Keep retries bounded: if a lower-priority task on the same core was
 * preempted while holding the slot, an unbounded spin here could deadlock it. */
static bool slot_writer_acquire(pcm_slot_tag_t *tag, uint32_t *seq_even) {
  for (int retry = 0; retry < 32; ++retry) {
    uint32_t seq = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq & 1U) {
      continue;
    }
    uint32_t expected = seq;
    if (__atomic_compare_exchange_n(&tag->seq, &expected, seq + 1U, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      *seq_even = seq;
      return true;
    }
  }
  return false;
}

typedef struct {
  pcm_slot_tag_t *tag;
  uint32_t slot;
  uint32_t base;
  uint32_t offset;
  uint32_t chunk;
  size_t src_frame;
  uint32_t seq_even;
  bool locked;
} pcm_write_chunk_t;

static bool pcm_rtp_ring_write_locked(pcm_rtp_ring_t *r, uint32_t first_rtp,
                        const int16_t *pcm, size_t frames, int channels,
                        uint32_t generation, uint32_t wanted_rtp,
                        bool wanted_valid) {
  if (!r || !pcm || channels != (int)PCM_RTP_CHANNELS || frames == 0 ||
      generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return false;
  }

  /* Current producers are AAC (1024 frames) and ALAC (352 frames), therefore
   * one decoded block can touch at most two 1024-frame RTP pages. Keeping that
   * invariant explicit lets us acquire every destination page before copying,
   * so a concurrent FLUSH invalidate can never leave a partially-published AU. */
  if (frames > PCM_RTP_SLOT_FRAMES) {
    return false;
  }

  pcm_write_chunk_t chunks[2] = {0};
  unsigned chunk_count = 0;
  uint32_t cur_rtp = first_rtp;
  size_t remain = frames;
  size_t src_frame = 0;
  while (remain) {
    if (chunk_count >= 2U) {
      return false;
    }
    uint32_t base = page_base(cur_rtp);
    uint32_t offset = cur_rtp - base;
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - offset;
    if ((size_t)chunk > remain) {
      chunk = (uint32_t)remain;
    }
    uint32_t slot = slot_for_page(r, base);
    chunks[chunk_count++] = (pcm_write_chunk_t){
        .tag = &r->tags[slot],
        .slot = slot,
        .base = base,
        .offset = offset,
        .chunk = chunk,
        .src_frame = src_frame,
    };
    src_frame += chunk;
    cur_rtp += chunk;
    remain -= chunk;
  }

  /* Acquire all destination pages first. If invalidate_range() currently owns
   * one of them, publish nothing from this decoded block. */
  for (unsigned i = 0; i < chunk_count; ++i) {
    if (!slot_writer_acquire(chunks[i].tag, &chunks[i].seq_even)) {
      for (unsigned j = 0; j < i; ++j) {
        if (chunks[j].locked) {
          __atomic_store_n(&chunks[j].tag->seq, chunks[j].seq_even + 2U,
                           __ATOMIC_RELEASE);
        }
      }
      return false;
    }
    chunks[i].locked = true;
  }

  /* A session generation can change while we are acquiring pages. Never let a
   * producer from the old timeline publish after the O(1) generation reset. */
  if (generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    for (unsigned j = 0; j < chunk_count; ++j) {
      __atomic_store_n(&chunks[j].tag->seq, chunks[j].seq_even + 2U,
                       __ATOMIC_RELEASE);
    }
    return false;
  }

  /* Collision decisions must be made while we own the tags; a preflight done
   * before acquiring the seqlock can itself race with FLUSH/invalidation. */
  for (unsigned i = 0; i < chunk_count; ++i) {
    pcm_slot_tag_t *tag = chunks[i].tag;
    if (tag->generation == generation && tag->page_rtp != chunks[i].base) {
      /* Empty tags are not occupied.  More importantly, a valid old page only
       * blocks this write when it still contains samples in the current
       * playhead's finite future cache window.  FLUSH can therefore make a
       * physical slot reusable immediately without changing the PCM session
       * generation, and unrelated RTP address spaces cannot pin the ring. */
      if (wanted_valid && page_has_protected_future(r, tag, wanted_rtp)) {
        for (unsigned j = 0; j < chunk_count; ++j) {
          __atomic_store_n(&chunks[j].tag->seq, chunks[j].seq_even + 2U,
                           __ATOMIC_RELEASE);
        }
        return false;
      }
    }
  }

  for (unsigned i = 0; i < chunk_count; ++i) {
    pcm_write_chunk_t *w = &chunks[i];
    pcm_slot_tag_t *tag = w->tag;
    const bool replacing =
        tag->generation != generation || tag->page_rtp != w->base;

    if (replacing) {
      tag->page_rtp = w->base;
      tag->generation = generation;
      validity_clear(tag->valid);
    }

    memcpy(r->pcm + ((size_t)w->slot * PCM_SLOT_SAMPLES) +
               ((size_t)w->offset * PCM_RTP_CHANNELS),
           pcm + (w->src_frame * PCM_RTP_CHANNELS),
           (size_t)w->chunk * PCM_RTP_CHANNELS * sizeof(int16_t));
    validity_set_range(tag->valid, w->offset, w->chunk);
  }

  for (unsigned i = 0; i < chunk_count; ++i) {
    __atomic_store_n(&chunks[i].tag->seq, chunks[i].seq_even + 2U,
                     __ATOMIC_RELEASE);
  }
  return true;
}

/* Serialize all tag writers so FLUSH cannot silently skip an owned slot.
 * Generation changes remain O(1), nonblocking, and safe under state_mux. */
bool pcm_rtp_ring_write(pcm_rtp_ring_t *r, uint32_t first_rtp,
                        const int16_t *pcm, size_t frames, int channels,
                        uint32_t generation, uint32_t wanted_rtp,
                        bool wanted_valid) {
  if (!r) return false;
  xSemaphoreTake(r->writer_mutex, portMAX_DELAY);
  bool ok = pcm_rtp_ring_write_locked(r, first_rtp, pcm, frames, channels,
                                     generation, wanted_rtp, wanted_valid);
  xSemaphoreGive(r->writer_mutex);
  return ok;
}

static bool read_page_range(const pcm_rtp_ring_t *r, uint32_t rtp,
                            uint32_t generation, uint32_t frames,
                            int16_t *out) {
  uint32_t base = page_base(rtp);
  uint32_t offset = rtp - base;
  if (offset + frames > PCM_RTP_SLOT_FRAMES) {
    return false;
  }

  uint32_t slot = slot_for_page(r, base);
  const pcm_slot_tag_t *tag = &r->tags[slot];
  for (int retry = 0; retry < 2; ++retry) {
    uint32_t seq1 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 & 1U) {
      continue;
    }
    if (tag->generation != generation || tag->page_rtp != base ||
        !validity_has_range(tag->valid, offset, frames)) {
      return false;
    }

    memcpy(out,
           r->pcm + ((size_t)slot * PCM_SLOT_SAMPLES) +
               ((size_t)offset * PCM_RTP_CHANNELS),
           (size_t)frames * PCM_RTP_CHANNELS * sizeof(int16_t));

    uint32_t seq2 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 == seq2 && !(seq2 & 1U)) {
      return true;
    }
  }
  return false;
}

static bool has_page_range(const pcm_rtp_ring_t *r, uint32_t rtp,
                           uint32_t generation, uint32_t frames) {
  uint32_t base = page_base(rtp);
  uint32_t offset = rtp - base;
  if (offset + frames > PCM_RTP_SLOT_FRAMES) {
    return false;
  }

  uint32_t slot = slot_for_page(r, base);
  const pcm_slot_tag_t *tag = &r->tags[slot];
  for (int retry = 0; retry < 2; ++retry) {
    uint32_t seq1 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 & 1U) {
      continue;
    }
    if (tag->generation != generation || tag->page_rtp != base ||
        !validity_has_range(tag->valid, offset, frames)) {
      return false;
    }
    uint32_t seq2 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 == seq2 && !(seq2 & 1U)) {
      return true;
    }
  }
  return false;
}

bool pcm_rtp_ring_has_range(const pcm_rtp_ring_t *r, uint32_t first_rtp,
                            uint32_t frames, uint32_t generation) {
  if (!r || frames == 0 || generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return false;
  }
  uint32_t cur = first_rtp;
  uint32_t remain = frames;
  while (remain) {
    uint32_t offset = cur & (PCM_RTP_SLOT_FRAMES - 1U);
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - offset;
    if (chunk > remain) {
      chunk = remain;
    }
    if (!has_page_range(r, cur, generation, chunk)) {
      return false;
    }
    cur += chunk;
    remain -= chunk;
  }
  return true;
}

uint32_t pcm_rtp_ring_contiguous_frames(const pcm_rtp_ring_t *r,
                                        uint32_t first_rtp,
                                        uint32_t generation,
                                        uint32_t max_frames) {
  if (!r || max_frames == 0U ||
      generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return 0U;
  }

  uint32_t total = 0U;
  uint32_t cur = first_rtp;
  while (total < max_frames) {
    const uint32_t base = page_base(cur);
    const uint32_t offset = cur - base;
    const uint32_t slot = slot_for_page(r, base);
    const pcm_slot_tag_t *tag = &r->tags[slot];
    uint32_t valid_copy[VALID_WORDS];
    bool stable = false;

    for (int retry = 0; retry < 3; ++retry) {
      const uint32_t seq1 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
      if (seq1 & 1U) continue;
      const uint32_t tag_generation = tag->generation;
      const uint32_t tag_page_rtp = tag->page_rtp;
      memcpy(valid_copy, tag->valid, sizeof(valid_copy));
      const uint32_t seq2 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
      if (seq1 != seq2 || (seq2 & 1U)) continue;
      if (tag_generation != generation || tag_page_rtp != base) return total;
      stable = true;
      break;
    }
    if (!stable) return total;

    uint32_t limit = PCM_RTP_SLOT_FRAMES - offset;
    const uint32_t remain = max_frames - total;
    if (limit > remain) limit = remain;
    uint32_t bit = offset;
    while (limit) {
      const uint32_t shift = bit & 31U;
      uint32_t count = 32U - shift;
      if (count > limit) count = limit;
      const uint32_t mask = count == 32U ? UINT32_MAX : (1U << count) - 1U;
      const uint32_t holes = ~(valid_copy[bit >> 5] >> shift) & mask;
      if (holes) {
        return total + (uint32_t)__builtin_ctz(holes);
      }
      total += count;
      cur += count;
      bit += count;
      limit -= count;
    }
  }
  return total;
}

bool pcm_rtp_ring_read(const pcm_rtp_ring_t *r, uint32_t first_rtp,
                       uint32_t frames, uint32_t generation, int16_t *out) {
  if (!r || !out || frames == 0 ||
      generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return false;
  }

  uint32_t cur = first_rtp;
  uint32_t remain = frames;
  size_t out_frame = 0;
  while (remain) {
    uint32_t offset = cur & (PCM_RTP_SLOT_FRAMES - 1U);
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - offset;
    if (chunk > remain) {
      chunk = remain;
    }
    if (!read_page_range(r, cur, generation, chunk,
                         out + out_frame * PCM_RTP_CHANNELS)) {
      return false;
    }
    cur += chunk;
    remain -= chunk;
    out_frame += chunk;
  }
  return true;
}

bool pcm_rtp_ring_read_256(const pcm_rtp_ring_t *r, uint32_t first_rtp,
                           uint32_t generation, int16_t *out) {
  return pcm_rtp_ring_read(r, first_rtp, 256U, generation, out);
}

static bool read_page_range_conceal(const pcm_rtp_ring_t *r, uint32_t rtp,
                                    uint32_t generation, uint32_t frames,
                                    int16_t *out, uint32_t *missing) {
  const uint32_t base = page_base(rtp);
  const uint32_t offset = rtp - base;
  if (offset + frames > PCM_RTP_SLOT_FRAMES) return false;

  const uint32_t slot = slot_for_page(r, base);
  const pcm_slot_tag_t *tag = &r->tags[slot];
  uint32_t valid_copy[VALID_WORDS];

  for (int retry = 0; retry < 2; ++retry) {
    const uint32_t seq1 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 & 1U) continue;
    if (tag->generation != generation || tag->page_rtp != base) return false;

    memcpy(out,
           r->pcm + ((size_t)slot * PCM_SLOT_SAMPLES) +
               ((size_t)offset * PCM_RTP_CHANNELS),
           (size_t)frames * PCM_RTP_CHANNELS * sizeof(int16_t));
    memcpy(valid_copy, tag->valid, sizeof(valid_copy));

    const uint32_t seq2 = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    if (seq1 != seq2 || (seq2 & 1U)) continue;

    uint32_t local_missing = 0U;
    for (uint32_t i = 0; i < frames; ++i) {
      const uint32_t bit = offset + i;
      if ((valid_copy[bit >> 5] & (1U << (bit & 31U))) == 0U) {
        out[(size_t)i * PCM_RTP_CHANNELS] = 0;
        out[(size_t)i * PCM_RTP_CHANNELS + 1U] = 0;
        local_missing++;
      }
    }
    if (missing) *missing += local_missing;
    return true;
  }
  return false;
}

bool pcm_rtp_ring_read_256_conceal(const pcm_rtp_ring_t *r,
                                   uint32_t first_rtp, uint32_t generation,
                                   int16_t *out, uint32_t *out_missing_frames) {
  if (out_missing_frames) *out_missing_frames = 0U;
  if (!r || !out ||
      generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    return false;
  }

  uint32_t cur = first_rtp;
  uint32_t remain = 256U;
  size_t out_frame = 0U;
  uint32_t missing = 0U;
  while (remain) {
    const uint32_t offset = cur & (PCM_RTP_SLOT_FRAMES - 1U);
    uint32_t chunk = PCM_RTP_SLOT_FRAMES - offset;
    if (chunk > remain) chunk = remain;
    if (!read_page_range_conceal(
            r, cur, generation, chunk,
            out + out_frame * PCM_RTP_CHANNELS, &missing)) {
      return false;
    }
    cur += chunk;
    remain -= chunk;
    out_frame += chunk;
  }
  if (out_missing_frames) *out_missing_frames = missing;
  return true;
}

void pcm_rtp_ring_invalidate_range(pcm_rtp_ring_t *r, uint32_t from_rtp,
                                   uint32_t until_rtp, uint32_t generation) {
  const int32_t length = (int32_t)(until_rtp - from_rtp);
  if (!r || length <= 0) return;

  xSemaphoreTake(r->writer_mutex, portMAX_DELAY);
  /* Visit physical tags once, even if the sender names hours of RTP time. */
  for (uint32_t i = 0; i < r->slot_count; ++i) {
    pcm_slot_tag_t *tag = &r->tags[i];
    if (tag->generation != generation) continue;
    const int64_t start = (int32_t)(tag->page_rtp - from_rtp);
    const int64_t end = start + PCM_RTP_SLOT_FRAMES;
    if (end <= 0 || start >= length) continue;
    const uint32_t off = start < 0 ? (uint32_t)(-start) : 0U;
    const uint32_t stop = end > length ? (uint32_t)(length - start)
                                      : PCM_RTP_SLOT_FRAMES;
    const uint32_t seq = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    (void)__atomic_exchange_n(&tag->seq, seq + 1U, __ATOMIC_ACQ_REL);
    validity_clear_range(tag->valid, off, stop - off);
    if (!validity_any(tag->valid)) {
      tag->page_rtp = 0U;
      tag->generation = 0U;
    }
    __atomic_store_n(&tag->seq, seq + 2U, __ATOMIC_RELEASE);
  }
  xSemaphoreGive(r->writer_mutex);
}

void pcm_rtp_ring_invalidate_before(pcm_rtp_ring_t *r, uint32_t until_rtp,
                                    uint32_t generation) {
  if (!r) return;
  xSemaphoreTake(r->writer_mutex, portMAX_DELAY);
  if (generation != __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE)) {
    xSemaphoreGive(r->writer_mutex);
    return;
  }
  for (uint32_t i = 0; i < r->slot_count; ++i) {
    pcm_slot_tag_t *tag = &r->tags[i];
    if (tag->generation != generation) continue;
    const int32_t before = rtp_delta(until_rtp, tag->page_rtp);
    if (before <= 0) continue;
    const uint32_t count = before >= (int32_t)PCM_RTP_SLOT_FRAMES
                              ? PCM_RTP_SLOT_FRAMES : (uint32_t)before;
    const uint32_t seq = __atomic_load_n(&tag->seq, __ATOMIC_ACQUIRE);
    (void)__atomic_exchange_n(&tag->seq, seq + 1U, __ATOMIC_ACQ_REL);
    validity_clear_range(tag->valid, 0U, count);
    if (!validity_any(tag->valid)) {
      tag->page_rtp = 0U;
      tag->generation = 0U;
    }
    __atomic_store_n(&tag->seq, seq + 2U, __ATOMIC_RELEASE);
  }
  xSemaphoreGive(r->writer_mutex);
}
