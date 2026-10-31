#include "apple_clock_core.h"

#include <math.h>
#include <string.h>

#define NS_PER_S 1000000000LL

/* The numbers below come from the two-way test on the board (2026-10-31,
 * docs/apple-clock-design.md section 2): the shortest round trip to the
 * Apple TV or the iPhone stays 1.4-2.2 ms with or without audio relayed,
 * while the median grows to 12-28 ms and single ones reach 640 ms. */

/* A second counts when its best round trip is within this of the floor. */
#define ACC_MARGIN_NS          1000000LL
/* The floor: the shortest best-of-a-second round trip of the last minute. */
#define ACC_FLOOR_WINDOW_NS    (60LL * NS_PER_S)
/* Before the line is ready there is no floor yet worth the name: a first
 * second counts only below this. */
#define ACC_START_RTT_NS       10000000LL
/* One fast exchange determines phase without waiting for rate learning.
 * Covers the 2.46 ms exchange at the start of the captured 32.5 s wait. */
#define ACC_FAST_START_RTT_NS  3000000LL
#define ACC_READY_POINTS       3U
/* Ready only after this many seconds of exchanges: the floor needs a few. */
#define ACC_START_SECONDS      4U
#define ACC_PRUNE_SPAN_NS      (30LL * NS_PER_S)
/* Off the estimate by more than this: not used; a run of them on one side
 * spanning ACC_STEP_SPAN_NS is a step of Apple's clock. */
#define ACC_FAR_NS             2000000LL
#define ACC_STEP_RUN           5U
#define ACC_STEP_SPAN_NS       (4LL * NS_PER_S)
#define ACC_WINDOW_NS          (600LL * NS_PER_S)
/* Without a valid exchange for this long the line ends (the speakers
 * carry the last line for the same 30 s, PTP_EXTERNAL_CARRY_NS). */
#define ACC_HOLD_NS            (30LL * NS_PER_S)
/* The published line takes a phase difference back over about ACC_TP_S, at
 * most ACC_MAX_CORR_PPB, bending at most ACC_SLEW_PPB_S. */
#define ACC_TP_S               20.0
#define ACC_MAX_CORR_PPB       50000.0
#define ACC_SLEW_PPB_S         20000.0
/* The scatter of the points is never taken as smaller than this: accepted
 * seconds may be up to ACC_MARGIN_NS / 2 off, and three of them in a row
 * lying on a line say nothing about the rate (seen in the host simulation:
 * at 50 us the start took a 190 ppm slope from three points). */
#define ACC_MIN_SIGMA_NS       200000.0
#define ACC_MAX_RTT_NS         NS_PER_S
/* Slope prior after a step or a pause: the rate was just measured. */
#define ACC_KNOWN_SIGMA_PPB    2000.0

