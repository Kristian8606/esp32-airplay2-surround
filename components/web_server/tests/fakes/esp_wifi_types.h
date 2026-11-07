/* SPDX-License-Identifier: Apache-2.0 */
/* Host fake of ESP-IDF esp_wifi_types.h: wifi_ap_record_t. */
#pragma once
#include <stdint.h>

typedef struct {
  uint8_t bssid[6];
  uint8_t ssid[33];
  uint8_t primary;
  int second;
  int8_t rssi;
  int authmode;
  int pairwise_cipher;
  int group_cipher;
  int ant;
  uint32_t phy_11b : 1;
  uint32_t phy_11g : 1;
  uint32_t phy_11n : 1;
  uint32_t phy_lr : 1;
  uint32_t phy_11a : 1;
  uint32_t phy_11ac : 1;
  uint32_t phy_11ax : 1;
  uint32_t wps : 1;
  uint32_t ftm_responder : 1;
  uint32_t ftm_initiator : 1;
  uint32_t reserved : 22;
} wifi_ap_record_t;
