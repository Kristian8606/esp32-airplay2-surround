#include "tsf_fit.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_wifi.h"

/* A reading this far off the line is a bad reading... */
#define TSF_FIT_REJECT_NS 200000.0
/* ...unless this many come in a row: then the TSF itself stepped (the access
 * point restarted, the station roamed) and the fit starts over. */
#define TSF_FIT_REJECT_RUN 8U
#define TSF_READ_TRIES 3
/* A pair read further apart than this was interrupted. */
#define TSF_READ_MAX_SPAN_US 100

void tsf_fit_reset(tsf_fit_t *fit) {
  const uint32_t restarts = fit->restarts;
  memset(fit, 0, sizeof(*fit));
  fit->restarts = restarts;
}

bool tsf_fit_read_sample(int64_t *local_ns, int64_t *tsf_ns) {
  int64_t best_span = INT64_MAX;
  for (int i = 0; i < TSF_READ_TRIES; ++i) {
    const int64_t t0 = esp_timer_get_time();
    const int64_t tsf = esp_wifi_get_tsf_time(WIFI_IF_STA);
    const int64_t t1 = esp_timer_get_time();
    if (tsf <= 0) return false;
    if (t1 - t0 < best_span) {
      best_span = t1 - t0;
      /* The TSF was read somewhere between t0 and t1: take the middle. */
      *local_ns = (t0 + t1) * 500LL;
      *tsf_ns = tsf * 1000LL;
    }
  }
  return best_span <= TSF_READ_MAX_SPAN_US;
}

static void recompute(tsf_fit_t *fit) {
  const uint32_t n = fit->count;
  double sx = 0.0, sy = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    sx += fit->x[i];
    sy += fit->y[i];
  }
  fit->mx = sx / (double)n;
  fit->my = sy / (double)n;
  double sxx = 0.0, sxy = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    const double dx = fit->x[i] - fit->mx;
    sxx += dx * dx;
    sxy += dx * (fit->y[i] - fit->my);
  }
  fit->slope = sxx > 0.0 ? sxy / sxx : 0.0;
  double sr = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    const double r =
        fit->y[i] - (fit->my + fit->slope * (fit->x[i] - fit->mx));
    sr += r * r;
  }
  fit->rms_ns = sqrt(sr / (double)n);
}

static void store(tsf_fit_t *fit, int64_t local_ns, int64_t delta_ns) {
  if (fit->count == 0U) {
    fit->ref_local_ns = local_ns;
    fit->ref_delta_ns = delta_ns;
    fit->start_local_ns = local_ns;
  }
  fit->x[fit->head] = (double)(local_ns - fit->ref_local_ns);
  fit->y[fit->head] = (double)(delta_ns - fit->ref_delta_ns);
  fit->head = (fit->head + 1U) % TSF_FIT_WINDOW;
  if (fit->count < TSF_FIT_WINDOW) fit->count++;
  recompute(fit);
}

tsf_fit_result_t tsf_fit_add(tsf_fit_t *fit, int64_t local_ns, int64_t tsf_ns) {
  const int64_t delta_ns = tsf_ns - local_ns;
  if (tsf_fit_ready(fit)) {
    const double x = (double)(local_ns - fit->ref_local_ns);
    const double y = (double)(delta_ns - fit->ref_delta_ns);
    const double predicted = fit->my + fit->slope * (x - fit->mx);
    if (fabs(y - predicted) > TSF_FIT_REJECT_NS) {
      if (++fit->reject_run < TSF_FIT_REJECT_RUN) return TSF_FIT_REJECTED;
      tsf_fit_reset(fit);
      fit->restarts++;
      store(fit, local_ns, delta_ns);
      return TSF_FIT_RESTARTED;
    }
  }
  fit->reject_run = 0;
  store(fit, local_ns, delta_ns);
  return TSF_FIT_ADDED;
}

bool tsf_fit_ready(const tsf_fit_t *fit) {
  return fit->count >= TSF_FIT_MIN_SAMPLES;
}

uint32_t tsf_fit_age_ms(const tsf_fit_t *fit, int64_t now_local_ns) {
  if (fit->count == 0U || now_local_ns < fit->start_local_ns) return 0;
  const int64_t ms = (now_local_ns - fit->start_local_ns) / 1000000LL;
  return ms > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

bool tsf_fit_tsf_at(const tsf_fit_t *fit, int64_t local_ns, int64_t *tsf_ns) {
  if (!tsf_fit_ready(fit)) return false;
  const double x = (double)(local_ns - fit->ref_local_ns);
  const double y = fit->my + fit->slope * (x - fit->mx);
  *tsf_ns = local_ns + fit->ref_delta_ns + llround(y);
  return true;
}

bool tsf_fit_local_at(const tsf_fit_t *fit, int64_t tsf_ns, int64_t *local_ns) {
  if (!tsf_fit_ready(fit)) return false;
  /* tsf = local + ref_delta + my + slope * (local - ref_local - mx),
   * solved for local. */
  const double k = (double)(tsf_ns - fit->ref_delta_ns - fit->ref_local_ns);
  const double x = (k - fit->my + fit->slope * fit->mx) / (1.0 + fit->slope);
  *local_ns = fit->ref_local_ns + llround(x);
  return true;
}
