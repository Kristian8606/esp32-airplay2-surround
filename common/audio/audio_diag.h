#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * Compile-time AirPlay diagnostics (menuconfig "AirPlay Logs and
 * Diagnostics").
 *
 * The permanent 2-second audio_status log is not part of this module. When a
 * category is off, the macros below erase both the call and its arguments at
 * preprocessing time: nothing is evaluated and no diagnostic state is linked.
 */

#if defined(CONFIG_AIRPLAY_DIAG_ACTIVE) && CONFIG_AIRPLAY_DIAG_ACTIVE
esp_err_t audio_diag_init(void);
#define AUDIO_DIAG_INIT() audio_diag_init()
#else
#define AUDIO_DIAG_INIT() (ESP_OK)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
typedef enum {
  AUDIO_DIAG_TASK_AAC_PROCESSOR = 1,
  AUDIO_DIAG_TASK_ALAC_STAGE,
  AUDIO_DIAG_TASK_PLAYOUT,
  AUDIO_DIAG_TASK_RT_DATA,
  AUDIO_DIAG_TASK_RT_CTRL,
  AUDIO_DIAG_TASK_RT_WORK,
  AUDIO_DIAG_TASK_RT_RESEND,
  AUDIO_DIAG_TASK_TCP_READER,
} audio_diag_task_id_t;

void audio_diag_lifecycle_task_started(audio_diag_task_id_t task_id, int core,
                                       uint32_t priority, uint32_t extra);
void audio_diag_lifecycle_realtime_stopped(void);
void audio_diag_lifecycle_realtime_epoch(uint32_t generation);
#define AUDIO_DIAG_LIFECYCLE_TASK_STARTED(...) \
  audio_diag_lifecycle_task_started(__VA_ARGS__)
#define AUDIO_DIAG_LIFECYCLE_REALTIME_STOPPED() \
  audio_diag_lifecycle_realtime_stopped()
#define AUDIO_DIAG_LIFECYCLE_REALTIME_EPOCH(...) \
  audio_diag_lifecycle_realtime_epoch(__VA_ARGS__)
#else
#define AUDIO_DIAG_LIFECYCLE_TASK_STARTED(...) do {} while (0)
#define AUDIO_DIAG_LIFECYCLE_REALTIME_STOPPED() do {} while (0)
#define AUDIO_DIAG_LIFECYCLE_REALTIME_EPOCH(...) do {} while (0)
#endif

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
void audio_diag_playout_i2s(uint32_t mclk_hz, uint32_t dma_desc,
                            uint32_t dma_frames, uint32_t isr_iram_safe);
#define AUDIO_DIAG_PLAYOUT_I2S(...) audio_diag_playout_i2s(__VA_ARGS__)
#else
#define AUDIO_DIAG_PLAYOUT_I2S(...) do {} while (0)
#endif
