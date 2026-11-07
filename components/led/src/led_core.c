/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_core.h"

#include <math.h>
#include <string.h>

/* Base colours (R, G, B) before the brightness. */
#define PLAYING_R 0
#define PLAYING_G 96
#define PLAYING_B 16
#define PAUSED_R 0
#define PAUSED_G 0
#define PAUSED_B 96
#define STANDBY_R 0
#define STANDBY_G 40
#define STANDBY_B 0

/* 32768^2: the mean square of a full-scale 16-bit sample. */
#define FULL_SCALE_SQ 1073741824.0

static const led_color_t OFF = {.kind = LED_COLOR_OFF};

bool led_core_init(led_core_t *core, const led_core_config_t *cfg) {
  if (core == NULL || cfg == NULL || cfg->now_us == NULL ||
      cfg->lock == NULL || cfg->unlock == NULL)
    return false;
  if (cfg->update_hz < 5 || cfg->update_hz > 60)
    return false;
  if ((unsigned)cfg->playing > LED_PLAYING_OFF ||
      (unsigned)cfg->paused > LED_PAUSED_OFF ||
      (unsigned)cfg->standby > LED_STANDBY_STEADY)
    return false;

  memset(core, 0, sizeof(*core));
  core->cfg = *cfg;
  core->period_us = 1000000 / cfg->update_hz;
  atomic_init(&core->counting, false);
  core->state = LED_STATE_STANDBY;
  core->shown_state = LED_STATE_STANDBY;
  core->level_db = -INFINITY;
  return true;
}

led_color_t led_core_steady(uint8_t base_r, uint8_t base_g, uint8_t base_b,
                            uint8_t brightness) {
  led_color_t c = {
      .kind = LED_COLOR_RGB,
      .r = (uint8_t)(base_r * brightness / 255),
      .g = (uint8_t)(base_g * brightness / 255),
      .b = (uint8_t)(base_b * brightness / 255),
  };
  if (c.r == 0 && c.g == 0 && c.b == 0)
    return OFF;
  return c;
}

/* For playing in VU mode this is the no-audio colour: the paused one. */
led_color_t led_core_state_color(const led_core_config_t *cfg,
                                 led_state_t state) {
  uint8_t bri = cfg->brightness;
  switch (state) {
  case LED_STATE_PLAYING:
    if (cfg->playing == LED_PLAYING_STEADY)
      return led_core_steady(PLAYING_R, PLAYING_G, PLAYING_B, bri);
    if (cfg->playing == LED_PLAYING_OFF)
      return OFF;
    /* fall through - VU without audio */
  case LED_STATE_PAUSED:
    if (cfg->paused == LED_PAUSED_STEADY)
      return led_core_steady(PAUSED_R, PAUSED_G, PAUSED_B, bri);
    return OFF;
  case LED_STATE_STANDBY:
  default:
    if (cfg->standby == LED_STANDBY_STEADY)
      return led_core_steady(STANDBY_R, STANDBY_G, STANDBY_B, bri);
    return OFF;
  }
}

double led_core_level_db(uint64_t sum_sq, uint64_t samples) {
  if (samples == 0 || sum_sq == 0)
    return -INFINITY;
  double ms = (double)sum_sq / (double)samples;
  return 10.0 * log10(ms / FULL_SCALE_SQ);
}

led_color_t led_core_meter_color(double shown_db, uint8_t brightness) {
  double t = (shown_db + 50.0) / 50.0;
  if (!(t > 0.0)) /* also -inf and NaN */
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  long v = lround(brightness * t * t);
  if (v <= 0)
    return OFF;
  led_color_t c = {
      .kind = LED_COLOR_HSV,
      .h = (uint16_t)lround(240.0 * (1.0 - t)),
      .s = 255,
      .v = (uint8_t)(v > 255 ? 255 : v),
  };
  return c;
}

bool led_core_color_eq(const led_color_t *a, const led_color_t *b) {
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case LED_COLOR_RGB:
    return a->r == b->r && a->g == b->g && a->b == b->b;
  case LED_COLOR_HSV:
    return a->h == b->h && a->s == b->s && a->v == b->v;
  case LED_COLOR_OFF:
  default:
    return true;
  }
}

