/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led.h"

#include "sdkconfig.h"

/* The options come from the firmware's Kconfig (airplay_options); without
 * the LED the others do not exist, so they are checked only with it. */
#if defined(CONFIG_ENABLE_RGB_AUDIO_LED)

#include <stdatomic.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "led_core.h"
#include "led_strip.h"
#include "rtsp_events.h"

#if (defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU) +                                \
     defined(CONFIG_RGB_AUDIO_LED_PLAYING_STEADY) +                            \
     defined(CONFIG_RGB_AUDIO_LED_PLAYING_OFF)) != 1
#error "Set exactly one CONFIG_RGB_AUDIO_LED_PLAYING_* option"
#endif
#if (defined(CONFIG_RGB_AUDIO_LED_PAUSED_STEADY) +                             \
     defined(CONFIG_RGB_AUDIO_LED_PAUSED_OFF)) != 1
#error "Set exactly one CONFIG_RGB_AUDIO_LED_PAUSED_* option"
#endif
#if (defined(CONFIG_RGB_AUDIO_LED_STANDBY_OFF) +                               \
     defined(CONFIG_RGB_AUDIO_LED_STANDBY_STEADY)) != 1
#error "Set exactly one CONFIG_RGB_AUDIO_LED_STANDBY_* option"
#endif

#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU)
#define PLAYING_MODE LED_PLAYING_VU
#elif defined(CONFIG_RGB_AUDIO_LED_PLAYING_STEADY)
#define PLAYING_MODE LED_PLAYING_STEADY
#else
#define PLAYING_MODE LED_PLAYING_OFF
#endif
#if defined(CONFIG_RGB_AUDIO_LED_PAUSED_STEADY)
#define PAUSED_MODE LED_PAUSED_STEADY
#else
#define PAUSED_MODE LED_PAUSED_OFF
#endif
#if defined(CONFIG_RGB_AUDIO_LED_STANDBY_STEADY)
#define STANDBY_MODE LED_STANDBY_STEADY
#else
#define STANDBY_MODE LED_STANDBY_OFF
#endif

#if !defined(CONFIG_RGB_AUDIO_LED_GPIO) || CONFIG_RGB_AUDIO_LED_GPIO < 0 ||     \
    CONFIG_RGB_AUDIO_LED_GPIO > 48
#error "CONFIG_RGB_AUDIO_LED_GPIO must be 0-48"
#endif
#if !defined(CONFIG_RGB_AUDIO_LED_BRIGHTNESS) ||                               \
    CONFIG_RGB_AUDIO_LED_BRIGHTNESS < 0 || CONFIG_RGB_AUDIO_LED_BRIGHTNESS > 255
#error "CONFIG_RGB_AUDIO_LED_BRIGHTNESS must be 0-255"
#endif
#if !defined(CONFIG_RGB_AUDIO_LED_UPDATE_HZ) ||                                \
    CONFIG_RGB_AUDIO_LED_UPDATE_HZ < 5 || CONFIG_RGB_AUDIO_LED_UPDATE_HZ > 60
#error "CONFIG_RGB_AUDIO_LED_UPDATE_HZ must be 5-60"
#endif

#define TASK_STACK 4096
#define TASK_PRIORITY 1
#define TASK_CORE 0
#define RMT_RESOLUTION_HZ (10 * 1000 * 1000)

static const char *TAG = "led";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static led_core_t s_core;
static led_strip_handle_t s_strip;
static TaskHandle_t s_task;
static bool s_init_called;
/* True while both functions work: after a good set-up, until an error. */
static atomic_bool s_running;

static int64_t now_us(void *arg) {
  (void)arg;
  return esp_timer_get_time();
}

static void lock(void *arg) { portENTER_CRITICAL((portMUX_TYPE *)arg); }

static void unlock(void *arg) { portEXIT_CRITICAL((portMUX_TYPE *)arg); }

static esp_err_t show(const led_color_t *color) {
  esp_err_t err;
  switch (color->kind) {
  case LED_COLOR_RGB:
    err = led_strip_set_pixel(s_strip, 0, color->r, color->g, color->b);
    break;
  case LED_COLOR_HSV:
    err = led_strip_set_pixel_hsv(s_strip, 0, color->h, color->s, color->v);
    break;
  case LED_COLOR_OFF:
  default:
    /* Clears the buffer and sends it. */
    return led_strip_clear(s_strip);
  }
  if (err != ESP_OK)
    return err;
  return led_strip_refresh(s_strip);
}

