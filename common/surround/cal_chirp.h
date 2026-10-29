#pragma once

/* Room calibration test signal, shared by the speaker (which plays it) and
 * the Gateway (which looks for it in the microphone recording), so both use
 * exactly the same waveform at their own sample rate. */

#include <stdint.h>

typedef enum {
  CAL_CHIRP_WIDE = 0, /* full-range speakers: 300 Hz - 8 kHz, 20 ms */
  CAL_CHIRP_LOW = 1,  /* subwoofer: 30 - 150 Hz, 200 ms */
} cal_chirp_kind_t;

/* Chirps per measurement run. */
#define CAL_CHIRP_BURSTS 4

/* Length of one chirp and the time from one chirp's start to the next (the
 * room's echoes die down in between), seconds. */
double cal_chirp_length_s(cal_chirp_kind_t kind);
double cal_chirp_period_s(cal_chirp_kind_t kind);

/* The chirp at t seconds after its start, peak 1.0; 0 outside it. */
float cal_chirp_value(cal_chirp_kind_t kind, double t);
