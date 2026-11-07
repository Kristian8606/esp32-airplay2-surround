/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Log tag "rtsp" (spec "Log"). Never log keys, shk or pairing secrets. */

#include "esp_log.h"
#define RTSP_TAG "rtsp"
#define RTSP_LOGI(...) ESP_LOGI(RTSP_TAG, __VA_ARGS__)
#define RTSP_LOGW(...) ESP_LOGW(RTSP_TAG, __VA_ARGS__)
#define RTSP_LOGE(...) ESP_LOGE(RTSP_TAG, __VA_ARGS__)
