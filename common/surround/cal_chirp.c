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
    [CAL_NOISE_PINK] = {0.0, 0.0, CAL_NOISE_S, CAL_NOISE_S, false, 0, 0, "pink noise"},
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
  out->period_s = d->period_s;
  return true;
}

bool cal_sweep_custom(uint32_t length_ms, uint32_t lo_hz, uint32_t hi_hz,
                      cal_sweep_def_t *out) {
  if (!out || length_ms < CAL_SWEEP_MIN_MS || length_ms > CAL_SWEEP_MAX_MS ||
      lo_hz < 10U || hi_hz > 20000U || hi_hz < 2U * lo_hz)
    return false;
  double f0 = 0.5 * (double)lo_hz, f1 = 2.0 * (double)hi_hz;
  if (f0 < 10.0) f0 = 10.0;
  if (f1 > 21000.0) f1 = 21000.0;
  const double len = (double)length_ms / 1000.0;
  /* Exponential: the time an octave takes is the same everywhere. */
  const double per_octave = len * log(2.0) / log(f1 / f0);
  out->f0_hz = f0;
  out->f1_hz = f1;
  out->length_s = len;
  out->fade_in_s = per_octave / 4.0;
  out->fade_out_s = per_octave / 8.0;
  out->period_s = len + CAL_SWEEP_TAIL_S;
  return true;
}

float cal_pink_next(cal_pink_t *p) {
  /* White noise from a 32-bit LCG, uniform in [-1, 1). */
  p->seed = p->seed * 1664525U + 1013904223U;
  const float w = (float)(int32_t)p->seed * (1.0f / 2147483648.0f);
  /* Paul Kellet's pink filter (refined), within 0.05 dB of -3 dB/octave
   * from 10 Hz up. With this input its RMS is 1.77. */
  float *b = p->b;
  b[0] = 0.99886f * b[0] + w * 0.0555179f;
  b[1] = 0.99332f * b[1] + w * 0.0750759f;
  b[2] = 0.96900f * b[2] + w * 0.1538520f;
  b[3] = 0.86650f * b[3] + w * 0.3104856f;
  b[4] = 0.55000f * b[4] + w * 0.5329522f;
  b[5] = -0.7616f * b[5] - w * 0.0168980f;
  float v = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
  b[6] = w * 0.115926f;
  v *= 1.0f / 1.771f; /* unit RMS */
  if (v > 4.0f) v = 4.0f;
  if (v < -4.0f) v = -4.0f;
  return v;
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
  cal_sweep_def_t d;
  if (!cal_sweep_def(kind, &d)) {
    for (size_t i = 0; dst && i < frames; ++i) dst[i * stride] = 0.0f;
    return;
  }
  cal_sweep_fill_def(&d, start, rate, gain, dst, frames, stride);
}

void cal_sweep_fill_def(const cal_sweep_def_t *def, uint32_t start, uint32_t rate,
                        float gain, float *dst, size_t frames, size_t stride) {
  if (!dst || frames == 0U) return;
  if (!def || rate == 0U) {
    for (size_t i = 0; i < frames; ++i) dst[i * stride] = 0.0f;
    return;
  }
  const cal_sweep_def_t *d = def;
  const double l = d->length_s / log(d->f1_hz / d->f0_hz);
  const uint32_t len = (uint32_t)ceil(d->length_s * rate);
  const double t0 = (double)start / rate;
  const double e0 = exp(t0 / l);
  const double p0 = fmod(2.0 * M_PI * d->f0_hz * l * (e0 - 1.0), 2.0 * M_PI);
  const float k = (float)(2.0 * M_PI * d->f0_hz * l * e0);
  const float c = (float)(1.0 / (l * rate));
  const float pf = (float)p0;
  const float fin = (float)(d->fade_in_s * rate);
  const float fout = (float)(d->fade_out_s * rate);
  const float slen = (float)(d->length_s * rate);
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