bool led_core_event(led_core_t *core, rtsp_event_t event) {
  led_state_t next;
  switch (event) {
  case RTSP_EVENT_DISCONNECTED:
    next = LED_STATE_STANDBY;
    break;
  case RTSP_EVENT_CLIENT_CONNECTED:
  case RTSP_EVENT_PAUSED:
    next = LED_STATE_PAUSED;
    break;
  case RTSP_EVENT_PLAYING:
    next = LED_STATE_PLAYING;
    break;
  default:
    return false;
  }

  core->cfg.lock(core->cfg.arg);
  /* PLAYING while playing changes nothing; any other state in between,
   * however short, starts playing again (play_epoch). */
  if (next == LED_STATE_PLAYING && core->state != LED_STATE_PLAYING) {
    core->play_epoch++;
    core->sum_sq = 0;
    core->samples = 0;
    core->fed = false;
  }
  core->state = next;
  atomic_store_explicit(&core->counting,
                        next == LED_STATE_PLAYING &&
                            core->cfg.playing == LED_PLAYING_VU,
                        memory_order_relaxed);
  core->cfg.unlock(core->cfg.arg);
  return true;
}

void led_core_feed(led_core_t *core, const int32_t *pcm,
                   size_t stereo_frames) {
  if (pcm == NULL || stereo_frames == 0 || stereo_frames > SIZE_MAX / 2)
    return;
  if (!atomic_load_explicit(&core->counting, memory_order_relaxed))
    return;

  size_t n = stereo_frames * 2;
  uint64_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t s = (int16_t)(pcm[i] >> 16);
    sum += (uint32_t)(s * s);
  }
  int64_t now = core->cfg.now_us(core->cfg.arg);

  core->cfg.lock(core->cfg.arg);
  /* Playing may have ended while the frames were summed. */
  if (atomic_load_explicit(&core->counting, memory_order_relaxed)) {
    core->sum_sq += sum;
    core->samples += n;
    core->fed = true;
    core->last_feed_us = now;
  }
  core->cfg.unlock(core->cfg.arg);
}

int64_t led_core_step(led_core_t *core, led_color_t *out, bool *changed) {
  const led_core_config_t *cfg = &core->cfg;
  bool vu = cfg->playing == LED_PLAYING_VU;
  int64_t now = cfg->now_us(cfg->arg);
  uint64_t sum = 0, samples = 0;
  bool fed;
  int64_t last_feed;

  cfg->lock(cfg->arg);
  led_state_t state = core->state;
  uint32_t epoch = core->play_epoch;
  bool started = state == LED_STATE_PLAYING &&
                 (core->shown_state != LED_STATE_PLAYING ||
                  core->shown_epoch != epoch);
  bool due = vu && state == LED_STATE_PLAYING && !started &&
             now >= core->next_update_us;
  if (due) {
    sum = core->sum_sq;
    samples = core->samples;
    core->sum_sq = 0;
    core->samples = 0;
  }
  fed = core->fed;
  last_feed = core->last_feed_us;
  cfg->unlock(cfg->arg);

  led_color_t color;
  int64_t wait = LED_CORE_WAIT_FOREVER;

  if (state != LED_STATE_PLAYING || !vu) {
    color = led_core_state_color(cfg, state);
  } else if (started) {
    /* No frame since playing started yet. */
    core->level_db = -INFINITY;
    core->next_update_us = now + core->period_us;
    color = led_core_state_color(cfg, LED_STATE_PLAYING);
    wait = core->period_us;
  } else if (!due) {
    /* Woken by an event between two updates: nothing new to show. */
    color = core->last;
    wait = core->next_update_us - now;
  } else {
    if (!fed || now - last_feed >= LED_CORE_NO_AUDIO_US) {
      core->level_db = -INFINITY;
      color = led_core_state_color(cfg, LED_STATE_PLAYING);
    } else if (samples == 0) {
      /* Fed less than 500 ms ago, but not since the last update: the
       * update is skipped, S and the colour stay (rule 1). */
      color = core->last;
    } else {
      double level = led_core_level_db(sum, samples);
      double held = core->level_db - LED_CORE_FALL_DB;
      core->level_db = level > held ? level : held;
      color = led_core_meter_color(core->level_db, cfg->brightness);
    }
    core->next_update_us += core->period_us;
    if (core->next_update_us <= now)
      core->next_update_us = now + core->period_us;
    wait = core->next_update_us - now;
  }

  core->shown_state = state;
  core->shown_epoch = epoch;
  *changed = !core->last_valid || !led_core_color_eq(&color, &core->last);
  core->last = color;
  core->last_valid = true;
  *out = color;
  return wait;
}
