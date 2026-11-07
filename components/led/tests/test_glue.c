/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* src/led.c itself, with fakes of FreeRTOS, esp_timer, log and led_strip
 * (tests/fakes). Built once per menuconfig set; each run does one scenario
 * (argv[1]), since led_init() works only once per process. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "fake_idf.h"
#include "led.h"
#include "rtsp_events.h"

static int calls_now(void) { return fake_strip.ncalls; }

static void check_call(int i, fake_strip_op_t op, uint32_t a, uint32_t b,
                       uint32_t c) {
  if (i >= fake_strip.ncalls) {
    fprintf(stderr, "  driver call %d missing\n", i);
    g_failures++;
    return;
  }
  const fake_strip_call_t *call = &fake_strip.calls[i];
  CHECK_INT(call->op, op);
  CHECK_INT(call->index, 0);
  if (op == FAKE_SET_PIXEL || op == FAKE_SET_PIXEL_HSV) {
    CHECK_INT(call->a, a);
    CHECK_INT(call->b, b);
    CHECK_INT(call->c, c);
  }
}

/* The colour written since driver call `from`: one set_pixel + refresh, or
 * one clear. */
static void check_rgb_since(int from, uint32_t r, uint32_t g, uint32_t b) {
  CHECK_INT(fake_strip.ncalls - from, 2);
  check_call(from, FAKE_SET_PIXEL, r, g, b);
  check_call(from + 1, FAKE_REFRESH, 0, 0, 0);
}

static void check_hsv_since(int from, uint32_t h, uint32_t v) {
  CHECK_INT(fake_strip.ncalls - from, 2);
  check_call(from, FAKE_SET_PIXEL_HSV, h, 255, v);
  check_call(from + 1, FAKE_REFRESH, 0, 0, 0);
}

static void check_clear_since(int from) {
  CHECK_INT(fake_strip.ncalls - from, 1);
  check_call(from, FAKE_CLEAR, 0, 0, 0);
}

static void feed_const(int16_t x, size_t frames) {
  int32_t *pcm = malloc((frames ? frames : 1) * 2 * sizeof(int32_t));
  for (size_t i = 0; i < frames * 2; i++)
    pcm[i] = (int32_t)((uint32_t)(uint16_t)x << 16);
  led_audio_feed(pcm, frames);
  free(pcm);
}

/* An event as the RTSP task emits it, then the task's wake-up. */
static void event(rtsp_event_t ev) {
  int gives = fake_task.notify_gives;
  rtsp_events_emit(ev);
  CHECK_INT(fake_task.notify_gives, gives + 1);
  fake_task_run_once();
}

static void check_driver_setup(int gpio) {
  CHECK_INT(fake_strip.new_device_calls, 1);
  CHECK_INT(fake_strip.led_config.strip_gpio_num, gpio);
  CHECK_INT(fake_strip.led_config.max_leds, 1);
  CHECK_INT(fake_strip.led_config.led_model, LED_MODEL_WS2812);
  CHECK_INT(fake_strip.led_config.color_component_format.format_id,
            LED_STRIP_COLOR_COMPONENT_FMT_GRB.format_id);
  CHECK_INT(fake_strip.led_config.flags.invert_out, 0);
  CHECK_INT(fake_strip.rmt_config.clk_src, RMT_CLK_SRC_DEFAULT);
  CHECK_INT(fake_strip.rmt_config.resolution_hz, 10000000);
  CHECK_INT(fake_strip.rmt_config.mem_block_symbols, 0);
  CHECK_INT(fake_strip.rmt_config.flags.with_dma, 0);
}

