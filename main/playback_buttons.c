#include "playback_buttons.h"
#include "rtsp_remote.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "airplay_log.h"

#define BUTTON_STACK_BYTES 3072
#define BUTTON_POLL_MS 10
static const char *TAG = "playback_buttons";
static TaskHandle_t button_task_handle;

typedef struct {
  const char *name;
  int gpio;
  uint8_t command;
  bool raw_pressed;
  bool stable_pressed;
  bool armed;
  int64_t changed_at_us;
} playback_button_t;

static playback_button_t buttons[] = {
  {.name = "Play/Pause", .gpio = CONFIG_AIRPLAY_BUTTON_PLAY_PAUSE_GPIO, .command = 2},
  {.name = "Next", .gpio = CONFIG_AIRPLAY_BUTTON_NEXT_GPIO, .command = 4},
  {.name = "Previous", .gpio = CONFIG_AIRPLAY_BUTTON_PREVIOUS_GPIO, .command = 5},
  {.name = "Volume -", .gpio = CONFIG_AIRPLAY_BUTTON_VOLUME_DOWN_GPIO,
   .command = RTSP_REMOTE_VOLUME_DOWN},
  {.name = "Volume +", .gpio = CONFIG_AIRPLAY_BUTTON_VOLUME_UP_GPIO,
   .command = RTSP_REMOTE_VOLUME_UP},
};

static const char *gpio_conflict(int pin) {
  if (pin == CONFIG_I2S_SCK_IO || pin == CONFIG_I2S_BCK_IO ||
      pin == CONFIG_I2S_WS_IO || pin == CONFIG_I2S_DO_IO) return "I2S";
#ifdef CONFIG_AIRPLAY_AMP_CONTROL
  if (pin == CONFIG_AIRPLAY_AMP_GPIO) return "amplifier";
#endif
#ifdef CONFIG_ENABLE_RGB_AUDIO_LED
  if (pin == CONFIG_RGB_AUDIO_LED_GPIO) return "RGB LED";
#endif
#ifdef CONFIG_AIRPLAY_LATENCY_CAL
  if (pin == CONFIG_AIRPLAY_LATENCY_CAL_GPIO) return "latency ADC";
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (pin >= 26 && pin <= 32) return "flash/PSRAM";
#if defined(CONFIG_SPIRAM_MODE_OCT) || defined(CONFIG_ESPTOOLPY_FLASHMODE_OPI)
  if (pin >= 33 && pin <= 37) return "octal flash/PSRAM";
#endif
#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
  if (pin == 43 || pin == 44) return "UART console";
#endif
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED) || defined(CONFIG_ESP_CONSOLE_USB_CDC)
  if (pin == 19 || pin == 20) return "USB console";
#endif
#endif
#ifdef CONFIG_ESP_CONSOLE_UART_CUSTOM
  if (pin == CONFIG_ESP_CONSOLE_UART_TX_GPIO || pin == CONFIG_ESP_CONSOLE_UART_RX_GPIO)
    return "UART console";
#endif
  return NULL;
}

static void button_worker(void *arg) {
  (void)arg;
  TickType_t poll_ticks = pdMS_TO_TICKS(BUTTON_POLL_MS);
  const int64_t debounce_us = (int64_t)CONFIG_AIRPLAY_BUTTON_DEBOUNCE_MS * 1000;
  if (!poll_ticks) poll_ticks = 1;
  int64_t now = esp_timer_get_time();
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
    playback_button_t *b = &buttons[i];
    if (b->gpio < 0) continue;
    b->raw_pressed = gpio_get_level((gpio_num_t)b->gpio) == 0;
    b->stable_pressed = b->raw_pressed;
    b->changed_at_us = now;
    b->armed = false; /* A button held at boot cannot issue a command. */
  }
  for (;;) {
    vTaskDelay(poll_ticks);
    now = esp_timer_get_time();
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
      playback_button_t *b = &buttons[i];
      if (b->gpio < 0) continue;
      bool pressed = gpio_get_level((gpio_num_t)b->gpio) == 0;
      if (pressed != b->raw_pressed) {
        b->raw_pressed = pressed;
        b->changed_at_us = now;
      }
      if (now - b->changed_at_us < debounce_us) continue;
      if (!pressed) {
        b->stable_pressed = false;
        b->armed = true; /* Release must also remain stable for debounce time. */
        continue;
      }
      if (b->stable_pressed) continue;
      b->stable_pressed = true;
      if (!b->armed) continue;
      b->armed = false;
      uint32_t id = 0;
      esp_err_t err = rtsp_remote_enqueue(b->command, &id);
      if (err == ESP_OK)
        AP_LOGI(TAG, "%s GPIO=%d queued id=%" PRIu32, b->name, b->gpio, id);
      else
        AP_LOGW(TAG, "%s GPIO=%d not queued: %s", b->name, b->gpio,
                 err == ESP_ERR_NOT_FOUND ? "no AirPlay event connection" :
                 err == ESP_ERR_INVALID_STATE ? "too many commands waiting" : esp_err_to_name(err));
    }
  }
}

esp_err_t playback_buttons_init(void) {
  if (button_task_handle) return ESP_OK;
  uint64_t mask = 0;
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
    playback_button_t *b = &buttons[i];
    if (b->gpio == -1) continue;
    if (!GPIO_IS_VALID_GPIO(b->gpio)) {
      ESP_LOGE(TAG, "%s has invalid GPIO=%d; buttons disabled", b->name, b->gpio);
      return ESP_ERR_INVALID_ARG;
    }
    const char *conflict = gpio_conflict(b->gpio);
    if (conflict || (mask & (UINT64_C(1) << b->gpio))) {
      ESP_LOGE(TAG, "%s GPIO=%d conflicts with %s; buttons disabled",
               b->name, b->gpio, conflict ? conflict : "another button");
      return ESP_ERR_INVALID_ARG;
    }
    mask |= UINT64_C(1) << b->gpio;
  }
  if (!mask) {
    ESP_LOGI(TAG, "No button GPIO configured; no task created");
    return ESP_OK;
  }
  gpio_config_t config = {
    .pin_bit_mask = mask,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Button GPIO setup failed: %s", esp_err_to_name(err));
    return err;
  }
  if (xTaskCreatePinnedToCoreWithCaps(button_worker, "playback_keys", BUTTON_STACK_BYTES,
        NULL, 2, &button_task_handle, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Task start failed: playback_keys stack=%u PSRAM bytes internalFree=%u "
                 "internalLargest=%u psramFree=%u psramLargest=%u",
             (unsigned)BUTTON_STACK_BYTES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
      if (buttons[i].gpio >= 0) gpio_reset_pin((gpio_num_t)buttons[i].gpio);
    button_task_handle = NULL;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "Ready: active-low buttons, debounce=%d ms, stack=%u bytes PSRAM",
           CONFIG_AIRPLAY_BUTTON_DEBOUNCE_MS, (unsigned)BUTTON_STACK_BYTES);
  return ESP_OK;
}
