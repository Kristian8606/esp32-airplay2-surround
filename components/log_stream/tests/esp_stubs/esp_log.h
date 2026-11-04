#pragma once
#include <stdarg.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
void esp_log_write(int level, const char *tag, const char *format, ...);
#define ESP_LOGI(tag, fmt, ...) esp_log_write(3, tag, fmt, ##__VA_ARGS__)
