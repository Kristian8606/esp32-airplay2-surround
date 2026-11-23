/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "c6_pulse.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/gpio_etm.h"
#include "driver/gptimer.h"
#include "driver/gptimer_etm.h"
#include "esp_etm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "c6_pulse";

#define PIN CONFIG_GW_C6_WAKEUP_GPIO

static gptimer_handle_t s_timer;
static bool s_on;
static uint64_t s_edge0;  /* C6 clock at edge 0 */
static uint64_t s_period; /* C6 ticks per period, measured */

/* Everything created, so a failure can undo it. */
static struct {
  esp_etm_event_handle_t alarm, edge;
  esp_etm_task_handle_t toggle, rearm, capture;
  esp_etm_channel_handle_t ch[3];
} s;

/* The calibration channel only (edge -> CAPTURE of the clock). */
static void drop_capture(void) {
  if (s.ch[2]) {
    esp_etm_channel_disable(s.ch[2]);
    esp_etm_del_channel(s.ch[2]);
    s.ch[2] = NULL;
  }
  if (s.capture) {
    esp_etm_del_task(s.capture);
    s.capture = NULL;
  }
  if (s.edge) {
    esp_etm_del_event(s.edge);
    s.edge = NULL;
  }
}

/* Waits for the capture register to change from *last (the next edge). */
static bool next_capture(gptimer_handle_t clock, uint64_t *last) {
  for (int ms = 0; ms < 2 * SURROUND_TIME_INTERVAL_MS; ms++) {
    vTaskDelay(1);
    uint64_t v = 0;
    if (gptimer_get_captured_count(clock, &v) == ESP_OK && v != *last) {
      *last = v;
      return true;
    }
  }
  return false;
}

/* Edges 0..CAL_EDGES-1 from the capture. The capture of an edge comes one
 * tick late now and then (the GPIO input is synchronised to another clock
 * than the C6 clock), so one interval reads period - 1 and the next
 * period + 1. The period itself is a whole number of ticks, fixed for this
 * boot. So: period = (last - first) / intervals, rounded; every interval
 * within CAL_SLACK of it; edge 0 = the mean of e[i] - i * period. */
#define CAL_EDGES 9
#define CAL_SLACK 2
#define CAL_TRIES 3

static esp_err_t calibrate(gptimer_handle_t clock, uint64_t *cap) {
  uint64_t e[CAL_EDGES];
  for (int i = 0; i < CAL_EDGES; i++) {
    if (!next_capture(clock, cap)) {
      ESP_LOGE(TAG, "no edge %d seen on GPIO%d", i, PIN);
      return ESP_ERR_TIMEOUT;
    }
    e[i] = *cap;
  }
  const uint64_t n = CAL_EDGES - 1;
  uint64_t period = (e[n] - e[0] + n / 2) / n;
  if (period < C6_PULSE_PERIOD_TICKS - 100 || period > C6_PULSE_PERIOD_TICKS + 100) {
    ESP_LOGE(TAG, "period %llu ticks, expected about %llu", (unsigned long long)period,
             (unsigned long long)C6_PULSE_PERIOD_TICKS);
    return ESP_ERR_INVALID_RESPONSE;
  }
  int64_t off = 0;
  for (int i = 0; i < CAL_EDGES; i++) {
    if (i > 0) {
      int64_t d = (int64_t)(e[i] - e[i - 1]) - (int64_t)period;
      if (d < -CAL_SLACK || d > CAL_SLACK) {
        ESP_LOGW(TAG, "edge %d: interval %llu, period %llu", i,
                 (unsigned long long)(e[i] - e[i - 1]), (unsigned long long)period);
        return ESP_ERR_INVALID_RESPONSE;
      }
    }
    off += (int64_t)(e[i] - e[0]) - (int64_t)(i * period);
  }
  off = off >= 0 ? (off + CAL_EDGES / 2) / CAL_EDGES : -((-off + CAL_EDGES / 2) / CAL_EDGES);
  s_edge0 = (uint64_t)((int64_t)e[0] + off);
  s_period = period;
  return ESP_OK;
}

static void undo(void) {
  drop_capture();
  for (int i = 0; i < 3; i++) {
    if (s.ch[i]) {
      esp_etm_channel_disable(s.ch[i]);
      esp_etm_del_channel(s.ch[i]);
    }
  }
  if (s.toggle) {
    gpio_etm_task_rm_gpio(s.toggle, PIN);
    esp_etm_del_task(s.toggle);
  }
  if (s.rearm) {
    esp_etm_del_task(s.rearm);
  }
  if (s.capture) {
    esp_etm_del_task(s.capture);
  }
  if (s.alarm) {
    esp_etm_del_event(s.alarm);
  }
  if (s.edge) {
    esp_etm_del_event(s.edge);
  }
  if (s_timer) {
    gptimer_disable(s_timer);
    gptimer_del_timer(s_timer);
    s_timer = NULL;
  }
  memset(&s, 0, sizeof(s));
  gpio_set_level(PIN, 0);
}

