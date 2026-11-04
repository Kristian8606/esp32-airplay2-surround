#pragma once
#include <stdio.h>
#define HOST_LOG_(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE HOST_LOG_
#define ESP_LOGW HOST_LOG_
#define ESP_LOGI HOST_LOG_
#define ESP_LOGD HOST_LOG_
