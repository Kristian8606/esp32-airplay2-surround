#include "cal_chirp.h"

#include <math.h>

typedef struct {
  double f0_hz, f1_hz, len_s, period_s;
} chirp_def_t;

/* A linear sweep: wide enough for a sharp correlation peak (timing), long
 * enough for energy above the room noise. The subwoofer gets a sweep it can
 * play through its low-pass. */
static const chirp_def_t k_defs[] = {
    [CAL_CHIRP_WIDE] = {300.0, 8000.0, 0.020, 0.350},
    [CAL_CHIRP_LOW] = {30.0, 150.0, 0.200, 0.600},
};

/* Cosine fade at both ends (10 % of the length), so the speaker does not
 * click and the spectrum stays inside the sweep. */
#define CAL_CHIRP_TAPER 0.10

static const chirp_def_t *def(cal_chirp_kind_t kind) {
  return &k_defs[kind == CAL_CHIRP_LOW ? CAL_CHIRP_LOW : CAL_CHIRP_WIDE];
}

double cal_chirp_length_s(cal_chirp_kind_t kind) { return def(kind)->len_s; }

double cal_chirp_period_s(cal_chirp_kind_t kind) { return def(kind)->period_s; }

float cal_chirp_value(cal_chirp_kind_t kind, double t) {
  const chirp_def_t *d = def(kind);
  if (t < 0.0 || t >= d->len_s) return 0.0f;
  const double edge = CAL_CHIRP_TAPER * d->len_s;
  double w = 1.0;
  if (t < edge) {
    w = 0.5 - 0.5 * cos(M_PI * t / edge);
  } else if (t > d->len_s - edge) {
    w = 0.5 - 0.5 * cos(M_PI * (d->len_s - t) / edge);
  }
  const double ph =
      2.0 * M_PI * (d->f0_hz * t + (d->f1_hz - d->f0_hz) * t * t / (2.0 * d->len_s));
  return (float)(w * sin(ph));
}
