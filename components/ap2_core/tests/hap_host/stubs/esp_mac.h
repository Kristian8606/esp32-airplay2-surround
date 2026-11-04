#pragma once
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_BASE } esp_mac_type_t;
static inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t t) {
  static const uint8_t fixed[6] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
  (void)t; memcpy(mac, fixed, 6); return ESP_OK;
}
