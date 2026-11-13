#include "cal_chirp.h"

#include <math.h>

typedef struct {
  double f0_hz, f1_hz, len_s, period_s;
  bool sweep;
  double fade_in_s, fade_out_s; /* sweeps */
  const char *name;
} chirp_def_t;

/* Chirps: a linear sweep, wide enough for a sharp correlation peak (timing),
 * long enough for energy above the room noise. The subwoofer gets one it can
 * play through its low-pass.
 * Sweeps: exponential (as much energy per octave, -3 dB/octave like pink
 * noise: gentle on a tweeter), long for a high signal-to-noise ratio; the
 * room has died down (at least 60 dB in a living room) before the next. The
 * fade-in covers the first quarter octave; each sweep goes past the top of
 * what it measures (20 kHz, the subwoofer's 300 Hz), so its fade-out stays
 * outside. 21 kHz is under the Nyquist frequency at 44.1 kHz. */
static const chirp_def_t k_defs[CAL_CHIRP_KIND_COUNT] = {
    [CAL_CHIRP_WIDE] = {300.0, 8000.0, 0.020, 0.350, false, 0, 0, "wide"},
    [CAL_CHIRP_LOW] = {30.0, 150.0, 0.200, 0.600, false, 0, 0, "low"},
    [CAL_SWEEP_WIDE] = {30.0, 21000.0, 2.5, 3.25, true, 0.1, 0.01, "sweep"},
    [CAL_SWEEP_LOW] = {15.0, 400.0, 2.5, 3.25, true, 0.2, 0.05, "low sweep"},
};

/* Cosine fade at both ends of a chirp (10 % of the length), so the speaker
 * does not click and the spectrum stays inside the sweep. */
#define CAL_CHIRP_TAPER 0.10

cal_chirp_kind_t cal_chirp_kind(int kind) {
  return kind >= 0 && kind < CAL_CHIRP_KIND_COUNT ? (cal_chirp_kind_t)kind
                                                  : CAL_CHIRP_WIDE;
}

static const chirp_def_t *def(cal_chirp_kind_t kind) {
  return &k_defs[cal_chirp_kind((int)kind)];
}

bool cal_chirp_is_sweep(cal_chirp_kind_t kind) { return def(kind)->sweep; }

bool cal_chirp_is_low(cal_chirp_kind_t kind) {
  kind = cal_chirp_kind((int)kind);
  return kind == CAL_CHIRP_LOW || kind == CAL_SWEEP_LOW;
}

const char *cal_chirp_name(cal_chirp_kind_t kind) { return def(kind)->name; }

double cal_chirp_length_s(cal_chirp_kind_t kind) { return def(kind)->len_s; }

double cal_chirp_period_s(cal_chirp_kind_t kind) { return def(kind)->period_s; }

bool cal_sweep_def(cal_chirp_kind_t kind, cal_sweep_def_t *out) {
  const chirp_def_t *d = def(kind);
  if (!d->sweep || !out) return false;
  out->f0_hz = d->f0_hz;
  out->f1_hz = d->f1_hz;
  out->length_s = d->len_s;
  out->fade_in_s = d->fade_in_s;
  out->fade_out_s = d->fade_out_s;
  return true;
}

static double sweep_fade(const chirp_def_t *d, double t) {
  if (t < d->fade_in_s) return 0.5 - 0.5 * cos(M_PI * t / d->fade_in_s);
  const double left = d->len_s - t;
  if (left < d->fade_out_s) return 0.5 - 0.5 * cos(M_PI * left / d->fade_out_s);
  return 1.0;
}

float cal_chirp_value(cal_chirp_kind_t kind, double t) {
  const chirp_def_t *d = def(kind);
  if (t < 0.0 || t >= d->len_s) return 0.0f;
  if (d->sweep) {
    const double l = d->len_s / log(d->f1_hz / d->f0_hz);
    const double ph = 2.0 * M_PI * d->f0_hz * l * expm1(t / l);
    return (float)(sweep_fade(d, t) * sin(ph));
  }
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

/* Exact phase (double) at the first sample, then within the block the
 * phase from there in single precision: phase(n) = p0 + k * expm1(n * c).
 * Nothing accumulates from sample to sample, and only one double exp per
 * call (the ESP32-S3 has no double-precision FPU). */
void cal_sweep_fill(cal_chirp_kind_t kind, uint32_t start, uint32_t rate,
                    float gain, float *dst, size_t frames, size_t stride) {
  const chirp_def_t *d = def(kind);
  if (!dst || frames == 0U) return;
  if (!d->sweep || rate == 0U) {
    for (size_t i = 0; i < frames; ++i) dst[i * stride] = 0.0f;
    return;
  }
  const double l = d->len_s / log(d->f1_hz / d->f0_hz);
  const uint32_t len = (uint32_t)ceil(d->len_s * rate);
  const double t0 = (double)start / rate;
  const double e0 = exp(t0 / l);
  const double p0 = fmod(2.0 * M_PI * d->f0_hz * l * (e0 - 1.0), 2.0 * M_PI);
  const float k = (float)(2.0 * M_PI * d->f0_hz * l * e0);
  const float c = (float)(1.0 / (l * rate));
  const float pf = (float)p0;
  const float fin = (float)(d->fade_in_s * rate);
  const float fout = (float)(d->fade_out_s * rate);
  const float slen = (float)(d->len_s * rate);
  for (size_t i = 0; i < frames; ++i) {
    const uint32_t n = start + (uint32_t)i;
    float v = 0.0f;
    if (n < len && (float)n < slen) {
      const float ph = pf + k * expm1f((float)i * c);
      v = sinf(ph);
      const float s = (float)n;
      if (s < fin) {
        v *= 0.5f - 0.5f * cosf((float)M_PI * s / fin);
      } else if (slen - s < fout) {
        v *= 0.5f - 0.5f * cosf((float)M_PI * (slen - s) / fout);
      }
    }
    dst[i * stride] = gain * v;
  }
}
