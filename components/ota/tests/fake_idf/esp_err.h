/* SPDX-License-Identifier: Apache-2.0
 * Host stand-in for ESP-IDF's esp_err.h: only what src/ota.c uses. */
#pragma once
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                       0
#define ESP_FAIL                     -1
#define ESP_ERR_NO_MEM               0x101
#define ESP_ERR_INVALID_ARG          0x102
#define ESP_ERR_INVALID_STATE        0x103
#define ESP_ERR_INVALID_SIZE         0x104
#define ESP_ERR_NOT_FOUND            0x105
#define ESP_ERR_NOT_SUPPORTED        0x106
#define ESP_ERR_TIMEOUT              0x107
#define ESP_ERR_OTA_BASE             0x1500
#define ESP_ERR_OTA_PARTITION_CONFLICT (ESP_ERR_OTA_BASE + 0x01)
#define ESP_ERR_OTA_VALIDATE_FAILED  (ESP_ERR_OTA_BASE + 0x03)
#define ESP_ERR_FLASH_OP_FAIL        0x6001

const char *esp_err_to_name(esp_err_t code);
