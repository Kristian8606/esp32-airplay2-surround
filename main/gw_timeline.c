#include "gw_timeline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "gw_timeline";

/* The estimate is fitted to the best (highest) Sync of each second over the
 * last ten minutes. On the board even the best Syncs of a few seconds scatter
 * by 3 to 6 ms while the Gateway relays audio, so the upper envelope needs
 * timely samples far apart: over ten minutes its rate is good to a few ppm.
 * The crystals drift far less than that in ten minutes. */
#define GW_TIMELINE_BUCKET_NS     1000000000LL
#define GW_TIMELINE_WINDOW_NS     600000000000LL
#define GW_TIMELINE_POINTS        1024U
/* Below this span the rate is not fitted: the remembered one is used and the
 * estimate is the highest sample on it. */
#define GW_TIMELINE_FIT_SPAN_NS   60000000000LL
/* A fitted rate beyond this is not two crystals: keep the remembered one. */
#define GW_TIMELINE_MAX_RATE      0.0005
/* No Wi-Fi delays a Sync this long: a sample this far below the estimate is
 * another clock (the Apple TV restarted) and replaces the older ones at
 * once. Smaller moves of the sender's clock are left to the window: Apple's
 * clock does not step, and rules guessing steps from late Syncs fired on
 * Wi-Fi delay alone (seen on the board). */
#define GW_TIMELINE_FAR_NS        1000000000LL
/* No Sync for this long (the Apple TV restarted or left, the PTP task
 * stopped): the samples before are stale. The fit stops; the line runs on
 * by itself (gw_timeline_hold) and the next Sync starts the fit again. */
#define GW_TIMELINE_GAP_NS        5000000000LL
/* A difference this large is another clock (a restarted sender): step to it
 * instead of crawling for minutes. */
#define GW_TIMELINE_STEP_NS       20000000LL
/* After a step the line follows the estimate directly this long: the first
 * timely Syncs raise it by milliseconds. */
#define GW_TIMELINE_SETTLE_NS     10000000000LL
/* Past settling the line runs on the estimate's rate plus P = difference /
 * GW_TIMELINE_TP_S, at most GW_TIMELINE_MAX_CORR_PPB, and its rate changes
 * at most GW_TIMELINE_SLEW_PPB_S: a speaker sees its reference bend, never
 * kink, and its I2S (160 ppm) can follow. */
#define GW_TIMELINE_TP_S          60.0
#define GW_TIMELINE_MAX_CORR_PPB  100000.0
#define GW_TIMELINE_SLEW_PPB_S    5000LL
/* Without an estimate the line runs on this long (gw_timeline_hold): as long
 * as a speaker rides out a silent link (SURROUND_LINK_TIMEOUT_STREAM_MS). A
 * PTP gap of a few seconds then never reaches the speakers. */
#define GW_TIMELINE_HOLD_NS       20000000000LL

typedef struct {
  int64_t t; /* TSF of the reception */
  int64_t y; /* sender time - t */
} point_t;

/* The upper envelope, as y(t) = y0 + rate * (t - t0). */
typedef struct {
  bool fitted;
  int64_t t0;
  double y0;
  double rate;
  int64_t span_ns;
} estimate_t;

static struct {
  /* The best sample of each finished second, oldest first, in a ring, and
   * of the second being collected. */
  point_t *pts;
  uint32_t head, count;
  bool cur_valid;
  int64_t cur_start_ns;
  point_t cur;
  int64_t last_sample_ns;
  uint64_t sample_domain;
  bool have_domain;
  /* The published line. */
  bool valid;
  uint64_t domain;
  uint16_t epoch;
  int64_t base_tsf_ns, base_ptp_ns;
  int32_t rate_ppb;
  int64_t start_tsf_ns;
  bool holding; /* gw_timeline_hold() since the last estimate */
  /* gw_timeline_take_best(): the highest Sync minus the line since. */
  int64_t best_ns;
  uint32_t best_count;
} s;
/* The last fitted rate, kept across steps: the crystals are the same. */
static double s_rate_memory;

static uint16_t s_hull[GW_TIMELINE_POINTS + 1];

/* The points the estimate uses: the finished seconds, then the current one. */
static uint32_t n_points(void) { return s.count + (s.cur_valid ? 1U : 0U); }

static const point_t *pt(uint32_t i) {
  if (i == s.count) return &s.cur;
  return &s.pts[(s.head + i) % GW_TIMELINE_POINTS];
}

static void drop_oldest(uint32_t n) {
  if (n > s.count) n = s.count;
  s.head = (s.head + n) % GW_TIMELINE_POINTS;
  s.count -= n;
}

static void clear_samples(void) {
  s.head = 0;
  s.count = 0;
  s.cur_valid = false;
}

/* Is c on or above the line through a and b? */
static bool above(const point_t *a, const point_t *b, const point_t *c) {
  return (double)(b->t - a->t) * (double)(c->y - a->y) -
             (double)(b->y - a->y) * (double)(c->t - a->t) >= 0.0;
}

