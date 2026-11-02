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
 * The same three rules hold at a cold boot, a stream after a pause, a new
 * master or a step:
 *  - each second keeps its shortest exchange and enters a ten-minute
 *    weighted least-squares fit, weighted by how far its round trip is
 *    above the shortest in the fit (the floor); the slope starts from a
 *    remembered prior;
 *  - the line is ready whenever the fit holds a second with a round trip
 *    of 10 ms or less;
 *  - the published line follows the estimate with bounded, smooth phase
 *    correction, and starts again as a new epoch if they differ by more
 *    than 10 ms.
 * Five good seconds consistently far from the estimate are a step of
 * Apple's clock: the fit starts again from them. After 30 seconds without
 * a valid exchange the fit is cleared. Seconds are closed and fitted in
 * acc_update only. Single-threaded: one caller. */

#include <stdbool.h>
#include <stdint.h>

#define ACC_POINTS 640U /* seconds: ten minutes and some */
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
  int32_t floor_rtt_ns;   /* shortest round trip among the seconds in the fit */
  uint32_t samples; /* exchanges within 1 ms of the floor, this acquisition */
  uint32_t points;
  uint32_t span_s;
} acc_line_t;

typedef struct {
  uint32_t exchanges;
  uint32_t bad_exchanges; /* round trip negative or over a second */
  uint32_t seconds;       /* seconds that gave a point */
  uint32_t poor;          /* seconds over 40 ms above the floor, not used */
  uint32_t far;           /* seconds far off the line, not used */
  uint32_t steps;         /* steps of Apple's clock */
  uint32_t jumps;         /* the line restarted 10 ms off the estimate */
} acc_stats_t;

typedef struct {
  int64_t asym_ns; /* added to every offset (calibration, 0 by default) */

  /* The current second. */
  bool bucket_open;
  int64_t bucket_start_ns;
  acc_point_t best;

  /* The seconds, oldest first, in a ring, and their shortest round trip. */
  acc_point_t pts[ACC_POINTS];
  uint32_t head, count;
  int32_t floor_ns;
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

/* Close a finished second (and fit it) and move the published line to now:
 * call it after each batch of received packets. */
void acc_update(acc_t *c, int64_t now_ns);

void acc_get_line(const acc_t *c, acc_line_t *out);
