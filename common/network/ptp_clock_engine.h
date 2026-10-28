#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A new mastership (first sample, GM change, restart after a gap) may drive
 * audio after this much continuous history (Shairport/NQPTP: 400 ms). Until
 * then the published offset follows the filter directly; afterwards it is
 * slew-limited so the audio timeline never sees a step. */
#define PTP_CLOCK_ENGINE_READY_MS 400U

typedef struct {
  bool source_changed;
  bool gm_changed;
  bool first_domain;
  uint64_t old_source_clock_id;
  uint64_t old_grandmaster_clock_id;
  uint64_t new_source_clock_id;
  uint64_t new_grandmaster_clock_id;
  bool old_estimator_valid;
  int64_t old_raw_offset_ns;
  int64_t old_filtered_offset_ns;
  uint32_t old_accepted_samples;
  uint32_t epoch;
} ptp_clock_engine_domain_event_t;

typedef struct {
  bool accepted;
  bool restarted;  /* sample gap too long: a new mastership starts here */
  /* Sender clock stepped back: the filter crawls to the new level instead of
   * jumping (see ptp_clock_engine_add_sample). */
  bool crawl_started;
  bool crawl_ended;
  bool crawl_aborted;   /* ended because samples came back above the filter */
  int64_t crawl_move_ns; /* start: planned move; end: total move */
  int64_t raw_offset_ns;
  int64_t filtered_offset_ns;
  int64_t output_offset_ns;
  int64_t filtered_step_ns;
  int64_t raw_filter_delta_ns;
  uint32_t accepted_samples;
  uint32_t epoch;
} ptp_clock_engine_sample_t;

typedef struct {
  bool valid;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;

  int64_t raw_offset_ns;
  int64_t filtered_offset_ns; /* NQPTP-style minimum-delay estimate */
  int64_t output_offset_ns;   /* what audio uses: filtered, slew-limited */
  int64_t mastership_start_time_ns;
  int64_t last_accepted_time_ns;
  uint32_t accepted_samples;

  /* Drift feed-forward: the filter's own long-term slope (remote vs ESP
   * crystal), so a drifting envelope is tracked without a drift-dependent
   * lag. Measured over windows of filtered values. */
  int64_t drift_ns_per_s;
  bool drift_valid; /* at least one full window measured */
  int64_t drift_window_start_ns;
  int64_t drift_window_start_offset_ns;

  /* Consecutive samples far below the filter (a backward step of the
   * sender's clock) and the best (largest) offset seen in that run. */
  uint32_t neg_run;
  int64_t neg_run_max_ns;

  /* Crawl to a lower level after such a run, at a bounded rate. */
  bool crawl_active;
  int64_t crawl_target_ns;
  int64_t crawl_start_filter_ns;
  /* After a crawl starts, the output follows the filter at the slew rate
   * (not dt/tau) until it has caught up; no drift is learned meanwhile. */
  bool output_catchup;
} ptp_clock_engine_t;

void ptp_clock_engine_init(ptp_clock_engine_t *engine);

/* Set the currently observed PTP source / grandmaster domain. A first domain
 * starts epoch 1; a later source or GM change increments the epoch and resets
 * only estimator history. Returns true when the domain record changed. */
bool ptp_clock_engine_set_domain(ptp_clock_engine_t *engine,
                                 uint64_t source_clock_id,
                                 uint64_t grandmaster_clock_id,
                                 ptp_clock_engine_domain_event_t *event_out);

/* Reset smoothing/history while preserving the current source/GM identity.
 * Bumps the estimator epoch so readers can distinguish pre/post-reset state. */
void ptp_clock_engine_reset_filter(ptp_clock_engine_t *engine,
                                   bool bump_epoch);

/* NQPTP-derived passive one-way estimator:
 *  - first sample accepted directly;
 *  - positive jitter (shorter network delay) accepted fully during startup,
 *    then at 1/16;
 *  - negative jitter ignored during startup, then clamped to -2.5 ms and
 *    applied at 1/256;
 *  - a long run of samples far below the filter may be a backward clock
 *    step: the filter crawls toward the best sample of the run at a bounded
 *    rate, and stops as soon as a sample shows the level was higher (a run
 *    of delayed Syncs, not a step);
 *  - a long gap without samples restarts the mastership (drift unknown);
 *  - output_offset_ns follows the filter, slew-limited after the first
 *    PTP_CLOCK_ENGINE_READY_MS of a mastership: never a step. */
ptp_clock_engine_sample_t ptp_clock_engine_add_sample(
    ptp_clock_engine_t *engine, int64_t raw_offset_ns,
    int64_t reception_time_ns);

/* Convert between remote PTP time and ESP-local monotonic time. */
bool ptp_clock_engine_remote_to_local(uint64_t remote_ns, int64_t offset_ns,
                                      uint64_t *local_ns);
bool ptp_clock_engine_local_to_remote(uint64_t local_ns, int64_t offset_ns,
                                      uint64_t *remote_ns);

/* Shairport-style buffered handover qualification. A transient GM must never
 * move the media anchor; only a qualified, different master may take it over. */
bool ptp_clock_engine_handover_ready(bool have_local_anchor, bool clock_locked,
                                     uint64_t anchor_clock_id,
                                     uint64_t current_master_clock_id,
                                     uint32_t mastership_age_ms,
                                     uint32_t last_anchor_age_ms);

#ifdef __cplusplus
}
#endif
