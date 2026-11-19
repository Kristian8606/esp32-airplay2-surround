#pragma once

/* Gateway: the room calibration microphone (INMP441 on I2S, menuconfig
 * Surround Build Role -> Room calibration microphone).
 *
 * One recording at a time: gw_mic_start() records into PSRAM until
 * gw_mic_stop(); the samples and their times stay available until the next
 * start. Sample times come from the DMA completion times (a line through
 * them, its lower envelope), like the speaker's wired latency test. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define GW_MIC_RATE_HZ 48000U

/* Up to max_seconds, in one block of PSRAM; when that is not free, the
 * longest that fits if it is at least min_seconds, else ESP_ERR_NO_MEM.
 * ESP_ERR_NOT_SUPPORTED when built without the microphone. */
esp_err_t gw_mic_start(uint32_t min_seconds, uint32_t max_seconds);
void gw_mic_stop(void);
/* Frees the recording (~1 MiB after a long one); the next start allocates
 * again. Nothing while recording. */
void gw_mic_release(void);

/* The last recording, 16 bits per sample (full scale GW_MIC_FULL_SCALE):
 * 10 s are ~940 KiB of PSRAM in one block, which is there after the USB
 * player too (as floats, ~1.9 MiB were not). The INMP441's noise and any
 * room's are far above the 16-bit step (-101 dBFS). */
#define GW_MIC_FULL_SCALE 32768.0f
const int16_t *gw_mic_samples(uint32_t *count);
/* esp_timer time (ns) of sample index (fractional) of the last recording;
 * false when there were too few DMA completions for a line. */
bool gw_mic_sample_time_ns(double index, int64_t *local_ns);
/* The inverse: fractional sample index at a local time. */
bool gw_mic_sample_at(int64_t local_ns, double *index);
