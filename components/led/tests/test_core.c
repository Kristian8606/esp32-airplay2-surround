/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* The rules of src/led_core.c against spec/spec.md, with a fake clock and a
 * mutex for the lock. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "led_core.h"

#define PERIOD_30HZ 33333

static _Atomic int64_t g_now = 1000000;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_lock_depth;

static int64_t fake_now(void *arg) {
  (void)arg;
  return atomic_load(&g_now);
}

static void fake_lock(void *arg) {
  pthread_mutex_lock((pthread_mutex_t *)arg);
  CHECK(atomic_fetch_add(&g_lock_depth, 1) == 0);
}

static void fake_unlock(void *arg) {
  atomic_fetch_sub(&g_lock_depth, 1);
  pthread_mutex_unlock((pthread_mutex_t *)arg);
}

static led_core_t core;

static led_core_config_t make_cfg(uint8_t brightness, uint8_t hz,
                                  led_playing_mode_t playing,
                                  led_paused_mode_t paused,
                                  led_standby_mode_t standby) {
  led_core_config_t cfg = {
      .brightness = brightness,
      .update_hz = hz,
      .playing = playing,
      .paused = paused,
      .standby = standby,
      .now_us = fake_now,
      .lock = fake_lock,
      .unlock = fake_unlock,
      .arg = &g_mutex,
  };
  return cfg;
}

static void setup(uint8_t brightness, led_playing_mode_t playing,
                  led_paused_mode_t paused, led_standby_mode_t standby) {
  led_core_config_t cfg =
      make_cfg(brightness, 30, playing, paused, standby);
  CHECK(led_core_init(&core, &cfg));
}

static void setup_vu(void) {
  setup(96, LED_PLAYING_VU, LED_PAUSED_STEADY, LED_STANDBY_OFF);
}

static led_color_t step_w(int64_t *wait, bool *changed) {
  led_color_t c;
  bool ch;
  int64_t w = led_core_step(&core, &c, &ch);
  if (wait)
    *wait = w;
  if (changed)
    *changed = ch;
  return c;
}

static led_color_t step(void) { return step_w(NULL, NULL); }

static led_color_t update(void) {
  atomic_fetch_add(&g_now, PERIOD_30HZ);
  return step();
}

/* Exactly frames * 2 slots on the heap, so that ASan sees any read past. */
static void feed_lr(int16_t left, int16_t right, size_t frames) {
  int32_t *pcm = malloc((frames ? frames : 1) * 2 * sizeof(int32_t));
  for (size_t i = 0; i < frames; i++) {
    pcm[2 * i] = (int32_t)((uint32_t)(uint16_t)left << 16) | 0x1234;
    pcm[2 * i + 1] = (int32_t)((uint32_t)(uint16_t)right << 16) | 0xffff;
  }
  led_core_feed(&core, pcm, frames);
  free(pcm);
}

static void feed(int16_t x, size_t frames) { feed_lr(x, x, frames); }

/* One update's worth of audio at 48 kHz: about 3.3 feeds of 480 frames. */
static void feed_window(int16_t x) {
  for (int i = 0; i < 3; i++)
    feed(x, 480);
}

static const led_color_t OFF = {.kind = LED_COLOR_OFF};

#define RGB(r_, g_, b_) {.kind = LED_COLOR_RGB, .r = r_, .g = g_, .b = b_}
#define HSV(h_, v_) {.kind = LED_COLOR_HSV, .h = h_, .s = 255, .v = v_}
#define OFF_C {.kind = LED_COLOR_OFF}

static led_color_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  led_color_t c = {.kind = LED_COLOR_RGB, .r = r, .g = g, .b = b};
  return c;
}

static led_color_t hsv(uint16_t h, uint8_t v) {
  led_color_t c = {.kind = LED_COLOR_HSV, .h = h, .s = 255, .v = v};
  return c;
}