static double clamp(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static double est_at(const acc_t *c, int64_t x_ns) {
  return c->est_y0_ns + c->est_rate * (double)(x_ns - c->est_x0_ns) / 1e9;
}

static const acc_point_t *pt(const acc_t *c, uint32_t i) {
  return &c->pts[(c->head + i) % ACC_POINTS];
}

static void clear_points(acc_t *c) {
  c->head = c->count = 0;
  c->far_n = 0;
  c->have_est = false;
  c->samples = 0;
}

void acc_init(acc_t *c, int64_t asym_ns) {
  memset(c, 0, sizeof(*c));
  c->asym_ns = asym_ns;
  c->prior_sigma = 30000.0; /* crystals: tens of ppm */
}

void acc_restart(acc_t *c, int32_t prior_rate_ppb, int32_t prior_sigma_ppb) {
  const int64_t asym = c->asym_ns;
  const uint32_t epoch = c->epoch;
  const acc_stats_t stats = c->stats;
  memset(c, 0, sizeof(*c));
  c->asym_ns = asym;
  c->epoch = epoch;
  c->stats = stats;
  c->prior_rate = prior_rate_ppb;
  c->prior_sigma = prior_sigma_ppb > 0 ? prior_sigma_ppb : 30000.0;
}

/* Least squares through the points, its slope pulled to the prior by how
 * well each is known: a few seconds of points say little about the rate. */
static void refit(acc_t *c) {
  if (c->count == 0) {
    c->have_est = false;
    return;
  }
  const acc_point_t *newest = pt(c, c->count - 1U);
  while (c->count > 0 && pt(c, 0)->x_ns < newest->x_ns - ACC_WINDOW_NS) {
    c->head = (c->head + 1U) % ACC_POINTS;
    c->count--;
  }
  const int64_t x_ref = newest->x_ns, y_ref = newest->y_ns;
  const uint32_t n = c->count;
  double mx = 0, my = 0;
  for (uint32_t i = 0; i < n; ++i) {
    mx += (double)(pt(c, i)->x_ns - x_ref) / 1e9;
    my += (double)(pt(c, i)->y_ns - y_ref);
  }
  mx /= n;
  my /= n;
  double sxx = 0, sxy = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const double dx = (double)(pt(c, i)->x_ns - x_ref) / 1e9 - mx;
    const double dy = (double)(pt(c, i)->y_ns - y_ref) - my;
    sxx += dx * dx;
    sxy += dx * dy;
  }
  const double pw = 1.0 / (c->prior_sigma * c->prior_sigma);
  double rate = c->prior_rate;
  if (n >= 3U && sxx > 0) {
    const double ls = sxy / sxx;
    double ss = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const double dx = (double)(pt(c, i)->x_ns - x_ref) / 1e9 - mx;
      const double r = (double)(pt(c, i)->y_ns - y_ref) - my - ls * dx;
      ss += r * r;
    }
    ss /= (double)(n - 2U);
    if (ss < ACC_MIN_SIGMA_NS * ACC_MIN_SIGMA_NS)
      ss = ACC_MIN_SIGMA_NS * ACC_MIN_SIGMA_NS;
    const double lw = sxx / ss; /* 1 / variance of the slope */
    rate = (c->prior_rate * pw + ls * lw) / (pw + lw);
  }
  c->est_x0_ns = x_ref + (int64_t)llround(mx * 1e9);
  c->est_y0_ns = (double)y_ref + my;
  c->est_rate = rate;
  c->have_est = true;
}

static void push(acc_t *c, const acc_point_t *p) {
  if (c->count == ACC_POINTS) {
    c->head = (c->head + 1U) % ACC_POINTS;
    c->count--;
  }
  c->pts[(c->head + c->count) % ACC_POINTS] = *p;
  c->count++;
}

static void add_point(acc_t *c, const acc_point_t *p) {
  if (c->have_est && c->count >= ACC_READY_POINTS) {
    const double r = (double)p->y_ns - est_at(c, p->x_ns);
    if (fabs(r) > (double)ACC_FAR_NS) {
      const int sign = r > 0 ? 1 : -1;
      if (c->far_n == 0 || sign != c->far_sign) c->far_n = 0;
      c->far_sign = sign;
      if (c->far_n < sizeof(c->far) / sizeof(c->far[0])) c->far[c->far_n++] = *p;
      c->stats.far++;
      if (c->far_n >= ACC_STEP_RUN &&
          c->far[c->far_n - 1U].x_ns - c->far[0].x_ns >= ACC_STEP_SPAN_NS) {
        /* Apple's clock stepped: start again from the far points, on the
         * rate just measured. */
        acc_point_t keep[sizeof(c->far) / sizeof(c->far[0])];
        const uint32_t n = c->far_n;
        memcpy(keep, c->far, n * sizeof(keep[0]));
        c->prior_rate = c->est_rate;
        c->prior_sigma = ACC_KNOWN_SIGMA_PPB;
        clear_points(c);
        for (uint32_t i = 0; i < n; ++i) push(c, &keep[i]);
        c->samples = n; /* the step itself has five good observations */
        refit(c);
        c->pub_started = false;
        c->stats.steps++;
      }
      return;
    }
  }
  c->far_n = 0;
  push(c, p);
  refit(c);
}

/* A lower floor while the estimate is young: exchanges accepted against the
 * old one may be poor ones (the first seconds set the floor themselves);
 * drop them. Later a lower floor only tightens what comes next: dropping
 * minutes of points would leave too few for the rate. */
