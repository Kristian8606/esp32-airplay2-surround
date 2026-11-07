/* SPDX-License-Identifier: Apache-2.0 */
/* Host fake: log lines are recorded for the tests. */
#pragma once

void fake_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, ...) fake_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) fake_log('D', tag, __VA_ARGS__)
