/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "c6_ptp.h"

#include <string.h>

#include "c6_clock.h"
#include "eh_cp.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_ptp";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static c6_ptp_rec_t s_ring[C6_PTP_RING];
static uint32_t s_head, s_count;
static c6_ptp_stats_t s_st;

/* The task told about new records (c6_ptp_set_notify). */
static TaskHandle_t volatile s_notify_task;
static uint32_t s_notify_bits;

/* ESP-Hosted's own callback (the one to the host), found at the first install. */
static eh_cp_traffic_cb_t volatile s_next;

/* An Ethernet II frame with IPv4/UDP to 319 or 320 and room for the PTP
 * header fields used (34 bytes). Fills r, except the clock. */
static IRAM_ATTR bool parse(const uint8_t *p, uint16_t len, c6_ptp_rec_t *r) {
  if (len < 14 + 20 + 8 + 34 || p[12] != 0x08 || p[13] != 0x00) {
    return false;
  }
  const uint8_t *ip = p + 14;
  if ((ip[0] >> 4) != 4 || ip[9] != 17) {
    return false;
  }
  if ((ip[6] & 0x3f) != 0 || ip[7] != 0) {
    return false; /* a fragment */
  }
  uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;
  if (ihl < 20 || 14 + ihl + 8 + 34 > len) {
    return false;
  }
  const uint8_t *udp = ip + ihl;
  uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
  if (dport != 319 && dport != 320) {
    return false;
  }
  const uint8_t *ptp = udp + 8;
  if ((ptp[1] & 0x0f) != 2) {
    return false; /* not PTPv2 */
  }
  r->type = ptp[0] & 0x0f;
  memcpy(r->src_id, ptp + 20, 10);
  r->seq = (uint16_t)((ptp[30] << 8) | ptp[31]);
  return true;
}

static IRAM_ATTR esp_err_t air_rx(void *buf, uint16_t len, void *eb) {
  uint64_t t = c6_clock_now(); /* first */
  c6_ptp_rec_t r;
  bool ptp = buf && parse(buf, len, &r);
  portENTER_CRITICAL_SAFE(&s_lock);
  s_st.frames++;
  if (ptp) {
    r.c6_ticks = t;
    if (s_count == C6_PTP_RING) {
      s_st.dropped++;
    } else {
      s_ring[(s_head + s_count) % C6_PTP_RING] = r;
      s_count++;
      s_st.records++;
    }
  }
  portEXIT_CRITICAL_SAFE(&s_lock);
  TaskHandle_t task = s_notify_task;
  if (ptp && task) {
    if (xPortInIsrContext()) {
      BaseType_t woken = pdFALSE;
      xTaskNotifyFromISR(task, s_notify_bits, eSetBits, &woken);
      portYIELD_FROM_ISR(woken);
    } else {
      xTaskNotify(task, s_notify_bits, eSetBits);
    }
  }
  eh_cp_traffic_cb_t next = s_next;
  if (next) {
    return next(buf, len, eb);
  }
  esp_wifi_internal_free_rx_buffer(eb);
  return ESP_OK;
}

/* Keeps air_rx in front of ESP-Hosted's callback. A callback that is neither
 * ours nor the one found first (Network split, for one) is left alone: it may
 * call the previous one, which would loop. */
static void watch_task(void *arg) {
  bool warned = false;
  for (;;) {
    eh_cp_traffic_cb_t cur = eh_cp_get_traffic_tx_cb(ESP_STA_IF);
    if (cur && cur != air_rx) {
      if (!s_next) {
        s_next = cur;
      }
      if (cur == s_next) {
        if (eh_cp_set_traffic_tx_cb(ESP_STA_IF, air_rx) == ESP_OK) {
          ESP_LOGI(TAG, "receive timestamps on (in front of %p)", (void *)cur);
        }
      } else if (!warned) {
        ESP_LOGW(TAG, "another station callback (%p): no PTP timestamps", (void *)cur);
        warned = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

esp_err_t c6_ptp_start(void) {
  static bool started;
  if (started) {
    return ESP_OK;
  }
  if (xTaskCreate(watch_task, "c6_ptp", 2560, NULL, 2, NULL) != pdPASS) {
    ESP_LOGE(TAG, "task not created");
    return ESP_ERR_NO_MEM;
  }
  started = true;
  return ESP_OK;
}

void c6_ptp_set_notify(TaskHandle_t task, uint32_t bits) {
  s_notify_bits = bits;
  s_notify_task = task;
}

size_t c6_ptp_take(c6_ptp_rec_t *out, size_t max) {
  size_t n = 0;
  portENTER_CRITICAL(&s_lock);
  while (n < max && s_count) {
    out[n++] = s_ring[s_head];
    s_head = (s_head + 1) % C6_PTP_RING;
    s_count--;
  }
  portEXIT_CRITICAL(&s_lock);
  return n;
}

void c6_ptp_get_stats(c6_ptp_stats_t *out) {
  portENTER_CRITICAL(&s_lock);
  *out = s_st;
  portEXIT_CRITICAL(&s_lock);
}
