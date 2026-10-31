#pragma once

/* The sender's (Apple's) clock from two-way PTP exchanges: pure math, no
 * ESP-IDF (docs/apple-clock-design.md, section 4).
 *
 * One exchange is a Sync (t1 Apple sent, t2 we received) and the Delay_Req
 * sent right after it (t3 we sent, t4 Apple received), t2 and t3 in our
 * clock (esp_timer ns), t1 and t4 in Apple's (PTP ns):
 *   round trip rtt   = (t4 - t3) - (t1 - t2)
 *   offset     theta = Apple - local = ((t1 - t2) + (t4 - t3)) / 2,
 * wrong only by half the difference of the two directions, which is small
 * when the round trip is close to the shortest one.
 *
 * A first exchange with RTT <= 3 ms seeds the published phase immediately,
 * on the prior rate; the same line then learns from filtered seconds.
 * Each second keeps its shortest exchange; only seconds within 1 ms of
 * the last minute's RTT floor enter a ten-minute least-squares fit. Its
 * slope starts from a remembered prior. The published line follows with
 * bounded, smooth phase correction. Five good seconds consistently far
 * from the estimate establish a clock step and a new epoch. Valid but
 * poor-RTT exchanges keep the line alive without changing its estimate;
 * after 30 seconds without any valid exchange it expires.
 * Single-threaded: one caller. */

#include <stdbool.h>
#include <stdint.h>

#define ACC_POINTS 640U /* seconds: ten minutes and some */
#define ACC_FLOOR_SECONDS 64U
#define ACC_FAR_RUN_MAX 8U

typedef struct {
  int64_t x_ns;   /* local time of the request (t3) */
  int64_t y_ns;   /* Apple - local */
  int32_t rtt_ns;
} acc_point_t;

typedef struct {
  /* Apple = base_apple + (local - base_local) * (1 + rate). */
  bool ready;
  uint32_t epoch;
  int64_t base_local_ns, base_apple_ns;
  int32_t rate_ppb;
  int64_t ready_since_ns; /* local time this epoch started */
  /* Diagnostics. */
  int32_t est_rate_ppb;
  int64_t est_minus_pub_ns; /* the estimate minus the published line */
  int32_t floor_rtt_ns;
  uint32_t samples; /* quality-checked exchanges in this acquisition */
  uint32_t points;
  uint32_t span_s;
} acc_line_t;

typedef struct {
  uint32_t exchanges;
  uint32_t bad_exchanges; /* round trip negative or over a second */
  uint32_t seconds;       /* seconds that gave a point */
  uint32_t poor;          /* seconds too far above the floor to use */
  uint32_t far;           /* seconds far off the line, not used */
  uint32_t steps;
} acc_stats_t;

typedef struct {
  int64_t asym_ns; /* added to every offset (calibration, 0 by default) */

  /* The current second. */
  bool bucket_open;
  int64_t bucket_start_ns;
  acc_point_t best;

  /* Best round trip of each recent second, for the floor. */
  int64_t sec_t[ACC_FLOOR_SECONDS];
  int32_t sec_rtt[ACC_FLOOR_SECONDS];
  uint32_t sec_n;
  int32_t floor_ns;

  /* The seconds, oldest first, in a ring. */
  acc_point_t pts[ACC_POINTS];
  uint32_t head, count;
  int64_t last_input_ns;
  uint32_t samples;

  /* Good seconds far off the estimate, all on one side. */
  acc_point_t far[ACC_FAR_RUN_MAX];
  uint32_t far_n;
  int far_sign;

  /* The estimate y(x) = est_y0 + est_rate * (x - est_x0). */
  bool have_est;
  int64_t est_x0_ns;
  double est_y0_ns;
  double est_rate; /* ns per s = ppb */
  double prior_rate, prior_sigma;

  /* The published line. */
  bool pub_started;
  uint32_t epoch, pub_epoch;
  int64_t pub_base_local_ns, pub_base_apple_ns, pub_last_ns;
  double pub_rate; /* ppb */
  int64_t pub_ready_since_ns;
  int64_t est_minus_pub_ns;

  acc_stats_t stats;
} acc_t;

void acc_init(acc_t *c, int64_t asym_ns);

/* Start over (a new sender or clock): no points, the line not ready. The
 * slope starts at prior_rate_ppb, trusted to prior_sigma_ppb. */
void acc_restart(acc_t *c, int32_t prior_rate_ppb, int32_t prior_sigma_ppb);

void acc_add_exchange(acc_t *c, int64_t t1, int64_t t2, int64_t t3, int64_t t4);

/* Close a finished second and move the published line to now. */
void acc_update(acc_t *c, int64_t now_ns);

void acc_get_line(const acc_t *c, acc_line_t *out);

/* Apple's time on a line at a local instant, and the local instant of an
 * Apple time. */
int64_t acc_line_apple_at(const acc_line_t *l, int64_t local_ns);
int64_t acc_line_local_at(const acc_line_t *l, int64_t apple_ns);