static bool estimate(estimate_t *e) {
  const uint32_t n = n_points();
  if (n == 0) return false;
  const point_t *first = pt(0), *last = pt(n - 1);
  e->span_ns = last->t - first->t;
  e->fitted = false;
  if (e->span_ns >= GW_TIMELINE_FIT_SPAN_NS) {
    /* Upper convex hull (the points are in time order), then its edge over
     * the middle of the window: the lowest line above every sample there,
     * which minimises the mean distance to the samples (the delay is never
     * negative). */
    uint32_t h = 0;
    for (uint32_t i = 0; i < n; ++i) {
      while (h >= 2 && above(pt(s_hull[h - 2]), pt(s_hull[h - 1]), pt(i))) --h;
      s_hull[h++] = (uint16_t)i;
    }
    const int64_t mid = first->t + e->span_ns / 2;
    for (uint32_t k = 0; k + 1 < h; ++k) {
      const point_t *a = pt(s_hull[k]), *b = pt(s_hull[k + 1]);
      if (b->t < mid) continue;
      const double rate = (double)(b->y - a->y) / (double)(b->t - a->t);
      if (fabs(rate) <= GW_TIMELINE_MAX_RATE) {
        e->fitted = true;
        e->t0 = a->t;
        e->y0 = (double)a->y;
        e->rate = rate;
      }
      break;
    }
  }
  if (!e->fitted) {
    /* The highest sample on the remembered rate. */
    e->rate = s_rate_memory;
    e->t0 = last->t;
    e->y0 = -INFINITY;
    for (uint32_t i = 0; i < n; ++i) {
      const double y = (double)pt(i)->y + e->rate * (double)(last->t - pt(i)->t);
      if (y > e->y0) e->y0 = y;
    }
  }
  return true;
}

static double estimate_y(const estimate_t *e, int64_t t) {
  return e->y0 + e->rate * (double)(t - e->t0);
}

