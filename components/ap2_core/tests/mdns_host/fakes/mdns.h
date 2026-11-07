/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Host-test stand-in for the espressif/mdns 1.14.1 functions this part
 * calls; same declarations as its mdns.h. */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
  const char *key;
  const char *value;
} mdns_txt_item_t;

esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_service_add(const char *instance_name, const char *service_type,
                           const char *proto, uint16_t port,
                           mdns_txt_item_t txt[], size_t num_items);
