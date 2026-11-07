/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* The rules of the LED that need no ESP-IDF: the state, the colours and the
 * level meter. led.c wires them to FreeRTOS, esp_timer and led_strip. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtsp_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LED_STATE_STANDBY,
  LED_STATE_PAUSED,
  LED_STATE_PLAYING,
} led_state_t;

typedef enum {
  LED_PLAYING_VU,
  LED_PLAYING_STEADY,
  LED_PLAYING_OFF,
} led_playing_mode_t;

typedef enum {
  LED_PAUSED_STEADY,
  LED_PAUSED_OFF,
} led_paused_mode_t;

typedef enum {
  LED_STANDBY_OFF,
  LED_STANDBY_STEADY,
} led_standby_mode_t;

typedef enum {
  LED_COLOR_OFF, /* the LED cleared */
  LED_COLOR_RGB, /* r, g, b */
  LED_COLOR_HSV, /* h (0-360), s, v */
} led_color_kind_t;

typedef struct {
  led_color_kind_t kind;
  uint8_t r, g, b;
  uint16_t h;
  uint8_t s, v;
} led_color_t;

typedef struct {
  uint8_t brightness; /* B, 0-255 */
  uint8_t update_hz;  /* 5-60 */
  led_playing_mode_t playing;
  led_paused_mode_t paused;
  led_standby_mode_t standby;
  /* The clock in microseconds, and a short lock around the data that the
   * feed, the events and the task share. */
  int64_t (*now_us)(void *arg);
  void (*lock)(void *arg);
  void (*unlock)(void *arg);
  void *arg;
} led_core_config_t;

typedef struct {
  led_core_config_t cfg;
  int64_t period_us;

  /* Read by led_core_feed() without the lock: true while frames count. */
  atomic_bool counting;

  /* Under the lock. */
  led_state_t state;
  uint32_t play_epoch; /* +1 each time playing starts */
  uint64_t sum_sq;
  uint64_t samples;
  bool fed; /* a frame has been fed since playing started */
  int64_t last_feed_us;

  /* Only for the task. */
  led_state_t shown_state;
  uint32_t shown_epoch;
  double level_db; /* S, the shown level */
  int64_t next_update_us;
  led_color_t last;
  bool last_valid;
} led_core_t;

/* led_core_step() returns this when nothing is due until the next event. */
#define LED_CORE_WAIT_FOREVER INT64_MAX

/* How long without a fed frame counts as no audio. */
#define LED_CORE_NO_AUDIO_US 500000

/* The hold: how far the shown level may fall per update. */
#define LED_CORE_FALL_DB 1.5

/* False for a config out of range (update_hz outside 5-60, a mode outside
 * its enum, a missing callback). */
bool led_core_init(led_core_t *core, const led_core_config_t *cfg);

/* The event callback's part: records the state. False for an unknown
 * event (nothing changes); true when the task should wake. */
bool led_core_event(led_core_t *core, rtsp_event_t event);

/* The audio path's part: counts the frames while playing in VU mode. Never
 * waits for more than the short lock; reads exactly 2 * stereo_frames
 * slots. */
void led_core_feed(led_core_t *core, const int32_t *pcm, size_t stereo_frames);

/* The task's part: works out what the LED shows now. *out is the colour,
 * *changed tells whether it differs from the last one returned (the only
 * time the LED is written). Returns the microseconds until the next call
 * is due, or LED_CORE_WAIT_FOREVER. */
int64_t led_core_step(led_core_t *core, led_color_t *out, bool *changed);

/* Pure helpers. */
led_color_t led_core_steady(uint8_t base_r, uint8_t base_g, uint8_t base_b,
                            uint8_t brightness);
led_color_t led_core_state_color(const led_core_config_t *cfg,
                                 led_state_t state);
double led_core_level_db(uint64_t sum_sq, uint64_t samples);
led_color_t led_core_meter_color(double shown_db, uint8_t brightness);
bool led_core_color_eq(const led_color_t *a, const led_color_t *b);

#ifdef __cplusplus
}
#endif
