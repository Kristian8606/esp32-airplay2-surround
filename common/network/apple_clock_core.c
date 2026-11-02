#include "apple_clock_core.h"

#include <math.h>
#include <string.h>

#define NS_PER_S 1000000000LL

/* The numbers below come from the board (2026-10-31 to 11-02): the shortest
 * round trip to an Apple TV or iPhone stays 1.4-2.2 ms with or without audio
 * relayed to three or four speakers, while the median grows to 12-28 ms and
 * single ones reach 640 ms; in the first ~30 s of a stream the best of each
 * second can stay near 4.6 ms. */

/* Error of a second at the floor (the shortest round trip among the seconds
 * in the fit); one above it may be off by up to half the excess, so its
 * weight is 1 / (sigma0^2 + (excess / 2)^2). The floor is taken from the
 * fit, not from a recent window: a 1.5 ms second stays a good measurement
 * for as long as it is in the fit. With a 60 s floor a pause before a
 * stream let the good seconds age out, the stream's first 4.6 ms seconds
 * became "the floor" with full weight and the line swung by 1.8 ms. */
#define ACC_SIGMA0_NS          150000.0
/* Above the floor by more than this: not worth a weight. */
#define ACC_POOR_NS            40000000LL
/* Close enough to the floor to count for or against a step of Apple's
 * clock. */
#define ACC_GOOD_NS            4000000LL
/* An exchange this close to the floor is a quality-checked sample (TIME's
 * sample_count). */
#define ACC_SAMPLE_MARGIN_NS   1000000LL
/* The line is ready whenever the fit holds a second this fast: its phase is
 * then known to half of it. One rule for a cold boot, a stream after a
 * pause, a new master and a step alike; everything after is the same
 * weighted fit followed calmly. Not lower: a stream's first ~30 s can keep
 * the best of each second near 4.6 ms (board, 11-02), and in a host
 * simulation of a cold boot straight into that a 4 ms limit left no time,
 * so no sound, for 30-38 s; with 10 ms the line came after 1-2 s, its start
 * off by 0.1-0.6 ms (once 2.2 ms), the same for every speaker and taken
 * back by the calm correction. */
#define ACC_READY_RTT_NS       10000000LL
/* The estimate needs this many seconds before a second can be judged far
 * from it. */
#define ACC_FAR_MIN_POINTS     3U
/* Off the estimate by more than this plus half the excess (or than half the
 * second's round trip plus half the floor, if larger): not used; a run of
 * good ones on one side spanning ACC_STEP_SPAN_NS is a step. */
#define ACC_FAR_NS             2000000.0
#define ACC_STEP_RUN           5U
#define ACC_STEP_SPAN_NS       (4LL * NS_PER_S)
#define ACC_WINDOW_NS          (600LL * NS_PER_S)
/* Without a valid exchange for this long the line ends (the speakers carry
 * the last line for the same 30 s, PTP_EXTERNAL_CARRY_NS). */
#define ACC_HOLD_NS            (30LL * NS_PER_S)
/* The published line takes a phase difference back over about ACC_TP_S, at
 * most ACC_MAX_CORR_PPB, bending at most ACC_SLEW_PPB_S. A difference beyond
 * ACC_JUMP_NS is not taken back slowly (that would take minutes): the line
 * starts again from the estimate, as a new epoch. */
#define ACC_TP_S               20.0
#define ACC_MAX_CORR_PPB       50000.0
#define ACC_SLEW_PPB_S         20000.0
#define ACC_JUMP_NS            10000000LL
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

static double excess(const acc_t *c, const acc_point_t *p) {
  const double e = (double)p->rtt_ns - (double)c->floor_ns;
  return e > 0 ? e : 0;
}

static double weight(const acc_t *c, const acc_point_t *p) {
  const double half = excess(c, p) / 2.0;
  return 1.0 / (ACC_SIGMA0_NS * ACC_SIGMA0_NS + half * half);
}

static void clear_points(acc_t *c) {
  c->head = c->count = 0;
  c->far_n = 0;
  c->have_est = false;
  c->floor_ns = 0;
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

/* The shortest round trip among the seconds in the fit. */
static void update_floor(acc_t *c) {
  int32_t f = 0;
  for (uint32_t i = 0; i < c->count; ++i)
    if (f == 0 || pt(c, i)->rtt_ns < f) f = pt(c, i)->rtt_ns;
  c->floor_ns = f;
}

/* Weighted least squares through the seconds, the slope pulled to the prior
 * by how well each is known (a few seconds say little about the rate). */
static void refit(acc_t *c) {
  if (c->count == 0) {
    c->have_est = false;
    c->floor_ns = 0;
    return;
  }
  const acc_point_t *newest = pt(c, c->count - 1U);
  while (c->count > 0 && pt(c, 0)->x_ns < newest->x_ns - ACC_WINDOW_NS) {
    c->head = (c->head + 1U) % ACC_POINTS;
    c->count--;
  }
  update_floor(c);
  const int64_t x_ref = newest->x_ns, y_ref = newest->y_ns;
  const uint32_t n = c->count;
  double sw = 0, mx = 0, my = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const double w = weight(c, pt(c, i));
    sw += w;
    mx += w * (double)(pt(c, i)->x_ns - x_ref) / 1e9;
    my += w * (double)(pt(c, i)->y_ns - y_ref);
  }
  mx /= sw;
  my /= sw;
  double sxx = 0, sxy = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const double w = weight(c, pt(c, i));
    const double dx = (double)(pt(c, i)->x_ns - x_ref) / 1e9 - mx;
    const double dy = (double)(pt(c, i)->y_ns - y_ref) - my;
    sxx += w * dx * dx;
    sxy += w * dx * dy;
  }
  const double pw = 1.0 / (c->prior_sigma * c->prior_sigma);
  double rate = c->prior_rate;
  if (n >= 3U && sxx > 0) {
    const double ls = sxy / sxx;
    /* Scatter beyond what the weights expect widens the slope's error. */
    double chi2 = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const double dx = (double)(pt(c, i)->x_ns - x_ref) / 1e9 - mx;
      const double r = (double)(pt(c, i)->y_ns - y_ref) - my - ls * dx;
      chi2 += weight(c, pt(c, i)) * r * r;
    }
    chi2 /= (double)(n - 2U);
    const double lw = sxx / (chi2 > 1.0 ? chi2 : 1.0); /* 1 / var(slope) */
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