static void prune_above(acc_t *c, int64_t limit_ns) {
  uint32_t kept = 0;
  for (uint32_t i = 0; i < c->count; ++i) {
    const acc_point_t p = *pt(c, i);
    if (p.rtt_ns > limit_ns) continue;
    c->pts[(c->head + kept) % ACC_POINTS] = p;
    kept++;
  }
  if (kept == c->count) return;
  c->count = kept;
  refit(c);
}

static void close_second(acc_t *c) {
  if (c->bucket_open) {
    const uint32_t k = c->sec_n++ % ACC_FLOOR_SECONDS;
    c->sec_t[k] = c->best.x_ns;
    c->sec_rtt[k] = c->best.rtt_ns;
    int32_t floor_ns = c->best.rtt_ns;
    const uint32_t have =
        c->sec_n < ACC_FLOOR_SECONDS ? c->sec_n : ACC_FLOOR_SECONDS;
    for (uint32_t i = 0; i < have; ++i) {
      if (c->sec_t[i] >= c->best.x_ns - ACC_FLOOR_WINDOW_NS &&
          c->sec_rtt[i] < floor_ns)
        floor_ns = c->sec_rtt[i];
    }
    const bool lower = c->sec_n > 1U && floor_ns < c->floor_ns;
    c->floor_ns = floor_ns;
    if (lower && !c->pub_started && c->count > 0 &&
        c->best.x_ns - pt(c, 0)->x_ns < ACC_PRUNE_SPAN_NS)
      prune_above(c, (int64_t)floor_ns + ACC_MARGIN_NS);
    const bool ok =
        c->best.rtt_ns <= floor_ns + ACC_MARGIN_NS &&
        (c->count >= ACC_READY_POINTS || c->best.rtt_ns <= ACC_START_RTT_NS);
    if (ok) {
      acc_point_t p = c->best;
      p.y_ns += c->asym_ns;
      c->stats.seconds++;
      add_point(c, &p);
    } else {
      c->stats.poor++;
    }
  }
  c->bucket_open = false;
}

/* The second x falls into; a later one closes the current second first. */
static void bucket(acc_t *c, int64_t x_ns) {
  if (c->bucket_open && x_ns >= c->bucket_start_ns + NS_PER_S) close_second(c);
  if (!c->bucket_open) {
    c->bucket_open = true;
    c->bucket_start_ns = x_ns;
    c->best.rtt_ns = INT32_MAX;
  }
}

void acc_add_exchange(acc_t *c, int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
  int64_t down, up, rtt;
  if (__builtin_sub_overflow(t1, t2, &down) ||
      __builtin_sub_overflow(t4, t3, &up) ||
      __builtin_sub_overflow(up, down, &rtt) ||
      rtt < 0 || rtt > ACC_MAX_RTT_NS) {
    c->stats.bad_exchanges++;
    return;
  }
  c->stats.exchanges++;
  c->last_input_ns = t3;
  bucket(c, t3);
  if (rtt < c->best.rtt_ns) {
    c->bucket_open = true;
    c->best = (acc_point_t){t3, down + rtt / 2, (int32_t)rtt};
  }
  if (!c->pub_started &&
      (c->count < ACC_READY_POINTS || c->sec_n < ACC_START_SECONDS) &&
      rtt <= ACC_FAST_START_RTT_NS) {
    /* Phase is already measured by this exchange. Do not postpone audio
     * until several sparse one-second minima also establish the rate.
     * The current bucket enters the fit once, when it closes normally. */
    clear_points(c);
    c->est_x0_ns = c->best.x_ns;
    c->est_y0_ns = (double)c->best.y_ns + c->asym_ns;
    c->est_rate = c->prior_rate;
    c->have_est = true;
    c->samples = 1;
  } else if ((c->floor_ns > 0 && rtt <= c->floor_ns + ACC_MARGIN_NS) ||
             (c->floor_ns == 0 && rtt <= ACC_FAST_START_RTT_NS)) {
    if (c->samples < UINT32_MAX) c->samples++;
  }
}

