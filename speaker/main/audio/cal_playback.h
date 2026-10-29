#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Room calibration (spk_link.c, CAL_PLAY): play `bursts` test chirps
 * (cal_chirp.h, kind = cal_chirp_kind_t) at level_db below full scale
 * through the EQ, and give each chirp's start in esp_timer us at the sync
 * servo's reference point (before the output latency). Only between streams:
 * the audio engine is released for it and restored before returning. */
esp_err_t audio_receiver_play_cal_chirps(int kind, int level_db, int bursts,
                                         int64_t *emit_us, int *n_emit);