/* Called from acc_update only, so the fit never runs between receiving a
 * packet and time-stamping the next one. */
static void close_second(acc_t *c) {
  c->bucket_open = false;
  acc_point_t p = c->best;
  p.y_ns += c->asym_ns;
  /* Judged against the floor including this second. */
  const int32_t floor_ns =
      c->count == 0 || p.rtt_ns < c->floor_ns ? p.rtt_ns : c->floor_ns;
  const double ex = p.rtt_ns > floor_ns ? (double)(p.rtt_ns - floor_ns) : 0;
  if (ex > (double)ACC_POOR_NS) {
    c->stats.poor++;
    return;
  }
  c->stats.seconds++;
  const bool good = ex <= (double)ACC_GOOD_NS;
  if (c->have_est && c->count >= ACC_FAR_MIN_POINTS) {
    /* The second may be off by half its round trip and the fit by half its
     * floor: a difference within their sum is not a step. Below a 2 ms
     * floor ACC_FAR_NS + ex / 2 is the larger. A HomePod master's first
     * seconds were 12.6 ms; the 2.7 ms ones that followed were taken for a
     * step, and the line waited 13.7 s instead of starting with the first
     * of them (v4.5.42; host simulation: always with the first, 5.7 s). */
    const double bound = ((double)p.rtt_ns + (double)c->floor_ns) / 2.0;
    const double far = ACC_FAR_NS + ex / 2.0;
    const double r = (double)p.y_ns - est_at(c, p.x_ns);
    if (fabs(r) > (bound > far ? bound : far)) {
      c->stats.far++;
      if (!good) return;
      const int sign = r > 0 ? 1 : -1;
      if (c->far_n == 0 || sign != c->far_sign) c->far_n = 0;
      c->far_sign = sign;
      if (c->far_n < ACC_FAR_RUN_MAX) c->far[c->far_n++] = p;
      if (c->far_n >= ACC_STEP_RUN &&
          c->far[c->far_n - 1U].x_ns - c->far[0].x_ns >= ACC_STEP_SPAN_NS) {
        /* Apple's clock stepped: start again from the far seconds, on the
         * rate just measured. */
        acc_point_t keep[ACC_FAR_RUN_MAX];
        const uint32_t n = c->far_n;
        memcpy(keep, c->far, n * sizeof(keep[0]));
        c->prior_rate = c->est_rate;
        c->prior_sigma = ACC_KNOWN_SIGMA_PPB;
        clear_points(c);
        for (uint32_t i = 0; i < n; ++i) push(c, &keep[i]);
        c->samples = n;
        refit(c);
        c->pub_started = false;
        c->stats.steps++;
      }
      return;
    }
  }
  /* Only a good second breaks a run of far ones. */
  if (good) c->far_n = 0;
  push(c, &p);
  refit(c);
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
  const acc_point_t p = {t3, down + rtt / 2, (int32_t)rtt};
  if (!c->bucket_open) {
    c->bucket_open = true;
    c->bucket_start_ns = t3;
    c->best = p;
  } else if (p.rtt_ns < c->best.rtt_ns) {
    c->best = p;
  }
  const int64_t limit =
      c->floor_ns > 0 ? c->floor_ns + ACC_SAMPLE_MARGIN_NS : ACC_READY_RTT_NS;
  if (rtt <= limit && c->samples < UINT32_MAX) c->samples++;
}

static int64_t pub_apple(const acc_t *c, int64_t local_ns) {
  const int64_t dt = local_ns - c->pub_base_local_ns;
  return c->pub_base_apple_ns + dt +
         (int64_t)llround((double)dt * c->pub_rate / 1e9);
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

  if (c->have_est && now_ns - c->last_input_ns > ACC_HOLD_NS) {
    /* No valid exchanges: keep the learned rate, measure the phase again
     * on rejoin. */
    c->prior_rate = c->est_rate;
    c->prior_sigma = ACC_KNOWN_SIGMA_PPB;
    clear_points(c);
  }
  if (!c->have_est || c->floor_ns > ACC_READY_RTT_NS) {
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
  if (d > ACC_JUMP_NS || d < -ACC_JUMP_NS) {
    c->stats.jumps++;
    pub_start(c, now_ns);
    return;
  }
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