static void print_color(const char *what, led_color_t c) {
  if (c.kind == LED_COLOR_OFF)
    fprintf(stderr, "  %s: off\n", what);
  else if (c.kind == LED_COLOR_RGB)
    fprintf(stderr, "  %s: rgb %d,%d,%d\n", what, c.r, c.g, c.b);
  else
    fprintf(stderr, "  %s: hsv %d,%d,%d\n", what, c.h, c.s, c.v);
}

#define CHECK_COLOR(actual, expected)                                          \
  do {                                                                         \
    led_color_t a__ = (actual), e__ = (expected);                              \
    if (!led_core_color_eq(&a__, &e__)) {                                      \
      fprintf(stderr, "%s:%d: colour of %s differs\n", __FILE__, __LINE__,     \
              #actual);                                                        \
      print_color("got", a__);                                                 \
      print_color("expected", e__);                                            \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

/* ---- colours ---- */

static void test_colour_table(void) {
  static const struct {
    uint8_t bri;
    led_color_t playing, paused, standby;
  } rows[] = {
      {96, RGB(0, 36, 6), RGB(0, 0, 36), RGB(0, 15, 0)},
      {255, RGB(0, 96, 16), RGB(0, 0, 96), RGB(0, 40, 0)},
      {10, RGB(0, 3, 0), RGB(0, 0, 3), RGB(0, 1, 0)},
      {0, OFF_C, OFF_C, OFF_C},
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    CHECK_COLOR(led_core_steady(0, 96, 16, rows[i].bri), rows[i].playing);
    CHECK_COLOR(led_core_steady(0, 0, 96, rows[i].bri), rows[i].paused);
    CHECK_COLOR(led_core_steady(0, 40, 0, rows[i].bri), rows[i].standby);

    led_core_config_t cfg = make_cfg(rows[i].bri, 30, LED_PLAYING_STEADY,
                                     LED_PAUSED_STEADY, LED_STANDBY_STEADY);
    CHECK_COLOR(led_core_state_color(&cfg, LED_STATE_PLAYING),
                rows[i].playing);
    CHECK_COLOR(led_core_state_color(&cfg, LED_STATE_PAUSED), rows[i].paused);
    CHECK_COLOR(led_core_state_color(&cfg, LED_STATE_STANDBY),
                rows[i].standby);
  }
}

/* B = 0: the LED is always off, also as a level meter. */
static void test_brightness_zero_meter(void) {
  setup(0, LED_PLAYING_VU, LED_PAUSED_STEADY, LED_STANDBY_STEADY);
  CHECK_COLOR(step(), OFF);
  led_core_event(&core, RTSP_EVENT_PLAYING);
  CHECK_COLOR(step(), OFF);
  feed_window(32767);
  CHECK_COLOR(update(), OFF);
}

/* ---- states ---- */

static void test_states_each_mode(void) {
  for (int p = LED_PLAYING_VU; p <= LED_PLAYING_OFF; p++)
    for (int q = LED_PAUSED_STEADY; q <= LED_PAUSED_OFF; q++)
      for (int s = LED_STANDBY_OFF; s <= LED_STANDBY_STEADY; s++) {
        setup(96, p, q, s);
        led_color_t standby = s == LED_STANDBY_STEADY ? rgb(0, 15, 0) : OFF;
        led_color_t paused = q == LED_PAUSED_STEADY ? rgb(0, 0, 36) : OFF;
        /* VU without audio shows the paused colour. */
        led_color_t playing = p == LED_PLAYING_STEADY ? rgb(0, 36, 6)
                              : p == LED_PLAYING_OFF  ? OFF
                                                      : paused;
        int64_t wait;
        CHECK_COLOR(step_w(&wait, NULL), standby);
        CHECK(wait == LED_CORE_WAIT_FOREVER);

        CHECK(led_core_event(&core, RTSP_EVENT_CLIENT_CONNECTED));
        CHECK_COLOR(step_w(&wait, NULL), paused);
        CHECK(wait == LED_CORE_WAIT_FOREVER);

        CHECK(led_core_event(&core, RTSP_EVENT_PLAYING));
        CHECK_COLOR(step_w(&wait, NULL), playing);
        if (p == LED_PLAYING_VU)
          CHECK_INT(wait, PERIOD_30HZ);
        else
          CHECK(wait == LED_CORE_WAIT_FOREVER);

        CHECK(led_core_event(&core, RTSP_EVENT_PAUSED));
        CHECK_COLOR(step_w(&wait, NULL), paused);
        CHECK(wait == LED_CORE_WAIT_FOREVER);

        CHECK(led_core_event(&core, RTSP_EVENT_PLAYING));
        CHECK_COLOR(step(), playing);

        CHECK(led_core_event(&core, RTSP_EVENT_DISCONNECTED));
        CHECK_COLOR(step_w(&wait, NULL), standby);
        CHECK(wait == LED_CORE_WAIT_FOREVER);

        /* A playing session can also end without a pause. */
        CHECK(led_core_event(&core, RTSP_EVENT_CLIENT_CONNECTED));
        CHECK(led_core_event(&core, RTSP_EVENT_DISCONNECTED));
        CHECK_COLOR(step(), standby);
      }
}

static void test_unknown_event(void) {
  setup_vu();
  led_core_event(&core, RTSP_EVENT_CLIENT_CONNECTED);
  CHECK(!led_core_event(&core, (rtsp_event_t)42));
  CHECK(!led_core_event(&core, (rtsp_event_t)-1));
  CHECK_INT(core.state, LED_STATE_PAUSED);
  CHECK_COLOR(step(), rgb(0, 0, 36));
}

/* The LED is written only when its colour changes. */
static void test_changed_only_on_change(void) {
  setup(96, LED_PLAYING_STEADY, LED_PAUSED_STEADY, LED_STANDBY_OFF);
  bool changed;
  step_w(NULL, &changed);
  CHECK(changed); /* the first colour is always written */
  step_w(NULL, &changed);
  CHECK(!changed);
  led_core_event(&core, RTSP_EVENT_CLIENT_CONNECTED);
  step_w(NULL, &changed);
  CHECK(changed);
  led_core_event(&core, RTSP_EVENT_PAUSED); /* same colour */
  step_w(NULL, &changed);
  CHECK(!changed);
}

static void test_bad_config(void) {
  led_core_config_t cfg = make_cfg(96, 30, LED_PLAYING_VU, LED_PAUSED_STEADY,
                                   LED_STANDBY_OFF);
  led_core_t c;
  CHECK(led_core_init(&c, &cfg));
  cfg.update_hz = 4;
  CHECK(!led_core_init(&c, &cfg));
  cfg.update_hz = 61;
  CHECK(!led_core_init(&c, &cfg));
  cfg.update_hz = 0;
  CHECK(!led_core_init(&c, &cfg));
  cfg.update_hz = 5;
  CHECK(led_core_init(&c, &cfg));
  CHECK_INT(c.period_us, 200000);
  cfg.update_hz = 60;
  CHECK(led_core_init(&c, &cfg));
  cfg.playing = (led_playing_mode_t)7;
  CHECK(!led_core_init(&c, &cfg));
  cfg.playing = LED_PLAYING_VU;
  cfg.lock = NULL;
  CHECK(!led_core_init(&c, &cfg));
  CHECK(!led_core_init(&c, NULL));
  CHECK(!led_core_init(NULL, &cfg));
}

/* ---- the level meter ---- */

static void start_playing(void) {
  setup_vu();
  step();
  led_core_event(&core, RTSP_EVENT_PLAYING);
  /* No frame since playing started: the paused colour. */
  CHECK_COLOR(step(), rgb(0, 0, 36));
}

static void test_meter_examples(void) {
  static const struct {
    int16_t x;
    double level;
    led_color_t color;
  } rows[] = {
      {32767, -0.0003, HSV(0, 96)},
      {16384, -6.02, HSV(29, 74)},
      {3277, -20.0, HSV(96, 35)},
      {328, -39.99, HSV(192, 4)},
      {33, -59.94, OFF_C},
      {0, -INFINITY, OFF_C},
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    start_playing();
    feed_window(rows[i].x);
    CHECK_COLOR(update(), rows[i].color);
    if (isinf(rows[i].level))
      CHECK(isinf(core.level_db) && core.level_db < 0);
    else
      CHECK_NEAR(core.level_db, rows[i].level, 0.005);
    /* Settled: the same again. */
    feed_window(rows[i].x);
    CHECK_COLOR(update(), rows[i].color);
  }

  /* Sign and the low 16 bits do not matter; -32768 is full scale. */
  start_playing();
  feed_lr(16384, -16384, 480);
  CHECK_COLOR(update(), hsv(29, 74));
  start_playing();
  feed(-32768, 480);
  CHECK_COLOR(update(), hsv(0, 96));
  CHECK_NEAR(core.level_db, 0.0, 1e-9);
  /* Left and right both count: full scale in one channel only is -3 dB. */
  start_playing();
  feed_lr(-32768, 0, 480);
  update();
  CHECK_NEAR(core.level_db, -3.0103, 0.0001);
}

static void test_meter_pure(void) {
  CHECK(isinf(led_core_level_db(0, 0)));
  CHECK(isinf(led_core_level_db(0, 960)));
  CHECK(isinf(led_core_level_db(12345, 0)));
  CHECK_NEAR(led_core_level_db(960ull * 16384 * 16384, 960), -6.0206, 0.0001);
  CHECK_COLOR(led_core_meter_color(-INFINITY, 96), OFF);
  CHECK_COLOR(led_core_meter_color(NAN, 96), OFF);
  CHECK_COLOR(led_core_meter_color(-50.0, 96), OFF);
  CHECK_COLOR(led_core_meter_color(10.0, 96), hsv(0, 96));
  CHECK_COLOR(led_core_meter_color(0.0, 255), hsv(0, 255));
  CHECK_COLOR(led_core_meter_color(-25.0, 96), hsv(120, 24));
}

/* From 16384 to 328: -7.52, -9.02, -10.52 ... -39.99 on the 23rd update. */
static void test_hold(void) {
  start_playing();
  feed_window(16384);
  update();
  double settled = core.level_db;
  CHECK_NEAR(settled, -6.02, 0.005);
  for (int n = 1; n <= 23; n++) {
    feed_window(328);
    update();
    if (n == 1)
      CHECK_NEAR(core.level_db, -7.52, 0.005);
    if (n == 2)
      CHECK_NEAR(core.level_db, -9.02, 0.005);
    if (n == 3)
      CHECK_NEAR(core.level_db, -10.52, 0.005);
    if (n < 23)
      CHECK_NEAR(core.level_db, settled - 1.5 * n, 1e-9);
  }
  CHECK_NEAR(core.level_db, -39.99, 0.005);
  CHECK_COLOR(led_core_meter_color(core.level_db, 96), hsv(192, 4));
  /* And it rises at once. */
  feed_window(16384);
  CHECK_COLOR(update(), hsv(29, 74));
}

/* An event between two updates is not an update: the hold does not fall. */
static void test_event_between_updates(void) {
  start_playing();
  feed_window(16384);
  update();
  double s = core.level_db;
  atomic_fetch_add(&g_now, PERIOD_30HZ / 2);
  led_core_event(&core, RTSP_EVENT_PLAYING); /* already playing */
  int64_t wait;
  bool changed;
  CHECK_COLOR(step_w(&wait, &changed), hsv(29, 74));
  CHECK(!changed);
  CHECK(wait > 0 && wait <= PERIOD_30HZ / 2 + 1);
  CHECK_NEAR(core.level_db, s, 0);
  /* The frames fed in between still count at the next update. */
  feed_window(32767);
  atomic_fetch_add(&g_now, PERIOD_30HZ - PERIOD_30HZ / 2);
  CHECK_COLOR(step(), hsv(0, 96));
}

/* Playing again after a pause starts the hold again at -inf. */
static void test_hold_restarts(void) {
  start_playing();
  feed_window(32767);
  update();
  led_core_event(&core, RTSP_EVENT_PAUSED);
  led_core_event(&core, RTSP_EVENT_PLAYING); /* before the task wakes */
  CHECK_COLOR(step(), rgb(0, 0, 36));
  CHECK(isinf(core.level_db));
  feed_window(328);
  CHECK_COLOR(update(), hsv(192, 4)); /* not held from 0 dB */
}

static void test_no_audio_500ms(void) {
  start_playing();
  /* None since playing started: the paused colour on every update. */
  for (int i = 0; i < 20; i++) {
    bool changed;
    update();
    CHECK_COLOR(step_w(NULL, &changed), rgb(0, 0, 36));
    CHECK(!changed);
  }

  start_playing();
  int64_t start = atomic_load(&g_now);
  atomic_store(&g_now, start + 1000);
  feed(16384, 480); /* the last frame, at start + 1 ms */
  atomic_store(&g_now, start);
  for (int k = 1; k <= 15; k++) /* start + 33.3 ms ... 500.0 ms - 1 ms */
    CHECK_COLOR(update(), hsv(29, 74));
  CHECK_COLOR(update(), rgb(0, 0, 36)); /* 16 * 33.3 ms - 1 ms >= 500 ms */
  CHECK(isinf(core.level_db));
  /* Audio again: the level rises at once, not held from before. */
  feed_window(328);
  CHECK_COLOR(update(), hsv(192, 4));

  /* With _PAUSED_OFF the LED is off instead. */
  setup(96, LED_PLAYING_VU, LED_PAUSED_OFF, LED_STANDBY_STEADY);
  step();
  led_core_event(&core, RTSP_EVENT_PLAYING);
  CHECK_COLOR(step(), OFF);
  feed_window(16384);
  CHECK_COLOR(update(), hsv(29, 74));
  for (int k = 0; k < 15; k++)
    update();
  CHECK_COLOR(update(), OFF);
}

/* An exact 500 ms gap counts as no audio. */
static void test_no_audio_boundary(void) {
  start_playing();
  feed(16384, 480);
  int64_t fed_at = atomic_load(&g_now);
  CHECK_COLOR(update(), hsv(29, 74));
  atomic_store(&g_now, fed_at + 499999);
  core.next_update_us = fed_at + 499999;
  CHECK_COLOR(step(), hsv(29, 74));
  atomic_store(&g_now, fed_at + 500000);
  core.next_update_us = fed_at + 500000;
  CHECK_COLOR(step(), rgb(0, 0, 36));
}

static void test_frames_outside_playing(void) {
  setup_vu();
  step();
  feed(32767, 4800); /* standby */
  led_core_event(&core, RTSP_EVENT_CLIENT_CONNECTED);
  step();
  feed(32767, 4800); /* paused */
  CHECK_INT(core.samples, 0);
  led_core_event(&core, RTSP_EVENT_PLAYING);
  CHECK_COLOR(step(), rgb(0, 0, 36));
  CHECK_COLOR(update(), rgb(0, 0, 36)); /* none of them counted */

  feed_window(16384);
  led_core_event(&core, RTSP_EVENT_PAUSED);
  feed(32767, 4800);
  led_core_event(&core, RTSP_EVENT_PLAYING);
  CHECK_INT(core.samples, 0); /* playing started again: nothing kept */
  step();
  CHECK_COLOR(update(), rgb(0, 0, 36));

  /* Not counted while playing in a steady mode either. */
  setup(96, LED_PLAYING_STEADY, LED_PAUSED_STEADY, LED_STANDBY_OFF);
  led_core_event(&core, RTSP_EVENT_PLAYING);
  feed(32767, 480);
  CHECK_INT(core.samples, 0);
  CHECK_COLOR(step(), rgb(0, 36, 6));
}

static void test_odd_feeds(void) {
  start_playing();
  int32_t two[2] = {16384 << 16, 16384 << 16};
  led_core_feed(&core, NULL, 480);
  led_core_feed(&core, NULL, 0);
  led_core_feed(&core, two, 0);
  led_core_feed(&core, two, SIZE_MAX);         /* 2 * n overflows */
  led_core_feed(&core, two, SIZE_MAX / 2 + 1); /* 2 * n overflows */
  CHECK_INT(core.samples, 0);
  CHECK(!core.fed);
  CHECK_COLOR(update(), rgb(0, 0, 36));

  /* One frame. */
  feed(16384, 1);
  CHECK_INT(core.samples, 2);
  CHECK_COLOR(update(), hsv(29, 74));

  /* Many frames at once: 2 s of full scale. */
  feed(32767, 96000);
  CHECK_COLOR(update(), hsv(0, 96));

  /* 4 M frames of -32768 in one window: the sums do not overflow. */
  size_t big = 4u * 1024 * 1024;
  int32_t *pcm = malloc(big * 2 * sizeof(int32_t));
  for (size_t i = 0; i < big * 2; i++)
    pcm[i] = INT32_MIN;
  for (int i = 0; i < 16; i++)
    led_core_feed(&core, pcm, big);
  free(pcm);
  CHECK_COLOR(update(), hsv(0, 96));
  CHECK_NEAR(core.level_db, 0.0, 1e-9);
}

/* CLIENT_CONNECTED while playing: paused, in every mode; PLAYING after it
 * starts playing again. */
static void test_client_connected_while_playing(void) {
  for (int p = LED_PLAYING_VU; p <= LED_PLAYING_OFF; p++) {
    setup(96, p, LED_PAUSED_STEADY, LED_STANDBY_OFF);
    led_core_event(&core, RTSP_EVENT_PLAYING);
    step();
    if (p == LED_PLAYING_VU) {
      feed_window(32767);
      CHECK_COLOR(update(), hsv(0, 96));
    }
    led_core_event(&core, RTSP_EVENT_CLIENT_CONNECTED);
    CHECK_INT(core.state, LED_STATE_PAUSED);
    CHECK_COLOR(step(), rgb(0, 0, 36));
    led_core_event(&core, RTSP_EVENT_PLAYING);
    if (p == LED_PLAYING_VU) {
      CHECK_COLOR(step(), rgb(0, 0, 36)); /* no frame since the new start */
      CHECK(isinf(core.level_db));
      feed_window(328);
      CHECK_COLOR(update(), hsv(192, 4)); /* not held from 0 dB */
    } else {
      CHECK_COLOR(step(), p == LED_PLAYING_STEADY ? rgb(0, 36, 6) : OFF);
    }
  }
}

/* PLAYING while already playing changes nothing: the hold, the frames fed
 * and the 500 ms clock all go on. */
static void test_playing_while_playing(void) {
  start_playing();
  feed_window(16384);
  CHECK_COLOR(update(), hsv(29, 74));
  uint32_t epoch = core.play_epoch;
  led_core_event(&core, RTSP_EVENT_PLAYING);
  bool changed;
  CHECK_COLOR(step_w(NULL, &changed), hsv(29, 74));
  CHECK(!changed);
  CHECK_INT(core.play_epoch, epoch);
  CHECK(core.fed);
  feed_window(328);
  led_core_event(&core, RTSP_EVENT_PLAYING);
  CHECK(core.samples > 0); /* not dropped */
  update();
  CHECK_NEAR(core.level_db, -7.52, 0.005); /* held, not restarted at -inf */
  /* No new frames: skipped, not the no-audio colour. */
  led_core_event(&core, RTSP_EVENT_PLAYING);
  led_color_t before = core.last;
  CHECK_COLOR(update(), before);
}

/* Rule 1: an update with no frames, less than 500 ms without frames, is
 * skipped: S and the colour stay. */
static void test_skipped_update(void) {
  start_playing();
  feed_window(16384);
  update();
  feed_window(328);
  update(); /* S = -7.52, falling */
  double s = core.level_db;
  for (int i = 0; i < 10; i++) { /* 333 ms */
    bool changed;
    atomic_fetch_add(&g_now, PERIOD_30HZ);
    CHECK_COLOR(step_w(NULL, &changed), led_core_meter_color(s, 96));
    CHECK(!changed);
    CHECK_NEAR(core.level_db, s, 0);
  }
  /* Frames again: the hold goes on from the S that stayed. */
  feed_window(328);
  update();
  CHECK_NEAR(core.level_db, s - 1.5, 1e-9);
}

/* Rule 3: round is lround(), halves away from zero. S = -25 dB gives
 * t = 0.5 exactly, so v = B / 4 and h = 120. */
static void test_round_halves(void) {
  CHECK_COLOR(led_core_meter_color(-25.0, 2), hsv(120, 1));  /* 0.5 -> 1 */
  CHECK_COLOR(led_core_meter_color(-25.0, 6), hsv(120, 2));  /* 1.5 -> 2 */
  CHECK_COLOR(led_core_meter_color(-25.0, 10), hsv(120, 3)); /* 2.5 -> 3 */
  CHECK_COLOR(led_core_meter_color(-25.0, 1), OFF); /* 0.25 -> 0: off */
}

/* ---- threads ---- */

static atomic_bool g_stop;

static void *feeder(void *arg) {
  (void)arg;
  unsigned seed = 1;
  int32_t *pcm = malloc(480 * 2 * sizeof(int32_t));
  while (!atomic_load(&g_stop)) {
    size_t n = (size_t)(rand_r(&seed) % 481); /* 0 to 480 frames */
    for (size_t i = 0; i < n * 2; i++)
      pcm[i] = (int32_t)((uint32_t)rand_r(&seed) << 1);
    led_core_feed(&core, n ? pcm : NULL, n);
  }
  free(pcm);
  return NULL;
}

static void test_threads(void) {
  for (int mode = LED_PLAYING_VU; mode <= LED_PLAYING_OFF; mode++) {
    setup(96, mode, LED_PAUSED_STEADY, LED_STANDBY_STEADY);
    atomic_store(&g_stop, false);
    pthread_t t[2];
    pthread_create(&t[0], NULL, feeder, NULL);
    pthread_create(&t[1], NULL, feeder, NULL);
    unsigned seed = 7;
    static const rtsp_event_t events[] = {
        RTSP_EVENT_CLIENT_CONNECTED, RTSP_EVENT_PLAYING, RTSP_EVENT_PLAYING,
        RTSP_EVENT_PLAYING,          RTSP_EVENT_PAUSED,  RTSP_EVENT_DISCONNECTED,
    };
    for (int i = 0; i < 20000; i++) {
      if (rand_r(&seed) % 8 == 0)
        led_core_event(&core, events[rand_r(&seed) % 6]);
      atomic_fetch_add(&g_now, 1000 + rand_r(&seed) % 40000);
      led_color_t c = step();
      CHECK(c.kind <= LED_COLOR_HSV);
      if (c.kind == LED_COLOR_HSV)
        CHECK(c.v >= 1 && c.v <= 96 && c.h <= 240 && c.s == 255);
    }
    atomic_store(&g_stop, true);
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  RUN(test_colour_table);
  RUN(test_brightness_zero_meter);
  RUN(test_states_each_mode);
  RUN(test_unknown_event);
  RUN(test_changed_only_on_change);
  RUN(test_bad_config);
  RUN(test_meter_examples);
  RUN(test_meter_pure);
  RUN(test_hold);
  RUN(test_event_between_updates);
  RUN(test_hold_restarts);
  RUN(test_no_audio_500ms);
  RUN(test_no_audio_boundary);
  RUN(test_frames_outside_playing);
  RUN(test_odd_feeds);
  RUN(test_client_connected_while_playing);
  RUN(test_playing_while_playing);
  RUN(test_skipped_update);
  RUN(test_round_halves);
  RUN(test_threads);
  printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
