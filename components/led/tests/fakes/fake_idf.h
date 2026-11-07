/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Host fakes of the parts of ESP-IDF, FreeRTOS and led_strip that led.c
 * uses, with controls for the tests. One header, so that every fake ESP-IDF
 * header just includes it. */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- esp_err.h ---- */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
const char *esp_err_to_name(esp_err_t err);

/* ---- esp_log.h ---- */
void fake_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, fmt, ...) fake_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fake_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fake_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fake_log('D', tag, fmt, ##__VA_ARGS__)

/* ---- esp_timer.h ---- */
int64_t esp_timer_get_time(void);

/* ---- esp_heap_caps.h ---- */
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)

/* ---- freertos ---- */
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;
typedef void (*TaskFunction_t)(void *);
typedef struct fake_task *TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define configTICK_RATE_HZ 100

typedef struct {
  pthread_mutex_t m;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {PTHREAD_MUTEX_INITIALIZER}
#define portENTER_CRITICAL(mux) pthread_mutex_lock(&(mux)->m)
#define portEXIT_CRITICAL(mux) pthread_mutex_unlock(&(mux)->m)

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t code, const char *name, configSTACK_DEPTH_TYPE stack,
    void *param, UBaseType_t priority, TaskHandle_t *created,
    BaseType_t core, UBaseType_t caps);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t task);

/* ---- led_strip.h (the public API of espressif/led_strip 3.0.x) ---- */
typedef struct fake_strip *led_strip_handle_t;
typedef enum {
  LED_MODEL_WS2812,
  LED_MODEL_SK6812,
  LED_MODEL_WS2811,
  LED_MODEL_INVALID,
} led_model_t;
typedef union {
  struct {
    uint32_t r_pos : 2;
    uint32_t g_pos : 2;
    uint32_t b_pos : 2;
    uint32_t w_pos : 2;
    uint32_t reserved : 19;
    uint32_t bytes_per_color : 2;
    uint32_t num_components : 3;
  } format;
  uint32_t format_id;
} led_color_component_format_t;
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB                                      \
  ((led_color_component_format_t){.format = {.r_pos = 1,                       \
                                             .g_pos = 0,                       \
                                             .b_pos = 2,                       \
                                             .w_pos = 3,                       \
                                             .reserved = 0,                    \
                                             .bytes_per_color = 1,             \
                                             .num_components = 3}})
typedef struct {
  int strip_gpio_num;
  uint32_t max_leds;
  led_model_t led_model;
  led_color_component_format_t color_component_format;
  struct {
    uint32_t invert_out : 1;
  } flags;
} led_strip_config_t;
typedef enum { RMT_CLK_SRC_APB = 4, RMT_CLK_SRC_DEFAULT = 4 } rmt_clock_source_t;
typedef struct {
  rmt_clock_source_t clk_src;
  uint32_t resolution_hz;
  size_t mem_block_symbols;
  struct {
    uint32_t with_dma : 1;
  } flags;
} led_strip_rmt_config_t;

esp_err_t led_strip_new_rmt_device(const led_strip_config_t *led_config,
                                   const led_strip_rmt_config_t *rmt_config,
                                   led_strip_handle_t *ret_strip);
esp_err_t led_strip_set_pixel(led_strip_handle_t strip, uint32_t index,
                              uint32_t red, uint32_t green, uint32_t blue);
esp_err_t led_strip_set_pixel_hsv(led_strip_handle_t strip, uint32_t index,
                                  uint16_t hue, uint8_t saturation,
                                  uint8_t value);
esp_err_t led_strip_refresh(led_strip_handle_t strip);
esp_err_t led_strip_clear(led_strip_handle_t strip);

/* ---- controls and records for the tests ---- */

void fake_set_now_us(int64_t now);
void fake_advance_us(int64_t us);

typedef enum {
  FAKE_SET_PIXEL,
  FAKE_SET_PIXEL_HSV,
  FAKE_REFRESH,
  FAKE_CLEAR,
} fake_strip_op_t;

typedef struct {
  fake_strip_op_t op;
  uint32_t index, a, b, c; /* r, g, b or h, s, v */
  pthread_t thread;
} fake_strip_call_t;

typedef struct {
  /* Set by the test. */
  esp_err_t new_device_result;
  int fail_write_after; /* >0: the n-th later write fails; 0: never */
  /* Recorded. */
  int new_device_calls;
  led_strip_config_t led_config;
  led_strip_rmt_config_t rmt_config;
  int ncalls;
  fake_strip_call_t calls[4096];
} fake_strip_state_t;
extern fake_strip_state_t fake_strip;

typedef struct {
  /* Set by the test. */
  bool create_fails;
  /* Recorded. */
  int create_calls;
  char name[16];
  uint32_t stack;
  unsigned priority;
  int core;
  unsigned caps;
  int notify_gives;
  TickType_t last_wait; /* the timeout of the last ulTaskNotifyTake() */
  pthread_t thread;
} fake_task_state_t;
extern fake_task_state_t fake_task;

/* Lets the task run one round, from one ulTaskNotifyTake() to the next,
 * and waits until it blocks again. */
void fake_task_run_once(void);
/* Waits until the task blocks in ulTaskNotifyTake(). */
void fake_task_wait_blocked(void);

typedef struct {
  int lines;
  int errors;
  char last[256];
  char first[256];
} fake_log_state_t;
extern fake_log_state_t fake_logs;

/* rtsp_events, the side of the helpers component. */
extern int fake_rtsp_register_calls;
extern int fake_rtsp_register_result;
