/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Host-test stand-in for ESP-IDF esp_log.h: every line goes to fake_log(). */

void fake_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) fake_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fake_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fake_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fake_log('D', tag, fmt, ##__VA_ARGS__)
