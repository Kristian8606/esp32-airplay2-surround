#pragma once

/* Gateway: the one timeline every speaker plays by.
 *
 * The speakers get a straight line on the access point's TSF,
 * sender = ptp + (tsf - tsf0) * (1 + rate), instead of the sender's time
 * itself. Each speaker evaluates the same line, so a speaker that receives an
 * update later than another still plays the same time.
 *
 * The line is fitted to the raw PTP Syncs, not to a filtered estimate. A Sync
 * is never early: it reads the sender's clock no later than its reception, so
 * every sample lies on or below the true line, and the delay only pushes it
 * further down. While the Gateway relays audio most Syncs arrive 5 to 100 ms
 * late and even the best of a few seconds scatter by 3 to 6 ms (seen on the
 * board); a filter that guessed clock steps from runs of late Syncs sat up
 * to 18 ms low and crawled at 250 us/s. Here:
 *  - the estimate is the upper envelope of the best Sync of each second over
 *    the last ten minutes: the edge of their convex hull over the middle of
 *    the window, the lowest line above all of them, so its rate comes from
 *    timely samples minutes apart and late ones never count;
 *  - a sample more than a second below it, or the first after a 5 s gap,
 *    is another clock (the Apple TV restarted): the fit starts again;
 *  - the published line follows the estimate on its rate, takes a phase
 *    difference back at most GW_TIMELINE_MAX_CORR_PPB and bends at most
 *    GW_TIMELINE_SLEW_PPB_S, so a speaker never sees a step;
 *  - it steps only for a new sender or clock domain, a TSF restart or a
 *    difference beyond GW_TIMELINE_STEP_NS (each starts a new epoch);
 *  - it is ready for a speaker to start on once its samples cover three
 *    seconds (ready_ms): the first few Syncs alone lie milliseconds low.
 * Speakers task only. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  int64_t ptp_ns;   /* the line at the tick's TSF */
  int32_t rate_ppb; /* d(sender)/d(TSF) - 1 */
  int64_t err_ns;   /* the estimate minus the line */
  uint16_t epoch;   /* changes when the line steps */
  bool settling;    /* following the estimate directly after a step */
  bool fitted;      /* the rate is measured, not the remembered one */
  uint32_t span_ms; /* time covered by the samples */
  /* How long the line has been ready to start a speaker on (its samples
   * cover GW_TIMELINE_READY_NS); 0 before. */
  uint32_t ready_ms;
} gw_timeline_out_t;

/* One measured Sync: the sender's clock read `ptp_ns` no later than the TSF
 * instant `tsf_ns`, in the timing domain `domain` (any value that changes
 * with the sender's clock). A new domain drops the older samples. */
void gw_timeline_add_sample(int64_t tsf_ns, int64_t ptp_ns, uint64_t domain);
/* One tick at `tsf_ns`: move the line. False without samples (no line then;
 * the caller holds the last one, see gw_timeline_hold). `tsf_restarted`
 * drops the samples: their TSF belongs to the old count. */
bool gw_timeline_update(int64_t tsf_ns, bool tsf_restarted,
                        gw_timeline_out_t *out);
/* No estimate this tick (a short gap in the sender's PTP): the line runs on
 * by itself, on its last rate, for up to GW_TIMELINE_HOLD_NS since the last
 * estimate, and the next estimate continues it without a step. False when
 * there is no line or the gap is longer: the line has ended then. */
bool gw_timeline_hold(int64_t tsf_ns, gw_timeline_out_t *out);
/* No sender (PTP lost): the samples are dropped, the next one starts a new
 * line. */
void gw_timeline_invalidate(void);
/* The sender's own time against the line: of the Syncs since the last call,
 * the highest one minus the line at its reception, and their count. The
 * best Sync arrives with the least delay, so this is how far the line lies
 * below the Apple TV's clock (plus that delay, ~0.5 ms). False without a
 * line or a Sync. */
bool gw_timeline_take_best(int64_t *best_ns, uint32_t *syncs);
