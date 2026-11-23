/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "c6_tsf.h"

#include <math.h>
#include <string.h>

#include "c6_clock.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "surround_proto.h"

static const char *TAG = "c6_tsf";

#define NS_PER_TICK (1e9 / C6_CLOCK_HZ)

typedef struct {
  uint64_t mid;    /* C6 clock in the middle of the pair */
  int64_t tsf_ns;
  uint32_t span;   /* ticks */
} pair_t;

/* All callers are tasks (this module, the event loop, the tick sender):
 * a mutex, not a critical section (no FPU: the fit takes tens of us). */
static SemaphoreHandle_t s_lock;
#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)
static struct {
  pair_t p[C6_TSF_FIT_N];
  uint32_t n, head;
  uint32_t jump_run;
  bool connected, restart;
  uint8_t bssid[6];
  /* the line: tsf_ns = tsf_ref + c + k * (ticks - ref); relative values
   * keep the sums of the fit small (the TSF in ns is about 1e15) */
  bool valid;
  uint64_t ref;
  int64_t tsf_ref;
  double c, k;
  uint32_t best_span;
  /* smallest spans of the last pairs */
  uint32_t spans[C6_TSF_SPAN_WINDOW];
  uint32_t nspans;
  c6_tsf_stats_t st;
} s;

/* Lock held. Fits the line over the kept pairs, without the ones more than
 * C6_TSF_REJECT_NS off a first fit. */
static void refit(void) {
  s.valid = false;
  if (s.n < C6_TSF_MIN_FIT) {
    return;
  }
  const pair_t *newest = &s.p[(s.head + C6_TSF_FIT_N - 1) % C6_TSF_FIT_N];
  uint64_t ref = newest->mid;
  int64_t tsf_ref = newest->tsf_ns;
  bool use[C6_TSF_FIT_N];
  for (uint32_t i = 0; i < s.n; i++) {
    use[i] = true;
  }
  double k = 0, c = 0;
  for (int pass = 0; pass < 2; pass++) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int m = 0;
    for (uint32_t i = 0; i < s.n; i++) {
      if (!use[i]) continue;
      const pair_t *q = &s.p[i];
      double x = (double)(int64_t)(q->mid - ref);
      double y = (double)(q->tsf_ns - tsf_ref) - x * NS_PER_TICK;
      sx += x; sy += y; sxx += x * x; sxy += x * y; m++;
    }
    if (m < (int)C6_TSF_MIN_FIT) {
      return;
    }
    double den = m * sxx - sx * sx;
    double kk = den != 0 ? (m * sxy - sx * sy) / den : 0;
    c = (sy - kk * sx) / m;
    k = NS_PER_TICK + kk;
    if (pass == 0) {
      for (uint32_t i = 0; i < s.n; i++) {
        const pair_t *q = &s.p[i];
        double x = (double)(int64_t)(q->mid - ref);
        double r = (double)(q->tsf_ns - tsf_ref) - (c + k * x);
        if (fabs(r) > C6_TSF_REJECT_NS) {
          use[i] = false;
        }
      }
    }
  }
  s.ref = ref;
  s.tsf_ref = tsf_ref;
  s.c = c;
  s.k = k;
  s.best_span = UINT32_MAX;
  for (uint32_t i = 0; i < s.n; i++) {
    if (use[i] && s.p[i].span < s.best_span) s.best_span = s.p[i].span;
  }
  s.valid = true;
}

/* Lock held. */
static void restart_line(void) {
  s.n = 0;
  s.head = 0;
  s.jump_run = 0;
  s.valid = false;
  s.restart = true;
  s.st.restarts++;
}

/* Lock held. A kept pair. */
static void add(const pair_t *q) {
  if (s.valid) {
    double r = (double)(q->tsf_ns - s.tsf_ref) - (s.c + s.k * (double)(int64_t)(q->mid - s.ref));
    if (fabs(r) > C6_TSF_JUMP_NS) {
      if (++s.jump_run >= 3) {
        restart_line(); /* the TSF stepped: start again from this pair */
      } else {
        s.st.outliers++;
        return;
      }
    } else {
      s.jump_run = 0;
    }
  }
  s.p[s.head] = *q;
  s.head = (s.head + 1) % C6_TSF_FIT_N;
  if (s.n < C6_TSF_FIT_N) s.n++;
  s.st.pairs++;
  refit();
}

