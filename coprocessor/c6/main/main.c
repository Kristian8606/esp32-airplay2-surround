/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* C6 firmware of the P4 Gateway: the standard ESP-Hosted co-processor (1.1),
 * the C6 clock (1.2), the time pulse on the WAKEUP line (1.3), the TSF
 * line (1.4), PTP receive timestamps (1.5) and the messages to the P4 over
 * peer data (1.7). The hosted core starts by itself (auto init); app_main
 * prepares NVS and the default event loop, then starts the rest.
 *
 * Rule for everything added here: never abort. A co-processor that resets in
 * a loop can no longer be updated from the P4 (only with USB-TTL), so errors
 * are logged and the feature stays off. */
#include "c6_clock.h"
#include "c6_link.h"
#include "c6_ptp.h"
#include "c6_pulse.h"
#include "c6_tsf.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "gw_c6";

static void nvs_start(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS: %s", esp_err_to_name(err));
  }
}

void app_main(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  ESP_LOGI(TAG, "%s %s (ESP-IDF %s)", app->project_name, app->version, app->idf_ver);
  nvs_start();
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
  }
  uint8_t features = 0;
  if (c6_clock_start() == ESP_OK) {
    features |= GW_C6_FEAT_CLOCK;
    if (c6_pulse_start() == ESP_OK) {
      features |= GW_C6_FEAT_PULSE;
    }
    if (c6_tsf_start() == ESP_OK) {
      features |= GW_C6_FEAT_TSF;
    }
    if (c6_ptp_start() == ESP_OK) {
      features |= GW_C6_FEAT_PTP;
    }
  }
  c6_link_start(features); /* also without the clock: HELLO says what is off */
}
