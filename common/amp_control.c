#include "amp_control.h"

#include "sdkconfig.h"

#if CONFIG_AIRPLAY_AMP_CONTROL

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "airplay_log.h"

static const char *TAG = "amp_control";

#define AMP_TASK_STACK_BYTES 3072
#define AMP_TASK_PRIORITY    2
#define AMP_TASK_CORE        0

static TaskHandle_t s_amp_task;
static atomic_uint s_active_sessions;

static inline int amp_active_level(void) {
#if CONFIG_AIRPLAY_AMP_ACTIVE_HIGH
  return 1;
#else
  return 0;
#endif
}

static inline int amp_inactive_level(void) {
  return amp_active_level() ? 0 : 1;
}

static void amp_set_output(bool active) {
  gpio_set_level((gpio_num_t)CONFIG_AIRPLAY_AMP_GPIO,
                 active ? amp_active_level() : amp_inactive_level());
}

static void amp_task(void *arg) {
  (void)arg;
  bool amplifier_on = false;

  for (;;) {
    const unsigned sessions =
        atomic_load_explicit(&s_active_sessions, memory_order_acquire);

    if (sessions > 0U) {
      if (!amplifier_on) {
        amp_set_output(true);
        amplifier_on = true;
        AP_LOGI(TAG, "Amplifier ON GPIO=%d active=%s sessions=%u",
                 CONFIG_AIRPLAY_AMP_GPIO,
                 amp_active_level() ? "HIGH" : "LOW", sessions);
      }
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    if (!amplifier_on) {
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

#if CONFIG_AIRPLAY_AMP_OFF_DELAY_SEC > 0
    AP_LOGI(TAG, "No AirPlay sessions; amplifier OFF in %d s",
             CONFIG_AIRPLAY_AMP_OFF_DELAY_SEC);
    const uint32_t notified = ulTaskNotifyTake(
        pdTRUE, pdMS_TO_TICKS((uint32_t)CONFIG_AIRPLAY_AMP_OFF_DELAY_SEC * 1000U));
    if (notified != 0U) {
      /* Connection state changed while waiting. Re-read the atomic state; a
       * reconnect never performs GPIO work in the RTSP task itself. */
      continue;
    }
#endif

    /* The timeout can race with a connection notification at the exact tick.
     * The atomic count is authoritative: never switch off when a session is
     * already visible. A notification arriving just after this check is
     * processed immediately on the next loop and restores ON. */
    if (atomic_load_explicit(&s_active_sessions, memory_order_acquire) == 0U) {
      amp_set_output(false);
      amplifier_on = false;
      AP_LOGI(TAG, "Amplifier OFF GPIO=%d", CONFIG_AIRPLAY_AMP_GPIO);
    }
  }
}

esp_err_t amp_control_init(void) {
  atomic_init(&s_active_sessions, 0U);

  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_AIRPLAY_AMP_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&io);
  if (err != ESP_OK) return err;

  amp_set_output(false);

  /* Stack in PSRAM: internal RAM is scarce during a session, and this task
   * never exits and never writes flash. */
  if (xTaskCreatePinnedToCoreWithCaps(amp_task, "amp_ctrl", AMP_TASK_STACK_BYTES,
                                      NULL, AMP_TASK_PRIORITY, &s_amp_task,
                                      AMP_TASK_CORE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
      s_amp_task == NULL) {
    s_amp_task = NULL;
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Ready GPIO=%d active=%s offDelay=%ds task=core%d/prio%d",
           CONFIG_AIRPLAY_AMP_GPIO, amp_active_level() ? "HIGH" : "LOW",
           CONFIG_AIRPLAY_AMP_OFF_DELAY_SEC, AMP_TASK_CORE, AMP_TASK_PRIORITY);
  return ESP_OK;
}

void amp_control_session_connected(void) {
  atomic_fetch_add_explicit(&s_active_sessions, 1U, memory_order_release);
  TaskHandle_t task = s_amp_task;
  if (task) xTaskNotifyGive(task);
}

void amp_control_session_disconnected(void) {
  unsigned current =
      atomic_load_explicit(&s_active_sessions, memory_order_acquire);
  while (current > 0U &&
         !atomic_compare_exchange_weak_explicit(
             &s_active_sessions, &current, current - 1U,
             memory_order_acq_rel, memory_order_acquire)) {
  }

  TaskHandle_t task = s_amp_task;
  if (task) xTaskNotifyGive(task);
}

#else

esp_err_t amp_control_init(void) { return ESP_OK; }
void amp_control_session_connected(void) {}
void amp_control_session_disconnected(void) {}

#endif