static TickType_t to_ticks(int64_t wait_us) {
  if (wait_us == LED_CORE_WAIT_FOREVER)
    return portMAX_DELAY;
  if (wait_us <= 0)
    return 1;
  /* Rounded up, so that the task never wakes before the update is due. */
  int64_t ticks = (wait_us * configTICK_RATE_HZ + 999999) / 1000000;
  if (ticks < 1)
    ticks = 1;
  if (ticks >= (int64_t)portMAX_DELAY)
    ticks = (int64_t)portMAX_DELAY - 1;
  return (TickType_t)ticks;
}

static void led_task(void *arg) {
  (void)arg;
  /* led_init() has shown the standby colour; wait for the first event. */
  int64_t wait_us = LED_CORE_WAIT_FOREVER;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, to_ticks(wait_us));
    if (!atomic_load(&s_running)) {
      wait_us = LED_CORE_WAIT_FOREVER;
      continue;
    }
    led_color_t color;
    bool changed;
    wait_us = led_core_step(&s_core, &color, &changed);
    if (changed) {
      esp_err_t err = show(&color);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED write failed: %s", esp_err_to_name(err));
        atomic_store(&s_running, false);
        wait_us = LED_CORE_WAIT_FOREVER;
      }
    }
  }
}

static void on_rtsp_event(rtsp_event_t event, void *user_data) {
  (void)user_data;
  if (!atomic_load(&s_running))
    return;
  if (led_core_event(&s_core, event) && s_task != NULL)
    xTaskNotifyGive(s_task);
}

void led_init(void) {
  if (s_init_called)
    return;
  s_init_called = true;

  const led_core_config_t cfg = {
      .brightness = CONFIG_RGB_AUDIO_LED_BRIGHTNESS,
      .update_hz = CONFIG_RGB_AUDIO_LED_UPDATE_HZ,
      .playing = PLAYING_MODE,
      .paused = PAUSED_MODE,
      .standby = STANDBY_MODE,
      .now_us = now_us,
      .lock = lock,
      .unlock = unlock,
      .arg = &s_mux,
  };
  if (!led_core_init(&s_core, &cfg)) {
    ESP_LOGE(TAG, "bad LED options");
    return;
  }

  const led_strip_config_t strip_cfg = {
      .strip_gpio_num = CONFIG_RGB_AUDIO_LED_GPIO,
      .max_leds = 1,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags = {.invert_out = false},
  };
  const led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = RMT_RESOLUTION_HZ,
      .mem_block_symbols = 0,
      .flags = {.with_dma = false},
  };
  esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LED set-up on GPIO %d failed: %s",
             CONFIG_RGB_AUDIO_LED_GPIO, esp_err_to_name(err));
    s_strip = NULL;
    return;
  }

  /* The standby colour, at once. */
  led_color_t color;
  bool changed;
  (void)led_core_step(&s_core, &color, &changed);
  err = show(&color);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "LED write failed: %s", esp_err_to_name(err));
    return;
  }

  atomic_store(&s_running, true);
  if (xTaskCreatePinnedToCoreWithCaps(led_task, "led", TASK_STACK, NULL,
                                      TASK_PRIORITY, &s_task, TASK_CORE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
      pdPASS) {
    atomic_store(&s_running, false);
    s_task = NULL;
    ESP_LOGE(TAG, "cannot create the LED task");
    (void)led_strip_clear(s_strip);
    return;
  }

  /* Without events the LED keeps the standby colour; the rest goes on. */
  if (rtsp_events_register(on_rtsp_event, NULL) != 0)
    ESP_LOGE(TAG, "cannot register for RTSP events");

#if defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU)
  ESP_LOGI(TAG, "WS2812 LED on GPIO %d, brightness %d, level meter %d Hz",
           CONFIG_RGB_AUDIO_LED_GPIO, CONFIG_RGB_AUDIO_LED_BRIGHTNESS,
           CONFIG_RGB_AUDIO_LED_UPDATE_HZ);
#else
  ESP_LOGI(TAG, "WS2812 LED on GPIO %d, brightness %d",
           CONFIG_RGB_AUDIO_LED_GPIO, CONFIG_RGB_AUDIO_LED_BRIGHTNESS);
#endif
}

void led_audio_feed(const int32_t *pcm, size_t stereo_frames) {
  if (!atomic_load_explicit(&s_running, memory_order_relaxed))
    return;
  led_core_feed(&s_core, pcm, stereo_frames);
}

#else /* !CONFIG_ENABLE_RGB_AUDIO_LED */

void led_init(void) {}

void led_audio_feed(const int32_t *pcm, size_t stereo_frames) {
  (void)pcm;
  (void)stereo_frames;
}

#endif
