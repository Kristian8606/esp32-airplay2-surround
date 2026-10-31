/* Pure signal processing for the wired latency measurement (no ESP APIs). */
#include "latency_cal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHIRP_F0_HZ   1500.0
#define CHIRP_F1_HZ   5000.0
#define CHIRP_LEN_S   0.004     /* 4 ms */
#define CHIRP_AMPL    0.25      /* -12 dBFS */
#define MIN_CORR      0.60f
#define MIN_P2P       40        /* ADC counts */
#define MAX_SPREAD_US 200

static double chirp_value(double t) {
  if (t < 0.0 || t >= CHIRP_LEN_S) return 0.0;
  const double w = 0.5 - 0.5 * cos(2.0 * M_PI * t / CHIRP_LEN_S);
  const double ph = 2.0 * M_PI *
      (CHIRP_F0_HZ * t + (CHIRP_F1_HZ - CHIRP_F0_HZ) * t * t / (2.0 * CHIRP_LEN_S));
  return w * sin(ph);
}

void latency_cal_fill_burst(int16_t *stereo, uint32_t frames,
                            uint32_t sample_rate) {
  if (!stereo || !frames || !sample_rate) return;
  for (uint32_t i = 0; i < frames; ++i) {
    const double v = CHIRP_AMPL * 32767.0 * chirp_value((double)i / sample_rate);
    const int16_t s = (int16_t)lrint(v);
    stereo[2 * i] = s;
    stereo[2 * i + 1] = s;
  }
}