static void check_task_setup(void) {
  CHECK_INT(fake_task.create_calls, 1);
  CHECK(strcmp(fake_task.name, "led") == 0);
  CHECK_INT(fake_task.stack, 4096);
  CHECK_INT(fake_task.priority, 1);
  CHECK_INT(fake_task.core, 0);
  CHECK_INT(fake_task.caps, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* After an error both functions do nothing. */
static void check_inert(void) {
  int calls = calls_now();
  int gives = fake_task.notify_gives;
  int lines = fake_logs.lines;
  rtsp_events_emit(RTSP_EVENT_PLAYING);
  feed_const(32767, 480);
  led_audio_feed(NULL, 480);
  led_init();
  CHECK_INT(calls_now(), calls);
  CHECK_INT(fake_task.notify_gives, gives);
  CHECK_INT(fake_logs.lines, lines);
  CHECK(fake_strip.new_device_calls <= 1);
  CHECK(fake_task.create_calls <= 1);
}

#if defined(CONFIG_ENABLE_RGB_AUDIO_LED) && defined(CONFIG_RGB_AUDIO_LED_PLAYING_VU)

static atomic_bool g_stop;

static void *feeder(void *arg) {
  (void)arg;
  int16_t levels[] = {16384, 3277, 0, 32767, 328};
  unsigned i = 0;
  while (!atomic_load(&g_stop)) {
    feed_const(levels[i++ % 5], 480);
    feed_const(1, 0);
    feed_const(-5, 1);
  }
  return NULL;
}

/* VU, paused steady, standby off, GPIO 38, B 96, 30 Hz. */
static void scenario_ok(void) {
  fake_set_now_us(5000000);
  led_init();
  check_driver_setup(38);
  check_task_setup();
  CHECK_INT(fake_rtsp_register_calls, 1);
  CHECK_INT(fake_logs.lines, 1);
  CHECK_INT(fake_logs.errors, 0);
  CHECK(strcmp(fake_logs.first,
               "WS2812 LED on GPIO 38, brightness 96, level meter 30 Hz") ==
        0);
  /* Standby at once: off. */
  check_clear_since(0);
  fake_task_wait_blocked();
  CHECK(fake_task.last_wait == portMAX_DELAY); /* no polling */

  int n = calls_now();
  event(RTSP_EVENT_CLIENT_CONNECTED);
  check_rgb_since(n, 0, 0, 36);
  CHECK(fake_task.last_wait == portMAX_DELAY);

  /* Fed while paused: not counted, no driver call. */
  n = calls_now();
  feed_const(32767, 480);
  CHECK_INT(calls_now(), n);

  event(RTSP_EVENT_PLAYING); /* no audio yet: still blue, not written */
  CHECK_INT(calls_now(), n);
  CHECK_INT(fake_task.last_wait, 4); /* 33.3 ms at 100 Hz ticks, rounded up */

  for (int i = 0; i < 4; i++)
    feed_const(16384, 480);
  CHECK_INT(calls_now(), n); /* the feed never calls the driver */
  fake_advance_us(33334);
  fake_task_run_once();
  check_hsv_since(n, 29, 74);

  n = calls_now();
  feed_const(16384, 480);
  fake_advance_us(33334);
  fake_task_run_once();
  CHECK_INT(calls_now(), n); /* same colour: not written */

  /* 500 ms without audio: back to blue. */
  for (int i = 0; i < 15; i++) {
    fake_advance_us(33334);
    fake_task_run_once();
  }
  check_rgb_since(n, 0, 0, 36);

  /* Feeds from another thread while the task updates and events come. */
  atomic_store(&g_stop, false);
  pthread_t t;
  pthread_create(&t, NULL, feeder, NULL);
  for (int i = 0; i < 300; i++) {
    if (i % 50 == 25)
      event(RTSP_EVENT_PAUSED);
    else if (i % 50 == 30)
      event(RTSP_EVENT_PLAYING);
    fake_advance_us(33334);
    fake_task_run_once();
    usleep(200);
  }
  atomic_store(&g_stop, true);
  pthread_join(t, NULL);
  int from_task = 0;
  for (int i = 0; i < fake_strip.ncalls; i++)
    if (i > 0) /* the first is led_init()'s, from this thread */
      from_task += pthread_equal(fake_strip.calls[i].thread, fake_task.thread);
  CHECK_INT(from_task, fake_strip.ncalls - 1);

  event(RTSP_EVENT_PAUSED);
  n = calls_now();
  event(RTSP_EVENT_DISCONNECTED);
  check_clear_since(n);
  CHECK(fake_task.last_wait == portMAX_DELAY);

  /* led_init() again does nothing. */
  n = calls_now();
  led_init();
  CHECK_INT(calls_now(), n);
  CHECK_INT(fake_task.create_calls, 1);
  CHECK_INT(fake_strip.new_device_calls, 1);

  CHECK_INT(fake_logs.lines, 1); /* nothing per event or update */
}

/* The first write after set-up fails: one error, then nothing. */
static void scenario_write_fail(void) {
  fake_strip.fail_write_after = 2;
  led_init();
  check_clear_since(0);
  event(RTSP_EVENT_CLIENT_CONNECTED);
  CHECK_INT(fake_logs.errors, 1);
  CHECK_INT(fake_logs.lines, 2);
  check_inert();
}

#endif

#if defined(CONFIG_ENABLE_RGB_AUDIO_LED) && defined(CONFIG_RGB_AUDIO_LED_PLAYING_STEADY)

/* Playing steady, paused off, standby steady, GPIO 5, B 255. */
static void scenario_ok(void) {
  led_init();
  check_driver_setup(5);
  check_task_setup();
  CHECK_INT(fake_logs.lines, 1);
  CHECK(strcmp(fake_logs.first, "WS2812 LED on GPIO 5, brightness 255") == 0);
  check_rgb_since(0, 0, 40, 0);
  fake_task_wait_blocked();

  int n = calls_now();
  event(RTSP_EVENT_CLIENT_CONNECTED);
  check_clear_since(n);
  n = calls_now();
  event(RTSP_EVENT_PLAYING);
  check_rgb_since(n, 0, 96, 16);
  CHECK(fake_task.last_wait == portMAX_DELAY); /* no level meter updates */
  n = calls_now();
  feed_const(32767, 480);
  fake_advance_us(1000000);
  CHECK_INT(calls_now(), n);
  event(RTSP_EVENT_PAUSED);
  check_clear_since(n);
  n = calls_now();
  event(RTSP_EVENT_DISCONNECTED);
  check_rgb_since(n, 0, 40, 0);
  CHECK_INT(fake_logs.lines, 1);
}

static void scenario_write_fail(void) {
  fake_strip.fail_write_after = 1; /* the standby colour at set-up */
  led_init();
  CHECK_INT(fake_logs.errors, 1);
  CHECK_INT(fake_logs.lines, 1);
  CHECK_INT(fake_task.create_calls, 0);
  check_inert();
}

#endif

#ifdef CONFIG_ENABLE_RGB_AUDIO_LED

static void scenario_strip_fail(void) {
  fake_strip.new_device_result = ESP_FAIL;
  led_init();
  CHECK_INT(fake_logs.errors, 1);
  CHECK_INT(fake_logs.lines, 1);
  CHECK_INT(fake_task.create_calls, 0);
  CHECK_INT(fake_rtsp_register_calls, 0);
  CHECK_INT(calls_now(), 0);
  check_inert();
}

static void scenario_task_fail(void) {
  fake_task.create_fails = true;
  led_init();
  CHECK_INT(fake_task.create_calls, 1);
  CHECK_INT(fake_logs.errors, 1);
  CHECK_INT(fake_logs.lines, 1);
  CHECK_INT(fake_rtsp_register_calls, 0);
  /* The LED cleared. */
  CHECK(calls_now() >= 1);
  check_call(calls_now() - 1, FAKE_CLEAR, 0, 0, 0);
  check_inert();
}

/* One error; the LED keeps the standby colour and the rest goes on. */
static void scenario_register_fail(void) {
  fake_rtsp_register_result = -1;
  led_init();
  CHECK_INT(fake_rtsp_register_calls, 1);
  CHECK_INT(fake_logs.errors, 1);
  /* The set-up worked: the start line too. */
  CHECK_INT(fake_logs.lines, 2);
  CHECK(strncmp(fake_logs.last, "WS2812 LED on GPIO ", 19) == 0);
  CHECK_INT(fake_task.create_calls, 1);
  int n = calls_now();
  CHECK(n >= 1); /* the standby colour */
  fake_task_wait_blocked();
  CHECK(fake_task.last_wait == portMAX_DELAY);
  rtsp_events_emit(RTSP_EVENT_PLAYING); /* never reaches the LED */
  feed_const(32767, 480);
  led_audio_feed(NULL, 0);
  CHECK_INT(fake_task.notify_gives, 0);
  CHECK_INT(calls_now(), n);
  CHECK_INT(fake_logs.lines, 2);
}

#else

static void scenario_disabled(void) {
  led_init();
  feed_const(32767, 480);
  led_audio_feed(NULL, 480);
  led_init();
  CHECK_INT(fake_strip.new_device_calls, 0);
  CHECK_INT(calls_now(), 0);
  CHECK_INT(fake_task.create_calls, 0);
  CHECK_INT(fake_rtsp_register_calls, 0);
  CHECK_INT(fake_logs.lines, 0);
}

#endif

int main(int argc, char **argv) {
  const char *name = argc > 1 ? argv[1] : "";
#ifdef CONFIG_ENABLE_RGB_AUDIO_LED
  if (strcmp(name, "ok") == 0)
    RUN(scenario_ok);
  else if (strcmp(name, "strip_fail") == 0)
    RUN(scenario_strip_fail);
  else if (strcmp(name, "task_fail") == 0)
    RUN(scenario_task_fail);
  else if (strcmp(name, "write_fail") == 0)
    RUN(scenario_write_fail);
  else if (strcmp(name, "register_fail") == 0)
    RUN(scenario_register_fail);
  else {
    fprintf(stderr, "unknown scenario '%s'\n", name);
    return 2;
  }
#else
  if (strcmp(name, "disabled") == 0)
    RUN(scenario_disabled);
  else {
    fprintf(stderr, "unknown scenario '%s'\n", name);
    return 2;
  }
#endif
  printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  /* The fake task thread is still blocked: leave without joining it. */
  fflush(stdout);
  _exit(g_failures ? 1 : 0);
}
