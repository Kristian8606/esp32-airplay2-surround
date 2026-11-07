/* SPDX-License-Identifier: Apache-2.0 */
/* Host stand-in for ESP-IDF's esp_log.h: lines go to fake_log(). */
#pragma once

void fake_log(char level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, ...) fake_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) fake_log('D', tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) fake_log('V', tag, __VA_ARGS__)
