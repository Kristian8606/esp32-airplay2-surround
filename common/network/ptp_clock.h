#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Simple PTP (IEEE 1588) slave for AirPlay time synchronization.
 * Listens for SYNC/FOLLOW_UP messages and tracks offset to PTP master.
 */

/**
 * Initialize and start PTP clock synchronization.
 * Creates a task that listens for PTP multicast messages.
 * Returns ESP_ERR_INVALID_STATE while an earlier task is running or stopping.
 */
esp_err_t ptp_clock_init(void);

/**
 * Surround speaker: clock state only, no PTP sockets or task. The sender's
 * clock then comes only from ptp_clock_set_external().
 */
esp_err_t ptp_clock_init_external(void);


/**
 * Clear PTP clock synchronization state.
 * Resets offset and lock status without stopping the clock.
 * Called during TEARDOWN to allow re-sync on new session.
 */
void ptp_clock_clear(void);

typedef struct {
  bool realtime_mode;
  bool valid;
  bool locked;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;
  int64_t raw_offset_ns;
  /* The usable remote-local offset: minimum-delay filter, slew-limited once
   * the mastership is ready, so audio never sees a step. */
  int64_t filtered_offset_ns;
  int64_t raw_filter_delta_ns;
  /* Age of the current mastership; buffered audio uses this to qualify a
   * new GM while the media timeline remains in ESP-local time. */
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  uint32_t sample_age_ms; /* UINT32_MAX when no accepted timing sample exists. */
  /* Diagnostics: why a buffered anchor may never become usable. */
  bool source_mixed; /* source identity changed within selected peer */
  /* Surround speaker: how fast the offset moves (the Gateway line's rate on
   * this clock, ppb), which is the I2S correction the sender needs. */
  bool offset_rate_valid;
  int32_t offset_rate_ppb;
} ptp_clock_snapshot_t;

/* Coherent status/diagnostic snapshot for either buffered or realtime mode. */
void ptp_clock_get_snapshot(ptp_clock_snapshot_t *snapshot);

/* One measured Sync of the selected sender, unfiltered: the sender's clock
 * read remote_ns no later than local_ns (the reception adds the delay). */
typedef struct {
  int64_t local_ns;
  int64_t remote_ns;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
} ptp_clock_sample_t;

/* The Syncs measured since *cursor (0 at first), oldest first, at most max;
 * advances *cursor. Samples older than the last PTP_CLOCK_SAMPLES are lost.
 * Lets the Surround Gateway fit its own line to the raw samples. */
uint32_t ptp_clock_read_samples(uint32_t *cursor, ptp_clock_sample_t *out,
                                uint32_t max);

/* Diagnostic: called for every measured Sync with its sequence number, the
 * PTP task's reception stamp and the raw offset (sender - that stamp), with
 * the PTP state lock held: it must only record. NULL turns it off. */
typedef void (*ptp_clock_sync_probe_t)(uint16_t seq, int64_t task_rx_ns,
                                       int64_t raw_offset_ns);
void ptp_clock_set_sync_probe(ptp_clock_sync_probe_t probe);

/**
 * Stream SETUP equivalent of Shairport/NQPTP T <RTSP client IP>, then B.
 * Starts one fresh timing record and selects that IP for both AAC and ALAC,
 * except that a settled estimate of the same sender is kept, also across a
 * switch between realtime and buffered. SETPEERS/SETPEERSX and media anchors
 * never reset this record.
 */
void ptp_clock_start_timing(bool realtime, uint32_t timing_peer_ip);



typedef struct {
  bool realtime_mode;
  bool master_ready;
  uint64_t master_clock_id;
  uint64_t source_clock_id;
  int64_t master_offset_ns;
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  uint32_t sample_age_ms; /* UINT32_MAX when no Follow_Up is available. */
  /* Monotonic within one realtime PTP session. Incremented whenever Announce
   * changes grandmasterIdentity after the first master has been observed.
   * Audio uses this only to distinguish mastership epochs; it never feeds
   * back into PTP selection/filtering. */
  uint32_t gm_change_count;
} ptp_realtime_snapshot_t;

/* Convert using one coherent clock snapshot. Both clock identity and readiness
 * belong to the same observation as the offset (including D7 admission). */
bool ptp_clock_realtime_snapshot_to_local(
    const ptp_realtime_snapshot_t *snapshot, uint64_t clock_id,
    uint64_t remote_ns, uint64_t *local_ns);

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *snapshot);

/* Surround speaker: the Gateway's estimate of the sender clock, already
 * mapped to this ESP's clock (spk_time.c), replaces the PTP estimator. Audio
 * reads it through ptp_clock_get_snapshot() exactly like a PTP estimate.
 * A speaker starts with ptp_clock_init_external(): it receives no PTP. */
typedef struct {
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  int64_t offset_ns;      /* sender PTP - local */
  int64_t sample_time_ns; /* local time the offset was measured for */
  /* How fast the offset moves (d offset / d local, ppb): the snapshot carries
   * it forward from sample_time_ns, so the time runs on between messages. */
  int32_t offset_rate_ppb;
  uint32_t mastership_age_ms;
  uint32_t sample_count;
  bool locked;
  /* Realtime (type 96) estimate: served by ptp_clock_get_realtime_snapshot()
   * like a realtime PTP session, with its own GM-change epochs. */
  bool realtime;
} ptp_clock_external_t;

/* NULL withdraws the estimate (the snapshot becomes invalid). */
void ptp_clock_set_external(const ptp_clock_external_t *ext);
