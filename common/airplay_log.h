#pragma once

/* Runtime event logs (RTSP, PTP, audio path and the other modules).
 *
 * Start-up messages, the 2-second audio_status line, SERVO CENTER and all
 * errors (ESP_LOGE) always use ESP_LOG* directly. Everything that would log
 * from a running RTSP/PTP/audio function uses AP_LOG* instead and exists in
 * the firmware only with CONFIG_AIRPLAY_LOG_RUNTIME. When the option is off
 * the call and its arguments are discarded at compile time: the format string
 * is still type-checked, but no code or string is emitted and no argument is
 * evaluated. */

#include <stdio.h>

#include "esp_log.h"
#include "sdkconfig.h"

#if defined(CONFIG_AIRPLAY_LOG_RUNTIME) && CONFIG_AIRPLAY_LOG_RUNTIME
#define AP_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define AP_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define AP_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#else
#define AP_LOG_DISCARD_(tag, fmt, ...)                                         \
  do {                                                                         \
    if (0) {                                                                   \
      (void)(tag);                                                             \
      printf(fmt, ##__VA_ARGS__);                                              \
    }                                                                          \
  } while (0)
#define AP_LOGI(tag, fmt, ...) AP_LOG_DISCARD_(tag, fmt, ##__VA_ARGS__)
#define AP_LOGW(tag, fmt, ...) AP_LOG_DISCARD_(tag, fmt, ##__VA_ARGS__)
#define AP_LOGD(tag, fmt, ...) AP_LOG_DISCARD_(tag, fmt, ##__VA_ARGS__)
#endif
