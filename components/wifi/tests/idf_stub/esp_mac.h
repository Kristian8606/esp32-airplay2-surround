/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Stand-in for the ESP-IDF header of the same name: see README.md. */
#pragma once
#include <stdint.h>

#include "esp_err.h"

typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