static int cmp_i32(const void *a, const void *b) {
  const int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
  return (x > y) - (x < y);
}
static int cmp_f(const void *a, const void *b) {
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

void latency_cal_analyze(const uint16_t *samples, uint32_t n,
                         const uint32_t *cb_count, const int64_t *cb_t,
                         uint32_t cb_n, const int64_t *emit_us, int n_emit,
                         latency_cal_result_t *res) {
  memset(res, 0, sizeof(*res));
  for (int k = 0; k < LATENCY_CAL_BURSTS; ++k) res->burst_us[k] = INT32_MIN;
  if (!samples || n < 1000 || !cb_count || !cb_t || cb_n < 10 || !emit_us ||
      n_emit <= 0) {
    res->error = "no ADC data captured";
    return;
  }
  if (n_emit > LATENCY_CAL_BURSTS) n_emit = LATENCY_CAL_BURSTS;

  /* 1. ADC sample index -> time: least-squares line through the DMA frame
   *    completion stamps (averages out interrupt jitter). Sample i completes
   *    at conversion count i+1. */
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  const double c0 = (double)cb_count[0];
  const double t0 = (double)cb_t[0];
  for (uint32_t k = 0; k < cb_n; ++k) {
    const double x = (double)cb_count[k] - c0, y = (double)cb_t[k] - t0;
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  const double den = (double)cb_n * sxx - sx * sx;
  if (den <= 0.0) { res->error = "ADC timing invalid"; return; }
  const double b = ((double)cb_n * sxy - sx * sy) / den;   /* us per sample */
  double a = t0 + (sy - b * sx) / (double)cb_n - b * c0;
  /* Interrupt latency only ever makes a stamp LATE, so the least-squares line
   * sits above the true timing by the mean latency. Keep the slope, but move
   * the line down to the earliest stamp (lower envelope). */
  double min_res = 0;
  for (uint32_t k = 0; k < cb_n; ++k) {
    const double r = (double)cb_t[k] - (a + b * (double)cb_count[k]);
    if (k == 0 || r < min_res) min_res = r;
  }
  a += min_res;
  const double nominal = 1e6 / (double)LATENCY_CAL_ADC_RATE_HZ;
  if (b < nominal * 0.97 || b > nominal * 1.03) {
    res->error = "ADC sample rate out of range";
    return;
  }
  /* time(i) = a + b * (i + 1) */

  /* 2. template at the ADC rate */
  const int tpl_n = (int)(CHIRP_LEN_S * 1e6 / b) + 1;
  float *tpl = malloc(sizeof(float) * (size_t)tpl_n);
  if (!tpl) { res->error = "out of memory"; return; }
  double et = 0;
  for (int j = 0; j < tpl_n; ++j) {
    tpl[j] = (float)chirp_value((double)j * b * 1e-6);
    et += (double)tpl[j] * tpl[j];
  }

  const int max_lags = (int)((LATENCY_CAL_MAX_LATENCY_US + 1000) / b) + 2;
  float *x = malloc(sizeof(float) * (size_t)(max_lags + tpl_n + 2));
  if (!x) { free(tpl); res->error = "out of memory"; return; }

  int32_t lat[LATENCY_CAL_BURSTS];
  float corr[LATENCY_CAL_BURSTS];
  int nv = 0, inv_votes = 0, best_p2p = 0;
  for (int k = 0; k < n_emit; ++k) {
    /* window: 1 ms before emission .. MAX_LATENCY after, plus the chirp */
    const double t_from = (double)emit_us[k] - 1000.0;
    long i0 = (long)floor((t_from - a) / b) - 1;
    if (i0 < 0) continue;
    const long lags = max_lags;
    if ((uint64_t)i0 + (uint64_t)lags + (uint64_t)tpl_n + 1 >= n) continue;

    double mean = 0;
    for (long i = 0; i < lags + tpl_n; ++i) mean += samples[i0 + i];
    mean /= (double)(lags + tpl_n);
    int mn = 65535, mx = 0;
    for (long i = 0; i < lags + tpl_n; ++i) {
      x[i] = (float)(samples[i0 + i] - mean);
      if (samples[i0 + i] < mn) mn = samples[i0 + i];
      if (samples[i0 + i] > mx) mx = samples[i0 + i];
    }

    /* normalised cross-correlation with a sliding energy */
    double ex = 0;
    for (int j = 0; j < tpl_n; ++j) ex += (double)x[j] * x[j];
    double best_r = 0, best_c = 0;
    long best_l = -1;
    for (long l = 0; l < lags; ++l) {
      if (l > 0) {
        ex += (double)x[l + tpl_n - 1] * x[l + tpl_n - 1] -
              (double)x[l - 1] * x[l - 1];
        if (ex < 0) ex = 0;
      }
      /* float MAC: the S3 FPU is single precision (double is software). */
      float c = 0.0f;
      const float *xs = &x[l];
      for (int j = 0; j < tpl_n; ++j) c += xs[j] * tpl[j];
      const double r = (ex > 0) ? c / sqrt(et * ex) : 0.0;
      if (fabs(r) > fabs(best_r)) { best_r = r; best_c = c; best_l = l; }
    }
    if (best_l <= 0 || best_l >= lags - 1) continue;
    if (fabs(best_r) < MIN_CORR || (mx - mn) < MIN_P2P) continue;

    /* sub-sample peak (parabola through the raw correlation, sign-aware) */
    float cm = 0.0f, cp = 0.0f;
    for (int j = 0; j < tpl_n; ++j) {
      cm += x[best_l - 1 + j] * tpl[j];
      cp += x[best_l + 1 + j] * tpl[j];
    }
    const double s = best_c < 0 ? -1.0 : 1.0;
    const double y0 = s * best_c, ym = s * cm, yp = s * cp;
    const double d = ym - 2.0 * y0 + yp;
    double frac = (d < 0) ? 0.5 * (ym - yp) / d : 0.0;
    if (frac > 0.5) frac = 0.5;
    if (frac < -0.5) frac = -0.5;

    const double arrival = a + b * ((double)(i0 + best_l) + frac + 1.0);
    lat[nv] = (int32_t)lrint(arrival - (double)emit_us[k]);
    corr[nv] = (float)fabs(best_r);
    res->burst_us[k] = lat[nv];
    if (best_r < 0) inv_votes++;
    if (mx - mn > best_p2p) best_p2p = mx - mn;
    nv++;
  }
  free(x);
  free(tpl);

  res->valid_bursts = (uint8_t)nv;
  res->amplitude = best_p2p;
  if (nv < (n_emit + 1) / 2 + 1) {
    res->error = nv == 0 ? "no test signal found on the ADC pin (check wire/pin/level)"
                         : "too few clean bursts (noise or clipping)";
    return;
  }
  int32_t sorted[LATENCY_CAL_BURSTS];
  memcpy(sorted, lat, sizeof(int32_t) * (size_t)nv);
  qsort(sorted, (size_t)nv, sizeof(int32_t), cmp_i32);
  qsort(corr, (size_t)nv, sizeof(float), cmp_f);
  res->latency_us = (nv & 1) ? sorted[nv / 2]
                             : (sorted[nv / 2 - 1] + sorted[nv / 2]) / 2;
  res->spread_us = sorted[nv - 1] - sorted[0];
  res->corr = corr[nv / 2];
  res->inverted = inv_votes * 2 > nv;
  if (res->spread_us > MAX_SPREAD_US) {
    res->error = "bursts disagree (unstable signal)";
    return;
  }
  res->ok = true;
}
