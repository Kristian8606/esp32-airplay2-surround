/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

typedef enum {
  ESP_RST_UNKNOWN,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
  ESP_RST_USB,
  ESP_RST_JTAG,
  ESP_RST_EFUSE,
  ESP_RST_PWR_GLITCH,
  ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;

/* The fake returns (and records the call). */
void esp_restart(void);
uint32_t esp_get_free_heap_size(void);
esp_reset_reason_t esp_reset_reason(void);
