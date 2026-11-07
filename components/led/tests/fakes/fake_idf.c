/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fake_idf.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "rtsp_events.h"

fake_strip_state_t fake_strip;
fake_task_state_t fake_task;
fake_log_state_t fake_logs;
int fake_rtsp_register_calls;
int fake_rtsp_register_result;

/* ---- clock ---- */

static _Atomic int64_t s_now_us;

int64_t esp_timer_get_time(void) { return atomic_load(&s_now_us); }
void fake_set_now_us(int64_t now) { atomic_store(&s_now_us, now); }
void fake_advance_us(int64_t us) { atomic_fetch_add(&s_now_us, us); }

/* ---- log ---- */

static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *esp_err_to_name(esp_err_t err) {
  return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

void fake_log(char level, const char *tag, const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&s_log_mutex);
  if (strcmp(tag, "led") != 0)
    snprintf(line, sizeof(line), "BAD TAG %s", tag);
  if (fake_logs.lines == 0)
    snprintf(fake_logs.first, sizeof(fake_logs.first), "%s", line);
  snprintf(fake_logs.last, sizeof(fake_logs.last), "%s", line);
  fake_logs.lines++;
  if (level == 'E')
    fake_logs.errors++;
  pthread_mutex_unlock(&s_log_mutex);
}

/* ---- task ---- */

static pthread_mutex_t s_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_task_cond = PTHREAD_COND_INITIALIZER;
static bool s_blocked, s_go;
static TaskFunction_t s_task_fn;
static void *s_task_arg;
static struct fake_task {
  int unused;
} s_task_handle;

static void *task_main(void *arg) {
  (void)arg;
  s_task_fn(s_task_arg);
  return NULL;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t code, const char *name, configSTACK_DEPTH_TYPE stack,
    void *param, UBaseType_t priority, TaskHandle_t *created,
    BaseType_t core, UBaseType_t caps) {
  fake_task.create_calls++;
  snprintf(fake_task.name, sizeof(fake_task.name), "%s", name);
  fake_task.stack = stack;
  fake_task.priority = priority;
  fake_task.core = core;
  fake_task.caps = caps;
  if (fake_task.create_fails)
    return pdFAIL;
  s_task_fn = code;
  s_task_arg = param;
  if (created != NULL)
    *created = &s_task_handle;
  pthread_create(&fake_task.thread, NULL, task_main, NULL);
  return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
  (void)clear;
  pthread_mutex_lock(&s_task_mutex);
  fake_task.last_wait = ticks;
  s_blocked = true;
  pthread_cond_broadcast(&s_task_cond);
  while (!s_go)
    pthread_cond_wait(&s_task_cond, &s_task_mutex);
  s_go = false;
  s_blocked = false;
  pthread_cond_broadcast(&s_task_cond);
  pthread_mutex_unlock(&s_task_mutex);
  return 1;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task) {
  (void)task;
  pthread_mutex_lock(&s_task_mutex);
  fake_task.notify_gives++;
  pthread_mutex_unlock(&s_task_mutex);
  return pdPASS;
}

void fake_task_run_once(void) {
  pthread_mutex_lock(&s_task_mutex);
  while (!s_blocked)
    pthread_cond_wait(&s_task_cond, &s_task_mutex);
  s_go = true;
  pthread_cond_broadcast(&s_task_cond);
  while (s_go)
    pthread_cond_wait(&s_task_cond, &s_task_mutex);
  while (!s_blocked)
    pthread_cond_wait(&s_task_cond, &s_task_mutex);
  pthread_mutex_unlock(&s_task_mutex);
}

void fake_task_wait_blocked(void) {
  pthread_mutex_lock(&s_task_mutex);
  while (!s_blocked)
    pthread_cond_wait(&s_task_cond, &s_task_mutex);
  pthread_mutex_unlock(&s_task_mutex);
}

/* ---- led_strip ---- */

static pthread_mutex_t s_strip_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct fake_strip {
  int unused;
} s_strip_handle;
static int s_strip_ops;

esp_err_t led_strip_new_rmt_device(const led_strip_config_t *led_config,
                                   const led_strip_rmt_config_t *rmt_config,
                                   led_strip_handle_t *ret_strip) {
  fake_strip.new_device_calls++;
  fake_strip.led_config = *led_config;
  fake_strip.rmt_config = *rmt_config;
  if (fake_strip.new_device_result != ESP_OK)
    return fake_strip.new_device_result;
  *ret_strip = &s_strip_handle;
  return ESP_OK;
}

static esp_err_t record(led_strip_handle_t strip, fake_strip_op_t op,
                        uint32_t index, uint32_t a, uint32_t b, uint32_t c) {
  if (strip != &s_strip_handle)
    return ESP_FAIL;
  pthread_mutex_lock(&s_strip_mutex);
  esp_err_t err = ESP_OK;
  if (fake_strip.ncalls < (int)(sizeof(fake_strip.calls) /
                                sizeof(fake_strip.calls[0]))) {
    fake_strip_call_t *call = &fake_strip.calls[fake_strip.ncalls++];
    call->op = op;
    call->index = index;
    call->a = a;
    call->b = b;
    call->c = c;
    call->thread = pthread_self();
  }
  s_strip_ops++;
  if (fake_strip.fail_write_after > 0 &&
      s_strip_ops >= fake_strip.fail_write_after)
    err = ESP_FAIL;
  pthread_mutex_unlock(&s_strip_mutex);
  return err;
}

esp_err_t led_strip_set_pixel(led_strip_handle_t strip, uint32_t index,
                              uint32_t red, uint32_t green, uint32_t blue) {
  return record(strip, FAKE_SET_PIXEL, index, red, green, blue);
}

esp_err_t led_strip_set_pixel_hsv(led_strip_handle_t strip, uint32_t index,
                                  uint16_t hue, uint8_t saturation,
                                  uint8_t value) {
  return record(strip, FAKE_SET_PIXEL_HSV, index, hue, saturation, value);
}

esp_err_t led_strip_refresh(led_strip_handle_t strip) {
  return record(strip, FAKE_REFRESH, 0, 0, 0, 0);
}

esp_err_t led_strip_clear(led_strip_handle_t strip) {
  return record(strip, FAKE_CLEAR, 0, 0, 0, 0);
}

/* ---- rtsp_events (helpers) ---- */

static rtsp_event_callback_t s_callback;
static void *s_user_data;

int rtsp_events_register(rtsp_event_callback_t callback, void *user_data) {
  fake_rtsp_register_calls++;
  if (callback == NULL || fake_rtsp_register_result != 0)
    return -1;
  s_callback = callback;
  s_user_data = user_data;
  return 0;
}

void rtsp_events_emit(rtsp_event_t event) {
  if (s_callback != NULL)
    s_callback(event, s_user_data);
}