static uint32_t span_limit(uint32_t span) {
  /* Lock held. Window of the smallest spans of the last pairs. */
  s.spans[s.nspans % C6_TSF_SPAN_WINDOW] = span;
  s.nspans++;
  uint32_t m = s.nspans < C6_TSF_SPAN_WINDOW ? s.nspans : C6_TSF_SPAN_WINDOW;
  uint32_t best = UINT32_MAX;
  for (uint32_t i = 0; i < m; i++) {
    if (s.spans[i] < best) best = s.spans[i];
  }
  return best + (uint32_t)((uint64_t)C6_TSF_SPAN_MARGIN_US * C6_CLOCK_HZ / 1000000U);
}

static void pair_once(void) {
  pair_t best = {.span = UINT32_MAX};
  for (int i = 0; i < C6_TSF_TRIES; i++) {
    uint64_t a = c6_clock_now();
    int64_t tsf_us = esp_wifi_get_tsf_time(WIFI_IF_STA);
    uint64_t b = c6_clock_now();
    if (tsf_us == 0) {
      LOCK();
      s.st.dropped_zero++;
      UNLOCK();
      return;
    }
    uint32_t span = (uint32_t)(b - a);
    if (span < best.span) {
      best.span = span;
      best.mid = a + span / 2;
      best.tsf_ns = tsf_us * 1000;
    }
  }
  LOCK();
  uint32_t limit = span_limit(best.span);
  if (best.span > limit) {
    s.st.dropped_span++;
  } else {
    add(&best);
  }
  UNLOCK();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (id == WIFI_EVENT_STA_CONNECTED) {
    const wifi_event_sta_connected_t *e = data;
    LOCK();
    /* Every new association: the TSF is the new AP's (or restarted). */
    memcpy(s.bssid, e->bssid, 6);
    s.connected = true;
    restart_line();
    UNLOCK();
  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    LOCK();
    s.connected = false;
    restart_line();
    UNLOCK();
  }
}

static void tsf_task(void *arg) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SURROUND_TIME_INTERVAL_MS));
    if (c6_clock_timer()) {
      pair_once();
    }
  }
}

esp_err_t c6_tsf_start(void) {
  static bool started;
  if (started) {
    return ESP_OK;
  }
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
      return ESP_ERR_NO_MEM;
    }
  }
  esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_wifi, NULL);
  if (err == ESP_OK) {
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, NULL);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Wi-Fi events: %s", esp_err_to_name(err));
    return err;
  }
  if (xTaskCreate(tsf_task, "c6_tsf", 3072, NULL, configMAX_PRIORITIES - 3, NULL) != pdPASS) {
    ESP_LOGE(TAG, "task not created");
    return ESP_ERR_NO_MEM;
  }
  started = true;
  ESP_LOGI(TAG, "pairing every %u ms, %d tries, line over %d", (unsigned)SURROUND_TIME_INTERVAL_MS,
           C6_TSF_TRIES, C6_TSF_FIT_N);
  return ESP_OK;
}

bool c6_tsf_at(uint64_t c6_ticks, int64_t *tsf_ns, uint32_t *span_ns, uint8_t *flags) {
  if (!s_lock) {
    if (tsf_ns) *tsf_ns = 0;
    if (span_ns) *span_ns = 0;
    if (flags) *flags = 0;
    return false;
  }
  LOCK();
  bool ok = s.valid && s.connected;
  int64_t t = ok ? s.tsf_ref + (int64_t)llround(s.c + s.k * (double)(int64_t)(c6_ticks - s.ref)) : 0;
  uint32_t sp = ok ? (uint32_t)(s.best_span * NS_PER_TICK) : 0;
  uint8_t f = (s.connected ? C6_TSF_FLAG_CONNECTED : 0) | (ok ? C6_TSF_FLAG_VALID : 0) |
              (s.restart ? C6_TSF_FLAG_RESTART : 0);
  s.restart = false;
  UNLOCK();
  if (tsf_ns) *tsf_ns = t;
  if (span_ns) *span_ns = sp;
  if (flags) *flags = f;
  return ok;
}

void c6_tsf_get_stats(c6_tsf_stats_t *out) {
  if (!s_lock) {
    memset(out, 0, sizeof(*out));
    return;
  }
  LOCK();
  *out = s.st;
  UNLOCK();
}
