/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Log tag "rtsp" (spec "Log"). On a computer the tests give
 * rtsp_host_log(). Never log keys, shk or pairing secrets. */

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define RTSP_TAG "rtsp"
#define RTSP_LOGI(...) ESP_LOGI(RTSP_TAG, __VA_ARGS__)
#define RTSP_LOGW(...) ESP_LOGW(RTSP_TAG, __VA_ARGS__)
#define RTSP_LOGE(...) ESP_LOGE(RTSP_TAG, __VA_ARGS__)
#else
void rtsp_host_log(char level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define RTSP_LOGI(...) rtsp_host_log('I', __VA_ARGS__)
#define RTSP_LOGW(...) rtsp_host_log('W', __VA_ARGS__)
#define RTSP_LOGE(...) rtsp_host_log('E', __VA_ARGS__)
#endif
