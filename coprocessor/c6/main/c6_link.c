/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "c6_link.h"

#include <string.h>

#include "c6_clock.h"
#include "c6_ptp.h"
#include "c6_pulse.h"
#include "c6_tsf.h"
#include "eh_cp_feat_peer_data.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_link";

static_assert(C6_TSF_FLAG_CONNECTED == GW_C6_TICK_CONNECTED &&
                  C6_TSF_FLAG_VALID == GW_C6_TICK_TSF_VALID &&
                  C6_TSF_FLAG_RESTART == GW_C6_TICK_RESTART,
              "TICK flags are the c6_tsf flags");
static_assert(sizeof(((c6_ptp_rec_t *)0)->src_id) == sizeof(((gw_c6_ptp_rec_t *)0)->src_id),
              "sourcePortIdentity size");

#define BIT_PTP 0x01   /* c6_ptp has records */
#define BIT_HELLO 0x02 /* the P4 asked for HELLO */

static TaskHandle_t s_task;
static uint8_t s_features;
static portMUX_TYPE s_st_lock = portMUX_INITIALIZER_UNLOCKED;
static c6_link_stats_t s_st;

#define COUNT(field, n)            \
  do {                             \
    portENTER_CRITICAL(&s_st_lock); \
    s_st.field += (n);             \
    portEXIT_CRITICAL(&s_st_lock);  \
  } while (0)

static bool send(uint32_t msg_id, const void *data, size_t len) {
  esp_err_t err = eh_cp_feat_peer_data_send(msg_id, data, len);
  if (err != ESP_OK) {
    static uint32_t fails;
    COUNT(send_fail, 1);
    if (fails++ % 64 == 0) { /* not every one: the link may be down for a while */
      ESP_LOGW(TAG, "send 0x%08lx: %s (%lu failed)", (unsigned long)msg_id,
               esp_err_to_name(err), (unsigned long)fails);
    }
    return false;
  }
  return true;
}

static bool send_hello(void) {
  const esp_app_desc_t *app = esp_app_get_description();
  gw_c6_hello_t h = {
      .proto = GW_C6_PROTO,
      .features = s_features,
      .clock_hz = C6_CLOCK_HZ,
      .pulse_period = (uint32_t)c6_pulse_period_ticks(),
  };
  strlcpy(h.name, app->project_name, sizeof(h.name));
  strlcpy(h.version, app->version, sizeof(h.version));
  strlcpy(h.idf_ver, app->idf_ver, sizeof(h.idf_ver));
  if (!send(GW_C6_MSG_HELLO, &h, sizeof(h))) {
    return false;
  }
  COUNT(hello, 1);
  return true;
}

/* Up to max waiting records, in the wire format. */
static uint8_t take_ptp(gw_c6_ptp_rec_t *out, uint8_t max) {
  if (!(s_features & GW_C6_FEAT_PTP)) {
    return 0;
  }
  c6_ptp_rec_t r[GW_C6_MAX_PTP];
  size_t n = c6_ptp_take(r, max < GW_C6_MAX_PTP ? max : GW_C6_MAX_PTP);
  for (size_t i = 0; i < n; i++) {
    out[i].dir = GW_C6_DIR_RX;
    out[i].type = r[i].type;
    out[i].seq = r[i].seq;
    memcpy(out[i].src_id, r[i].src_id, sizeof(out[i].src_id));
    out[i].c6_ticks = r[i].c6_ticks;
  }
  return (uint8_t)n;
}

static void send_tick(uint64_t edge, uint32_t seq) {
  struct __attribute__((packed)) {
    gw_c6_tick_t t;
    gw_c6_ptp_rec_t ptp[GW_C6_MAX_PTP];
  } m = {
      .t = {.proto = GW_C6_PROTO, .seq = (uint16_t)seq, .edge_c6_ticks = edge},
  };
  if (s_features & GW_C6_FEAT_TSF) {
    int64_t tsf = 0;
    uint32_t span = 0;
    uint8_t flags = 0;
    if (c6_tsf_at(edge, &tsf, &span, &flags) && tsf > 0) {
      m.t.edge_tsf_ns = (uint64_t)tsf;
      m.t.tsf_span_ns = span;
    } else {
      flags &= (uint8_t)~GW_C6_TICK_TSF_VALID;
    }
    m.t.flags = flags;
  }
  m.t.n_ptp = take_ptp(m.ptp, GW_C6_MAX_PTP);
  if (send(GW_C6_MSG_TICK, &m, sizeof(m.t) + m.t.n_ptp * sizeof(m.ptp[0]))) {
    COUNT(ticks, 1);
    COUNT(ptp_recs, m.t.n_ptp);
  }
}

