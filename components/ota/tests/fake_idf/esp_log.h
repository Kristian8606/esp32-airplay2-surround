/* SPDX-License-Identifier: Apache-2.0
 * Host stand-in for ESP-IDF's esp_log.h: lines go to fake_log(). */
#pragma once

void fake_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) fake_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fake_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fake_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fake_log('D', tag, fmt, ##__VA_ARGS__)
