/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Stand-in for the ESP-IDF header of the same name: see README.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct esp_netif_obj esp_netif_t;
typedef struct {
  uint32_t addr; /* network byte order */
} esp_ip4_addr_t;
typedef struct {
  esp_ip4_addr_t ip;
  esp_ip4_addr_t netmask;
  esp_ip4_addr_t gw;
} esp_netif_ip_info_t;
typedef struct {
  esp_netif_t *esp_netif;
  esp_netif_ip_info_t ip_info;
  bool ip_changed;
} ip_event_got_ip_t;
typedef enum {
  IP_EVENT_STA_GOT_IP,
  IP_EVENT_STA_LOST_IP,
} ip_event_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_err_t esp_netif_set_hostname(esp_netif_t *netif, const char *hostname);
