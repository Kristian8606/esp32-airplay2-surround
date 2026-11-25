#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cal_chirp.h"
#include "esp_err.h"

/* Room EQ as REW (protocol 14): instead of the kind's fixed signal, a sweep
 * of `def` at peak `gain`, or with kind CAL_NOISE_PINK pink noise of RMS
 * `gain`. `bursts` is then the number of repeats (per driver with PER_WAY,
 * the drivers taking turns). */
typedef struct {
  bool custom;
  cal_sweep_def_t def;
  float gain;
} cal_play_spec_t;

/* Room calibration (spk_link.c, CAL_PLAY): play `bursts` test chirps
 * (cal_chirp.h, kind = cal_chirp_kind_t) at level_db below full scale
 * through the EQ, and give each chirp's start in esp_timer us at the sync
 * servo's reference point (before the output latency). Only between streams:
 * the audio engine is released for it and restored before returning.
 *
 * flags (SURROUND_CAL_*): RAW plays without the user's EQ (audio_eq_process_
 * raw); PER_WAY, with bi-amp outputs, plays one burst per driver instead of
 * `bursts`, tweeter first, the other output silent. way[k] (SURROUND_CAL_WAY_*,
 * room for SURROUND_CAL_BURSTS_MAX) says what played burst k. spec: NULL,
 * or a Room EQ sweep or noise (above). */
esp_err_t sound_engine_play_cal_chirps(int kind, int level_db, int bursts,
                                       uint8_t flags, const cal_play_spec_t *spec,
                                       int64_t *emit_us, uint8_t *way, int *n_emit);