void gw_timeline_add_sample(int64_t tsf_ns, int64_t ptp_ns, uint64_t domain) {
  if (!s.pts) {
    s.pts = heap_caps_calloc(GW_TIMELINE_POINTS, sizeof(point_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.pts) s.pts = calloc(GW_TIMELINE_POINTS, sizeof(point_t));
    if (!s.pts) return;
  }
  if (!s.have_domain || domain != s.sample_domain) {
    clear_samples();
    s.sample_domain = domain;
    s.have_domain = true;
  }
  if (n_points() > 0) {
    if (tsf_ns <= s.last_sample_ns) return;
    if (tsf_ns - s.last_sample_ns > GW_TIMELINE_GAP_NS) {
      AP_LOGI(TAG, "first Sync after %.1f s: the fit starts again",
              (double)(tsf_ns - s.last_sample_ns) / 1e9);
      clear_samples();
    }
  }
  const point_t p = {tsf_ns, ptp_ns - tsf_ns};
  if (s.valid) {
    const int64_t dt = tsf_ns - s.base_tsf_ns;
    const int64_t line_ns = s.base_ptp_ns + dt + dt * s.rate_ppb / 1000000000LL;
    const int64_t d = ptp_ns - line_ns;
    if (s.best_count == 0 || d > s.best_ns) s.best_ns = d;
    s.best_count++;
  }

  estimate_t e;
  if (estimate(&e) &&
      (double)p.y < estimate_y(&e, tsf_ns) - (double)GW_TIMELINE_FAR_NS) {
    AP_LOGI(TAG, "a Sync %.3f s below the estimate: another clock, the older "
                 "samples dropped",
            (estimate_y(&e, tsf_ns) - (double)p.y) / 1e9);
    clear_samples();
  }
  s.last_sample_ns = tsf_ns;

  /* The best sample of each second. */
  if (s.cur_valid && tsf_ns - s.cur_start_ns >= GW_TIMELINE_BUCKET_NS) {
    if (s.count == GW_TIMELINE_POINTS) drop_oldest(1);
    s.pts[(s.head + s.count) % GW_TIMELINE_POINTS] = s.cur;
    s.count++;
    s.cur_valid = false;
  }
  if (!s.cur_valid) {
    s.cur_valid = true;
    s.cur_start_ns = tsf_ns;
    s.cur = p;
  } else if (p.y > s.cur.y) {
    s.cur = p;
  }
  uint32_t old = 0;
  while (old < s.count && pt(old)->t < tsf_ns - GW_TIMELINE_WINDOW_NS) ++old;
  drop_oldest(old);
}

static void start_line(int64_t tsf_ns, int64_t est_ns, int32_t rate_ppb,
                       const char *why) {
  const int64_t dt = tsf_ns - s.base_tsf_ns;
  const int64_t old_ptp = s.base_ptp_ns + dt + dt * s.rate_ppb / 1000000000LL;
  const bool was_valid = s.valid;
  s.valid = true;
  s.domain = s.sample_domain;
  s.epoch++;
  s.base_tsf_ns = tsf_ns;
  s.base_ptp_ns = est_ns;
  s.rate_ppb = rate_ppb;
  s.start_tsf_ns = tsf_ns;
  if (was_valid) {
    AP_LOGI(TAG, "new line (epoch %u): %s, step %+.3f ms, rate %+.3f ppm",
            (unsigned)s.epoch, why, (double)(est_ns - old_ptp) / 1e6,
            (double)rate_ppb / 1000.0);
  } else {
    AP_LOGI(TAG, "new line (epoch %u): %s, rate %+.3f ppm", (unsigned)s.epoch,
            why, (double)rate_ppb / 1000.0);
  }
}

bool gw_timeline_update(int64_t tsf_ns, bool tsf_restarted,
                        gw_timeline_out_t *out) {
  if (tsf_restarted) clear_samples();
  /* No recent Sync: no fit (the caller holds the line). */
  if (n_points() == 0 || tsf_ns - s.last_sample_ns > GW_TIMELINE_GAP_NS)
    return false;
  estimate_t e;
  if (!estimate(&e)) return false;
  if (s.holding && s.valid) {
    AP_LOGI(TAG, "sender time back after %.1f s",
            (double)(tsf_ns - s.base_tsf_ns) / 1e9);
  }
  s.holding = false;

  const int64_t est_ns = tsf_ns + llround(estimate_y(&e, tsf_ns));
  const int32_t est_rate_ppb = (int32_t)llround(e.rate * 1e9);
  if (!s.valid || s.domain != s.sample_domain || tsf_ns <= s.base_tsf_ns) {
    start_line(tsf_ns, est_ns, est_rate_ppb,
               !s.valid ? "first sender time"
               : s.domain != s.sample_domain ? "new sender clock"
                                             : "access point TSF restarted");
  }

  /* The line at this instant, on its current rate. */
  const int64_t dt = tsf_ns - s.base_tsf_ns;
  const int64_t line_ns = s.base_ptp_ns + dt + dt * s.rate_ppb / 1000000000LL;
  int64_t err = est_ns - line_ns;
  if (err > GW_TIMELINE_STEP_NS || err < -GW_TIMELINE_STEP_NS) {
    start_line(tsf_ns, est_ns, est_rate_ppb, "sender time moved too far");
    err = 0;
  }

  const bool settling = tsf_ns - s.start_tsf_ns < GW_TIMELINE_SETTLE_NS;
  if (settling) {
    s.base_ptp_ns = est_ns;
    s.rate_ppb = est_rate_ppb;
  } else if (dt > 0) {
    s.base_ptp_ns = line_ns;
    double p = (double)err / GW_TIMELINE_TP_S;
    if (p > GW_TIMELINE_MAX_CORR_PPB) p = GW_TIMELINE_MAX_CORR_PPB;
    if (p < -GW_TIMELINE_MAX_CORR_PPB) p = -GW_TIMELINE_MAX_CORR_PPB;
    const int64_t target = est_rate_ppb + llround(p);
    const int64_t max_change = GW_TIMELINE_SLEW_PPB_S * dt / 1000000000LL + 1;
    int64_t change = target - s.rate_ppb;
    if (change > max_change) change = max_change;
    if (change < -max_change) change = -max_change;
    s.rate_ppb += (int32_t)change;
  }
  s.base_tsf_ns = tsf_ns;
  if (e.fitted) s_rate_memory = e.rate;

  out->ptp_ns = s.base_ptp_ns;
  out->rate_ppb = s.rate_ppb;
  out->err_ns = err;
  out->epoch = s.epoch;
  out->settling = settling;
  out->fitted = e.fitted;
  out->span_ms = (uint32_t)(e.span_ns / 1000000LL);
  return true;
}

bool gw_timeline_take_best(int64_t *best_ns, uint32_t *syncs) {
  const uint32_t n = s.best_count;
  if (best_ns) *best_ns = s.best_ns;
  if (syncs) *syncs = n;
  s.best_count = 0;
  return n > 0;
}

void gw_timeline_invalidate(void) {
  if (s.valid) AP_LOGI(TAG, "line ended: no sender time");
  s.valid = false;
  s.holding = false;
  clear_samples();
}

bool gw_timeline_hold(int64_t tsf_ns, gw_timeline_out_t *out) {
  if (!s.valid) return false;
  /* base_tsf_ns stays at the last estimate's tick while holding. */
  const int64_t dt = tsf_ns - s.base_tsf_ns;
  if (dt < 0 || dt > GW_TIMELINE_HOLD_NS) {
    AP_LOGI(TAG, "no sender time for %.1f s: line ended", (double)dt / 1e9);
    gw_timeline_invalidate();
    return false;
  }
  if (!s.holding) {
    s.holding = true;
    AP_LOGI(TAG, "no sender time: the line runs on (epoch %u, %+.3f ppm) for up "
                 "to %d s",
            (unsigned)s.epoch, (double)s.rate_ppb / 1000.0,
            (int)(GW_TIMELINE_HOLD_NS / 1000000000LL));
  }
  out->ptp_ns = s.base_ptp_ns + dt + dt * s.rate_ppb / 1000000000LL;
  out->rate_ppb = s.rate_ppb;
  out->err_ns = 0;
  out->epoch = s.epoch;
  out->settling = false;
  out->fitted = false;
  out->span_ms = 0;
  return true;
}
