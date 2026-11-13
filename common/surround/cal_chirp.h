#pragma once

/* Room calibration test signals, shared by the speaker (which plays them)
 * and the Gateway (which looks for them in the microphone recording), so
 * both use exactly the same waveform at their own sample rate.
 *
 * Chirps: short linear sweeps for the travel time and level of every speaker.
 * Sweeps: long exponential sweeps for one speaker's frequency response (the
 * Room EQ page deconvolves them; the definition is in /api/calibration). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  CAL_CHIRP_WIDE = 0, /* full-range speakers: 300 Hz - 8 kHz, 20 ms */
  CAL_CHIRP_LOW = 1,  /* subwoofer: 30 - 150 Hz, 200 ms */
  CAL_SWEEP_WIDE = 2, /* full-range speakers: 30 Hz - 21 kHz, 2.5 s */
  CAL_SWEEP_LOW = 3,  /* subwoofer: 15 - 400 Hz, 2.5 s */
  CAL_CHIRP_KIND_COUNT
} cal_chirp_kind_t;

/* Chirps per measurement run. */
#define CAL_CHIRP_BURSTS 4

/* Any other value is a wide chirp. */
cal_chirp_kind_t cal_chirp_kind(int kind);
bool cal_chirp_is_sweep(cal_chirp_kind_t kind);
bool cal_chirp_is_low(cal_chirp_kind_t kind);
const char *cal_chirp_name(cal_chirp_kind_t kind);

/* Length of one chirp and the time from one chirp's start to the next (the
 * room's echoes die down in between), seconds. */
double cal_chirp_length_s(cal_chirp_kind_t kind);
double cal_chirp_period_s(cal_chirp_kind_t kind);

/* The signal at t seconds after its start, peak 1.0; 0 outside it. Double
 * precision: the reference. */
float cal_chirp_value(cal_chirp_kind_t kind, double t);

/* An exponential sweep: phase 2 pi f0 L (exp(t / L) - 1) with
 * L = length / ln(f1 / f0), raised-cosine fades at both ends. */
typedef struct {
  double f0_hz, f1_hz, length_s, fade_in_s, fade_out_s;
} cal_sweep_def_t;
/* False for a chirp. */
bool cal_sweep_def(cal_chirp_kind_t kind, cal_sweep_def_t *out);

/* frames samples of a sweep from sample `start` on (its own sample count at
 * `rate`), times gain, into dst[i * stride]; zero outside the sweep. Single
 * precision for the speaker's playout, within 1e-3 rad of the reference
 * phase. */
void cal_sweep_fill(cal_chirp_kind_t kind, uint32_t start, uint32_t rate,
                    float gain, float *dst, size_t frames, size_t stride);
