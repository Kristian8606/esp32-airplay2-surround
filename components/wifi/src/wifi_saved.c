/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_saved.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "wifi";

size_t wifi_saved_load(settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS],
                       wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS]) {
  size_t total = settings_get_wifi_network_count();
  if (total > SETTINGS_MAX_WIFI_NETWORKS) {
    ESP_LOGW(TAG, "%u saved networks: only the first %u are used",
             (unsigned)total, (unsigned)SETTINGS_MAX_WIFI_NETWORKS);
    total = SETTINGS_MAX_WIFI_NETWORKS;
  }
  size_t count = 0;
  for (size_t i = 0; i < total; i++) {
    settings_wifi_network_t *n = &net[count];
    memset(n, 0, sizeof(*n));
    esp_err_t err = settings_get_wifi_network(i, n);
    if (err != ESP_OK || wl_ssid_len(n->ssid) == 0) {
      ESP_LOGW(TAG, "saved network %u: %s: left out", (unsigned)i,
               err != ESP_OK ? esp_err_to_name(err) : "empty SSID");
      continue; /* the slot is cleared again before the next read */
    }
    n->ssid[SETTINGS_WIFI_SSID_LEN - 1] = '\0';
    n->password[SETTINGS_WIFI_PASSWORD_LEN - 1] = '\0';
    saved[count].ssid = n->ssid;
    saved[count].has_password = n->password[0] != '\0';
    count++;
  }
  /* The rest cleared: nothing of a network that failed stays behind. */
  for (size_t i = count; i < SETTINGS_MAX_WIFI_NETWORKS; i++) {
    memset(&net[i], 0, sizeof(net[i]));
    saved[i] = (wl_saved_t){NULL, false};
  }
  if (total > 0 && count == 0) {
    ESP_LOGW(TAG, "none of the %u saved networks can be read: working as "
             "with no saved network", (unsigned)total);
  }
  return count;
}