static void send_ptp(void) {
  for (;;) {
    struct __attribute__((packed)) {
      gw_c6_ptp_t p;
      gw_c6_ptp_rec_t ptp[GW_C6_MAX_PTP];
    } m = {.p = {.proto = GW_C6_PROTO}};
    m.p.n_ptp = take_ptp(m.ptp, GW_C6_MAX_PTP);
    if (m.p.n_ptp == 0) {
      return;
    }
    if (send(GW_C6_MSG_PTP, &m, sizeof(m.p) + m.p.n_ptp * sizeof(m.ptp[0]))) {
      COUNT(ptp_msgs, 1);
      COUNT(ptp_recs, m.p.n_ptp);
    }
  }
}

/* Runs in ESP-Hosted's RPC receive path: only wake the task. Answered for any
 * proto byte, so a P4 of another version still learns ours. */
static void on_hello_req(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx) {
  TaskHandle_t task = s_task;
  if (task) {
    xTaskNotify(task, BIT_HELLO, eSetBits);
  }
}

/* ESP-Hosted's own start (auto init) may still be running: try for 10 s. */
static void register_req(void) {
  esp_err_t err = ESP_FAIL;
  for (int i = 0; i < 100; i++) {
    err = eh_cp_feat_peer_data_register_callback(GW_C6_MSG_HELLO_REQ, on_hello_req, NULL);
    if (err == ESP_OK) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  ESP_LOGE(TAG, "HELLO_REQ callback: %s", esp_err_to_name(err));
}

/* Until about 1 ms after the next edge (one period if there is no pulse). */
static TickType_t until_next_edge(void) {
  uint64_t edge = 0;
  uint64_t period = c6_pulse_period_ticks();
  if (!(s_features & GW_C6_FEAT_PULSE) || !period || !c6_pulse_last_edge(&edge, NULL)) {
    return pdMS_TO_TICKS(SURROUND_TIME_INTERVAL_MS);
  }
  uint64_t now = c6_clock_now();
  uint64_t next = edge + period;
  uint32_t ms = next > now ? (uint32_t)((next - now) / (C6_CLOCK_HZ / 1000U)) : 0;
  TickType_t t = pdMS_TO_TICKS(ms + 1);
  return t ? t : 1;
}

static void link_task(void *arg) {
  register_req();
  bool hello_sent = false;
  uint32_t last_seq = 0;
  bool have_seq = false;
  for (;;) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, until_next_edge());
    if (!hello_sent || (bits & BIT_HELLO)) {
      hello_sent = send_hello() || hello_sent;
    }
    uint64_t edge;
    uint32_t seq;
    if ((s_features & GW_C6_FEAT_PULSE) && c6_pulse_last_edge(&edge, &seq) &&
        (!have_seq || seq != last_seq)) {
      if (have_seq && seq > last_seq + 1) {
        COUNT(ticks_missed, seq - last_seq - 1);
      }
      last_seq = seq;
      have_seq = true;
      send_tick(edge, seq);
    }
    send_ptp();
  }
}

esp_err_t c6_link_start(uint8_t features) {
  if (s_task) {
    return ESP_OK;
  }
  s_features = features;
  /* Below the TSF task (configMAX_PRIORITIES - 3): TICKs carry exact times,
   * so being late only delays them. */
  if (xTaskCreate(link_task, "c6_link", 4096, NULL, configMAX_PRIORITIES - 4, &s_task) != pdPASS) {
    ESP_LOGE(TAG, "task not created");
    return ESP_ERR_NO_MEM;
  }
  if (features & GW_C6_FEAT_PTP) {
    c6_ptp_set_notify(s_task, BIT_PTP);
  }
  ESP_LOGI(TAG, "messages to the P4 on (features 0x%02x)", features);
  return ESP_OK;
}

void c6_link_get_stats(c6_link_stats_t *out) {
  portENTER_CRITICAL(&s_st_lock);
  *out = s_st;
  portEXIT_CRITICAL(&s_st_lock);
}
