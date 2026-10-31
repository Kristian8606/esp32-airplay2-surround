#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Direct RTP-addressed PCM store: the realtime (type 96) ALAC staging ring,
 * 128 slots x 1024 stereo frames = 2.97 s @ 44.1 kHz on caller-owned storage.
 * A slot is keyed by absolute RTP page base (rtp & ~1023). RTP itself is the
 * address: no sorting, no scan, no linked list, no binary search.
 *
 * A packet touches at most two slots. Partial slots are tracked with a
 * 1024-bit validity bitmap so missing PCM is never mistaken for valid audio.
 */

#define PCM_RTP_SLOT_FRAMES 1024U
#define PCM_RTP_SLOT_COUNT  128U /* caller-backed ALAC staging */
#define PCM_RTP_RING_FRAMES (PCM_RTP_SLOT_FRAMES * PCM_RTP_SLOT_COUNT)
#define PCM_RTP_CHANNELS    2U

typedef struct pcm_rtp_ring pcm_rtp_ring_t;

/* Size of the 128-page ALAC staging stereo PCM backing array only. Tags/mutex/control state
 * are deliberately excluded and retain their normal ownership. */
size_t pcm_rtp_ring_storage_bytes(void);

/* Construct the 128-page ALAC staging ring on caller-owned PCM storage. The
 * storage must remain valid until the ring is destroyed and must not be
 * concurrently reused by another codec while this ring is active. */
esp_err_t pcm_rtp_ring_create_with_storage(pcm_rtp_ring_t **out,
                                           void *storage,
                                           size_t storage_bytes);
void pcm_rtp_ring_destroy(pcm_rtp_ring_t *ring);

/* O(1) hard media/session invalidation. Buffered seek/anchor changes do NOT
 * use this: cached PCM stays addressable until an explicit FLUSH invalidates
 * it or the ring naturally overwrites it. */
void pcm_rtp_ring_set_generation(pcm_rtp_ring_t *ring, uint32_t generation);

/*
 * Write decoded stereo PCM at its exact RTP address.
 *
 * If wanted_valid is true, a write is rejected only when it would overwrite
 * still-valid PCM inside the current playhead's finite future cache window.
 * Empty/invalid pages and pages from an unrelated RTP neighbourhood are cache
 * history and are immediately reusable.  The caller may wait here; TCP remains
 * independent and is stopped only by the compressed-store backpressure point.
 */
bool pcm_rtp_ring_write(pcm_rtp_ring_t *ring, uint32_t first_rtp,
                        const int16_t *pcm, size_t frames, int channels,
                        uint32_t generation, uint32_t wanted_rtp,
                        bool wanted_valid);

/* Exact RTP lookup for an arbitrary contiguous stereo range. */
bool pcm_rtp_ring_read(const pcm_rtp_ring_t *ring, uint32_t first_rtp,
                       uint32_t frames, uint32_t generation,
                       int16_t *out_stereo);

/* Low-frequency lock-free diagnostic: count contiguous valid frames starting
 * at first_rtp. It uses the existing page seqlock and never blocks a producer. */
uint32_t pcm_rtp_ring_contiguous_frames(const pcm_rtp_ring_t *ring,
                                        uint32_t first_rtp,
                                        uint32_t generation,
                                        uint32_t max_frames);


/* Immediate FLUSHBUFFERED control-path operation: invalidate every cached PCM
 * sample strictly before until_rtp in wrap-safe RTP order. The bytes are not
 * erased; validity metadata alone is changed so playout/decoder never waits on
 * physical clearing. */
void pcm_rtp_ring_invalidate_before(pcm_rtp_ring_t *ring, uint32_t until_rtp,
                                    uint32_t generation);

