/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "c6_clock.h"

#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "c6_clock";
static gptimer_handle_t s_timer;

esp_err_t c6_clock_start(void) {
  if (s_timer) {
    return ESP_OK;
  }
  const gptimer_config_t cfg = {
      .clk_src = GPTIMER_CLK_SRC_PLL_F80M,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = C6_CLOCK_HZ,
  };
  gptimer_handle_t t = NULL;
  esp_err_t err = gptimer_new_timer(&cfg, &t);
  if (err == ESP_OK) {
    err = gptimer_enable(t);
  }
  if (err == ESP_OK) {
    err = gptimer_start(t);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "not started: %s", esp_err_to_name(err));
    if (t) {
      gptimer_disable(t);
      gptimer_del_timer(t);
    }
    return err;
  }
  uint32_t hz = 0;
  gptimer_get_resolution(t, &hz);
  s_timer = t;
  ESP_LOGI(TAG, "GPTimer PLL_F80M, %lu Hz", (unsigned long)hz);
  return ESP_OK;
}

IRAM_ATTR uint64_t c6_clock_now(void) {
  uint64_t v = 0;
  if (s_timer) {
    gptimer_get_raw_count(s_timer, &v);
  }
  return v;
}

gptimer_handle_t c6_clock_timer(void) {
  return s_timer;
}