static esp_err_t connect(int i, esp_etm_event_handle_t ev, esp_etm_task_handle_t task) {
  const esp_etm_channel_config_t cfg = {0};
  esp_err_t err = esp_etm_new_channel(&cfg, &s.ch[i]);
  if (err == ESP_OK) {
    err = esp_etm_channel_connect(s.ch[i], ev, task);
  }
  if (err == ESP_OK) {
    err = esp_etm_channel_enable(s.ch[i]);
  }
  return err;
}

#define TRY(x)                                                    \
  do {                                                            \
    err = (x);                                                    \
    if (err != ESP_OK) {                                          \
      ESP_LOGE(TAG, "%s: %s", #x, esp_err_to_name(err));          \
      undo();                                                     \
      return err;                                                 \
    }                                                             \
  } while (0)

esp_err_t c6_pulse_start(void) {
  if (s_on) {
    return ESP_OK;
  }
  gptimer_handle_t clock = c6_clock_timer();
  if (!clock) {
    ESP_LOGE(TAG, "the C6 clock is off");
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t err;

  /* Output, with the input path on so that its own edge is an ETM event. */
  const gpio_config_t io = {.pin_bit_mask = 1ULL << PIN, .mode = GPIO_MODE_INPUT_OUTPUT};
  TRY(gpio_config(&io));
  TRY(gpio_set_level(PIN, 0));

  const gptimer_config_t tc = {
      .clk_src = GPTIMER_CLK_SRC_PLL_F80M,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = C6_CLOCK_HZ,
  };
  TRY(gptimer_new_timer(&tc, &s_timer));
  const gptimer_alarm_config_t ac = {
      .alarm_count = C6_PULSE_PERIOD_TICKS / 2,
      .reload_count = 0,
      .flags.auto_reload_on_alarm = true,
  };
  TRY(gptimer_set_alarm_action(s_timer, &ac));

  const gptimer_etm_event_config_t ec = {.event_type = GPTIMER_ETM_EVENT_ALARM_MATCH};
  TRY(gptimer_new_etm_event(s_timer, &ec, &s.alarm));
  const gpio_etm_task_config_t gt = {.action = GPIO_ETM_TASK_ACTION_TOG};
  TRY(gpio_new_etm_task(&gt, &s.toggle));
  TRY(gpio_etm_task_add_gpio(s.toggle, PIN));
  const gptimer_etm_task_config_t rt = {.task_type = GPTIMER_ETM_TASK_EN_ALARM};
  TRY(gptimer_new_etm_task(s_timer, &rt, &s.rearm));

  const gpio_etm_event_config_t ge = {.edge = GPIO_ETM_EVENT_EDGE_POS};
  TRY(gpio_new_etm_event(&ge, &s.edge));
  TRY(gpio_etm_event_bind_gpio(s.edge, PIN));
  const gptimer_etm_task_config_t ct = {.task_type = GPTIMER_ETM_TASK_CAPTURE};
  TRY(gptimer_new_etm_task(clock, &ct, &s.capture));

  TRY(connect(0, s.alarm, s.toggle));
  TRY(connect(1, s.alarm, s.rearm));
  TRY(connect(2, s.edge, s.capture));

  TRY(gptimer_enable(s_timer));
  uint64_t cap = c6_clock_now(); /* also fills the capture register */
  TRY(gptimer_start(s_timer));

  for (int attempt = 1;; attempt++) {
    err = calibrate(clock, &cap);
    if (err == ESP_OK) {
      break;
    }
    if (attempt == CAL_TRIES) {
      undo();
      return err;
    }
  }
  drop_capture();
  s_on = true;
  ESP_LOGI(TAG, "GPIO%d: rising edge every %u ms (ETM), period %llu ticks", PIN,
           (unsigned)SURROUND_TIME_INTERVAL_MS, (unsigned long long)s_period);
  return ESP_OK;
}

bool c6_pulse_last_edge(uint64_t *edge_c6_ticks, uint32_t *seq) {
  if (!s_on) {
    return false;
  }
  uint64_t now = c6_clock_now();
  uint64_t n = now >= s_edge0 ? (now - s_edge0) / s_period : 0;
  if (edge_c6_ticks) {
    *edge_c6_ticks = s_edge0 + n * s_period;
  }
  if (seq) {
    *seq = (uint32_t)n;
  }
  return true;
}

uint64_t c6_pulse_period_ticks(void) {
  return s_on ? s_period : 0;
}