static int64_t pub_apple(const acc_t *c, int64_t local_ns) {
  const int64_t dt = local_ns - c->pub_base_local_ns;
  return c->pub_base_apple_ns + dt + (int64_t)llround((double)dt * c->pub_rate / 1e9);
}

static void pub_start(acc_t *c, int64_t now_ns) {
  c->pub_started = true;
  c->pub_epoch = ++c->epoch;
  c->pub_base_local_ns = now_ns;
  c->pub_base_apple_ns = now_ns + (int64_t)llround(est_at(c, now_ns));
  c->pub_rate = c->est_rate;
  c->pub_last_ns = now_ns;
  c->pub_ready_since_ns = now_ns;
  c->est_minus_pub_ns = 0;
}

void acc_update(acc_t *c, int64_t now_ns) {
  if (c->bucket_open && now_ns >= c->bucket_start_ns + NS_PER_S) close_second(c);

  const bool fresh = c->have_est && now_ns - c->last_input_ns <= ACC_HOLD_NS;
  const bool qualified = c->pub_started ||
      (c->have_est && c->count == 0) || /* immediate two-way phase seed */
      (c->count > 0 && pt(c, 0)->rtt_ns <= ACC_FAST_START_RTT_NS) ||
      (c->count >= ACC_READY_POINTS && c->sec_n >= ACC_START_SECONDS);
  if (!fresh || !qualified) {
    if (c->have_est && !fresh) {
      /* No valid exchanges: retain the learned rate, remeasure phase on
       * rejoin. The old path minimum need not describe the next session. */
      c->prior_rate = c->est_rate;
      c->prior_sigma = ACC_KNOWN_SIGMA_PPB;
      clear_points(c);
      c->sec_n = 0;
      c->floor_ns = 0;
    }
    c->pub_started = false;
    return;
  }
  if (!c->pub_started) {
    pub_start(c, now_ns);
    return;
  }
  const double dt_s = (double)(now_ns - c->pub_last_ns) / 1e9;
  if (dt_s <= 0) return;
  const int64_t p = pub_apple(c, now_ns);
  const int64_t d = now_ns + (int64_t)llround(est_at(c, now_ns)) - p;
  const double target =
      c->est_rate + clamp((double)d / ACC_TP_S, -ACC_MAX_CORR_PPB, ACC_MAX_CORR_PPB);
  const double max_bend = ACC_SLEW_PPB_S * dt_s;
  c->pub_rate += clamp(target - c->pub_rate, -max_bend, max_bend);
  c->pub_base_local_ns = now_ns;
  c->pub_base_apple_ns = p;
  c->pub_last_ns = now_ns;
  c->est_minus_pub_ns = d;
}

void acc_get_line(const acc_t *c, acc_line_t *out) {
  memset(out, 0, sizeof(*out));
  out->ready = c->pub_started;
  out->epoch = c->pub_epoch;
  out->base_local_ns = c->pub_base_local_ns;
  out->base_apple_ns = c->pub_base_apple_ns;
  out->rate_ppb = (int32_t)lround(c->pub_rate);
  out->ready_since_ns = c->pub_ready_since_ns;
  out->est_rate_ppb = c->have_est ? (int32_t)lround(c->est_rate) : 0;
  out->est_minus_pub_ns = c->est_minus_pub_ns;
  out->floor_rtt_ns = c->floor_ns;
  out->samples = c->samples;
  out->points = c->count;
  out->span_s = c->count
                    ? (uint32_t)((pt(c, c->count - 1U)->x_ns - pt(c, 0)->x_ns) /
                                 NS_PER_S)
                    : 0U;
}

int64_t acc_line_apple_at(const acc_line_t *l, int64_t local_ns) {
  const int64_t dt = local_ns - l->base_local_ns;
  return l->base_apple_ns + dt + dt * (int64_t)l->rate_ppb / 1000000000LL;
}

int64_t acc_line_local_at(const acc_line_t *l, int64_t apple_ns) {
  const double dt = (double)(apple_ns - l->base_apple_ns) /
                    (1.0 + (double)l->rate_ppb / 1e9);
  return l->base_local_ns + (int64_t)llround(dt);
}
