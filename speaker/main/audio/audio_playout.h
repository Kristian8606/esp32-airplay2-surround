#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "audio_out_sample.h"

/* I2S playout backend: AUDIO_PLAYOUT_DMA_BLOCKS DMA descriptors of
 * AUDIO_PLAYOUT_FRAMES frames each. Every application audio block (one
 * descriptor) is tagged with its RTP/generation before it reaches DMA. The TX
 * EOF ISR records the local completion timestamp for that exact tag, allowing
 * the playout task to compare the physical DMA completion edge against
 * AirPlay PTP.
 *
 * 480 frames x 8 bytes (32-bit stereo slots) = 3840 bytes, inside the IDF
 * limit of 4092 bytes per descriptor. 8 descriptors = 30 KiB internal DMA RAM
 * and ~87 ms of audio queued at 44.1 kHz. The playout task wakes once per
 * AUDIO_PLAYOUT_BATCH free descriptors and then writes that many blocks
 * without blocking.
 */

#define AUDIO_PLAYOUT_FRAMES     480U
#define AUDIO_PLAYOUT_DMA_BLOCKS 8U
#define AUDIO_PLAYOUT_BATCH      2U

typedef struct {
  uint32_t rtp;
  uint32_t generation;
  uint32_t frames;
  int64_t done_local_us;
} audio_playout_completion_t;

typedef struct {
  int32_t requested_ppm;
  int32_t actual_delta_mclk_hz;
  uint32_t curr_mclk_hz;
} audio_playout_tune_info_t;



esp_err_t audio_playout_init(void);
esp_err_t audio_playout_flush(void);

/* Preload is valid only while the TX channel is disabled/READY. It is used for
 * the first AUDIO_PLAYOUT_DMA_BLOCKS blocks of an epoch so the channel starts
 * with known audio in every DMA descriptor instead of clocking zero-filled
 * descriptors first (each EOF must have a tag). */
esp_err_t audio_playout_preload_tagged(const audio_out_sample_t *stereo,
                                       uint32_t frames,
                                       uint32_t rtp, uint32_t generation);

/* Enable after all DMA blocks have been preloaded and the PTP start edge has
 * been reached. */
esp_err_t audio_playout_enable(void);

/* Normal steady-state write. The RTP tag is paired FIFO-wise with the TX EOF
 * callback for the descriptor that physically finished sending. */
esp_err_t audio_playout_write_tagged(const audio_out_sample_t *stereo,
                                     uint32_t frames,
                                     uint32_t rtp, uint32_t generation);

/* Drain one completed tagged DMA block. done_local_us comes from
 * esp_timer_get_time() inside the TX ISR. */
bool audio_playout_poll_completion(audio_playout_completion_t *out);

/* Sticky until a successful flush. Never use completions after a FIFO fault. */
bool audio_playout_has_fault(void);
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
/* Counts since the last call: DMA underruns, EOF tag faults. */
void audio_playout_diag_take(uint32_t *underruns, uint32_t *tag_faults);
#endif
/* Single playout owner only; timeout is in FreeRTOS ticks. */
bool audio_playout_wait_completion(audio_playout_completion_t *out,
                                    uint32_t timeout_ticks);

bool audio_playout_is_enabled(void);

/* Descriptors free for writing right now (DMA blocks - queued tagged blocks). */
uint32_t audio_playout_free_blocks(void);

/* Single playout owner only: sleep until at least `blocks` descriptors are
 * free (the TX ISR wakes the task only then), a fault, or the timeout.
 * Returns true when that many descriptors are free. */
bool audio_playout_wait_free(uint32_t blocks, uint32_t timeout_ticks);

#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
/* Single playout owner only. Soft stop with the clocks running: the audio
 * already queued in the DMA fades out in place (S-curve, ~55-75 ms: what is
 * queued minus the block playing now) and ends in exact zeros; if that is not
 * possible it is cut to zeros at once. The queued tags become silence tags
 * (generation 0, RTP 0) and completions already reported are dropped. The
 * caller keeps feeding tagged zero blocks afterwards. *faded_frames = frames
 * faded (0: cut). ESP_ERR_NOT_FOUND before the first DMA round (descriptors
 * not known yet), ESP_ERR_INVALID_STATE when disabled or after a fault: stop
 * I2S instead. */
esp_err_t audio_playout_fade_out_queued(uint32_t *faded_frames);
#endif

/* Fine tune the TX master clock while I2S keeps running: only the
 * fractional MCLK divider register is written (no disable/enable, the DMA is
 * not touched). The two divider levels around target_ppm are alternated per
 * written block, so the average clock is exactly target_ppm relative to the
 * nominal MCLK measured at init (+/-160 ppm). Call only from the playout task. ESP_ERR_NOT_SUPPORTED when
 * live tuning is unavailable (see the boot log). halt_us = time spent. */
esp_err_t audio_playout_tune_ppm(int32_t target_ppm,
                                 audio_playout_tune_info_t *out,
                                 uint32_t *halt_us);
/* Reset the MCLK correction to nominal (hard audio-session boundary, not
 * normal track/anchor changes inside one session). */
esp_err_t audio_playout_reset_tune(void);

/* True when the clock can be tuned: the fractional MCLK divider register is
 * rewritten while I2S keeps running (checked at init). */
bool audio_playout_tune_is_live(void);

/* Current I2S sample rate (Hz). */
uint32_t audio_playout_get_rate(void);
/* Switch the I2S sample rate: stops the channel (like audio_playout_flush),
 * reprograms the clock and resets the MCLK correction to the new nominal.
 * No-op when already at rate_hz. Playout task only, like flush. */
esp_err_t audio_playout_set_rate(uint32_t rate_hz);
