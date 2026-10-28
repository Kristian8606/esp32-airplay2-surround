#include "sync_pulse.h"

#include "sdkconfig.h"

#if defined(CONFIG_AIRPLAY_SURROUND_SYNC_PULSE) && CONFIG_AIRPLAY_SURROUND_SYNC_PULSE

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "ptp_clock.h"
#include "ptp_clock_engine.h"

static const char *TAG = "sync_pulse";

#define PULSE_GPIO        CONFIG_AIRPLAY_SURROUND_SYNC_PULSE_GPIO
#define PULSE_WIDTH_MS    10
/* Re-read the clock this long before the edge and arm the timer then, so
 * the edge uses the estimate audio is using at that moment. */
#define PULSE_ARM_LEAD_US 20000LL
#define PULSE_LOG_EVERY   10U
#define NS_PER_S          1000000000ULL

static esp_timer_handle_t s_timer;
static TaskHandle_t s_task;
static volatile int64_t s_target_us;
static volatile int64_t s_fired_us;

/* Runs in the esp_timer interrupt: no flash code (the GPIO write is an
 * inline register access, esp_timer_get_time is in IRAM). */
static void IRAM_ATTR pulse_isr(void *arg) {
  (void)arg;
  gpio_ll_set_level(&GPIO, PULSE_GPIO, 1);
  s_fired_us = esp_timer_get_time();
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(s_task, &woken);
  if (woken) esp_timer_isr_dispatch_need_yield();
}

/* Local time (us) of the next whole sender second at least min_lead_us
 * away, or 0 when there is no usable sender time. */
static int64_t next_edge_us(int64_t min_lead_us, uint64_t *second,
                            ptp_clock_snapshot_t *ps) {
  ptp_clock_get_snapshot(ps);
  if (!ps->valid || ps->realtime_mode) return 0;
  const int64_t now_us = esp_timer_get_time();
  uint64_t remote_now = 0;
  if (!ptp_clock_engine_local_to_remote((uint64_t)now_us * 1000ULL,
                                        ps->filtered_offset_ns, &remote_now))
    return 0;
  uint64_t sec = remote_now / NS_PER_S + 1U;
  for (;;) {
    uint64_t local_ns = 0;
    if (!ptp_clock_engine_remote_to_local(sec * NS_PER_S, ps->filtered_offset_ns,
                                          &local_ns))
      return 0;
    const int64_t at_us = (int64_t)((local_ns + 500U) / 1000U);
    if (at_us - now_us >= min_lead_us) {
      *second = sec;
      return at_us;
    }
    sec++;
  }
}

static void pulse_task(void *arg) {
  (void)arg;
  uint32_t pulses = 0;
  int64_t late_max_us = 0, late_sum_us = 0;
  bool waiting_logged = false;
  for (;;) {
    ptp_clock_snapshot_t ps;
    uint64_t second = 0;
    /* Sleep until shortly before the next edge... */
    int64_t at_us = next_edge_us(PULSE_ARM_LEAD_US + 5000, &second, &ps);
    if (at_us == 0) {
      if (!waiting_logged) {
        ESP_LOGI(TAG, "waiting for sender time");
        waiting_logged = true;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    waiting_logged = false;
    const int64_t sleep_us = at_us - PULSE_ARM_LEAD_US - esp_timer_get_time();
    if (sleep_us > 0) vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000) + 1);

    /* ...then take the estimate of that moment for this same second. */
    uint64_t again = 0;
    at_us = next_edge_us(1000, &again, &ps);
    if (at_us == 0 || again != second) continue;
    const int64_t wait_us = at_us - esp_timer_get_time();
    if (wait_us < 100) continue;
    s_target_us = at_us;
    (void)ulTaskNotifyTake(pdTRUE, 0);
    if (esp_timer_start_once(s_timer, (uint64_t)wait_us) != ESP_OK) continue;
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
      (void)esp_timer_stop(s_timer);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(PULSE_WIDTH_MS));
    gpio_set_level(PULSE_GPIO, 0);

    const int64_t late_us = s_fired_us - s_target_us;
    if (late_us > late_max_us) late_max_us = late_us;
    late_sum_us += late_us;
    if (++pulses % PULSE_LOG_EVERY == 0U) {
      ESP_LOGI(TAG,
               "PULSE second %" PRIu64 " on GPIO %d | interrupt late avg %"
               PRId64 " us max %" PRId64 " us over %u | offset=%" PRId64
               " ns gm=%016" PRIx64 " locked=%d",
               second, PULSE_GPIO, late_sum_us / (int64_t)PULSE_LOG_EVERY,
               late_max_us, PULSE_LOG_EVERY, ps.filtered_offset_ns,
               ps.grandmaster_clock_id, ps.locked);
      late_max_us = 0;
      late_sum_us = 0;
    }
  }
}

esp_err_t sync_pulse_start(void) {
  if (s_task) return ESP_OK;
  const gpio_config_t io = {
      .pin_bit_mask = 1ULL << PULSE_GPIO,
      .mode = GPIO_MODE_OUTPUT,
  };
  esp_err_t err = gpio_config(&io);
  if (err != ESP_OK) return err;
  gpio_set_level(PULSE_GPIO, 0);
  const esp_timer_create_args_t args = {
      .callback = pulse_isr,
      .dispatch_method = ESP_TIMER_ISR,
      .name = "sync_pulse",
  };
  err = esp_timer_create(&args, &s_timer);
  if (err != ESP_OK) return err;
  /* High priority so the pulse is lowered on time; it sleeps almost always. */
  if (xTaskCreatePinnedToCore(pulse_task, "sync_pulse", 3072, NULL, 10, &s_task,
                              0) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGW(TAG, "sync test pulse on GPIO %d at every sender second", PULSE_GPIO);
  return ESP_OK;
}

#else

esp_err_t sync_pulse_start(void) { return ESP_OK; }

#endif
