#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Relation between this ESP's clock (esp_timer) and the Wi-Fi TSF of the
 * access point it is connected to, both in ns.
 *
 * Every station of one access point keeps its TSF in step with the AP's
 * beacons, so the TSF is a clock the Gateway and the speakers share. A single
 * reading jitters by a few microseconds (register read, beacon corrections),
 * but the relation itself is a straight line (two crystals, ppm apart), so a
 * least-squares line over the last samples removes the jitter without lag. */

#define TSF_FIT_WINDOW 64U /* 8 s at SURROUND_TIME_INTERVAL_MS */
#define TSF_FIT_MIN_SAMPLES 8U

typedef enum {
  TSF_FIT_ADDED,
  TSF_FIT_REJECTED,  /* far off the line: a single bad reading */
  TSF_FIT_RESTARTED, /* a run of them: the TSF stepped, the fit starts over */
} tsf_fit_result_t;

typedef struct {
  /* Samples relative to the first one, so doubles keep sub-ns precision. */
  int64_t ref_local_ns;
  int64_t ref_delta_ns; /* tsf - local of the first sample */
  double x[TSF_FIT_WINDOW]; /* local - ref_local */
  double y[TSF_FIT_WINDOW]; /* (tsf - local) - ref_delta */
  uint32_t count;
  uint32_t head;
  uint32_t reject_run;
  int64_t start_local_ns;
  uint32_t restarts;
  /* The line y = my + slope * (x - mx), and its residual RMS. */
  double mx, my, slope, rms_ns;
} tsf_fit_t;

void tsf_fit_reset(tsf_fit_t *fit);

/* One reading of (esp_timer, TSF) taken as close together as possible.
 * False while the station is not connected (TSF 0). */
bool tsf_fit_read_sample(int64_t *local_ns, int64_t *tsf_ns);

tsf_fit_result_t tsf_fit_add(tsf_fit_t *fit, int64_t local_ns, int64_t tsf_ns);

/* Enough samples for a line. */
bool tsf_fit_ready(const tsf_fit_t *fit);
/* Age of the current fit (since its first sample), ms. */
uint32_t tsf_fit_age_ms(const tsf_fit_t *fit, int64_t now_local_ns);

bool tsf_fit_tsf_at(const tsf_fit_t *fit, int64_t local_ns, int64_t *tsf_ns);
bool tsf_fit_local_at(const tsf_fit_t *fit, int64_t tsf_ns, int64_t *local_ns);
