#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#ifndef PCM_FIFO_HOST_TEST
#include "sdkconfig.h"
#endif

/*
 * Final playout PCM store: one contiguous circular FIFO of decoded stereo
 * PCM in play order, as float (1.0 = full scale; see audio_out_sample.h).
 *
 * The FIFO holds the RTP window [head, tail). Writers (buffered AAC decoder,
 * realtime ALAC stage) append at tail; the I2S playout task reads a block at
 * the RTP it needs. A read is a plain copy of at most two contiguous pieces:
 * there are no per-page tags, seqlocks or validity bitmaps.
 *
 *  - A write that starts after tail fills the gap with silence first (a lost
 *    packet plays as silence, exactly as an unfilled RTP slot did before).
 *  - Frames once written are never changed: a write that overlaps
 *    [head, tail) keeps the frames already there and appends only the rest
 *    (as Shairport Sync's player buffer keeps blocks in arrival order).
 *    A timeline the sender re-sends (AutoMix) replaces them explicitly:
 *    pcm_fifo_truncate() at its first RTP, then ordinary appends.
 *  - Reads do not consume. The oldest frames are dropped only when a writer
 *    needs room, and then only frames the playhead has already passed
 *    (wanted_valid) - pause/resume can replay PCM queued in the DMA.
 *  - set_generation() is O(1) and lock-free (safe inside a critical section):
 *    the FIFO is empty for every other generation until the next write.
 *
 * Single reader (playout) without locks; writers and control operations are
 * serialised by an internal mutex. Appends never touch frames the reader can
 * see; the rare window resets (FLUSH, truncate, new position) are short
 * critical sections the reader detects with a sequence counter and waits
 * out.
 */

#define PCM_FIFO_CHANNELS 2U
/* 2^17 frames = 2.97 s at 44.1 kHz, 1 MiB of float in PSRAM: the same horizon
 * as the former RTP-addressed ring (realtime ALAC publishes up to its
 * latency). */
#define PCM_FIFO_FRAMES (1U << 17)

/* One stored sample. Silence (gaps, frames outside the window) is 0.0f,
 * i.e. all-zero bytes. */
typedef float pcm_fifo_sample_t;

typedef struct pcm_fifo pcm_fifo_t;

esp_err_t pcm_fifo_create(pcm_fifo_t **out);
void pcm_fifo_destroy(pcm_fifo_t *fifo);

/* O(1), lock-free. */
void pcm_fifo_set_generation(pcm_fifo_t *fifo, uint32_t generation);

/*
 * Store `frames` frames (<= 4096) of stereo PCM starting at RTP `rtp`.
 * Returns false only when there is no room: the frames needed would drop PCM
 * at or after `wanted_rtp` (the frame audible now) that has not played yet.
 * Without a valid playhead the oldest frames are dropped as needed. A write
 * that lies entirely before the window is ignored (returns true).
 */
bool pcm_fifo_write(pcm_fifo_t *fifo, uint32_t rtp, const pcm_fifo_sample_t *pcm,
                    size_t frames, uint32_t generation, uint32_t wanted_rtp,
                    bool wanted_valid);

/*
 * Copy [rtp, rtp + frames) into out_stereo. Frames outside the window are
 * written as silence. Returns the number of frames that came from the FIFO
 * (frames == full block, 0 == nothing). Never blocks.
 */
uint32_t pcm_fifo_read(const pcm_fifo_t *fifo, uint32_t rtp, uint32_t frames,
                       uint32_t generation, pcm_fifo_sample_t *out_stereo);

/* True when the whole range is inside the window. No copy. */
bool pcm_fifo_has_range(const pcm_fifo_t *fifo, uint32_t rtp, uint32_t frames,
                        uint32_t generation);

/* Frames available from rtp up to tail (0 when rtp is outside the window). */
uint32_t pcm_fifo_contiguous_frames(const pcm_fifo_t *fifo, uint32_t rtp,
                                    uint32_t generation, uint32_t max_frames);

/* Immediate FLUSH: drop every frame before until_rtp. */
void pcm_fifo_invalidate_before(pcm_fifo_t *fifo, uint32_t until_rtp,
                                uint32_t generation);

/* Drop every frame from from_rtp on (the newest end of the window). */
void pcm_fifo_truncate(pcm_fifo_t *fifo, uint32_t from_rtp, uint32_t generation);

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
/* Take and clear silence inserted for RTP gaps since the previous snapshot.
 * Frames are stereo frames, not individual channel samples. Counts describe
 * inserted PCM, which a later FLUSH may discard before it is heard. Normal
 * zero-valued PCM and silence returned outside the window are not counted.
 * Takes the writer mutex; call from the status task, never a critical section.
 * Generation/position changes preserve pending counts. */
void pcm_fifo_diag_take(pcm_fifo_t *fifo, uint32_t *gap_events,
                        uint32_t *gap_frames);
#endif
