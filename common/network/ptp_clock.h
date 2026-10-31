#pragma once

/* Speaker clock state supplied by Gateway TIME. No sockets or estimator. */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
esp_err_t ptp_clock_init_external(void);

typedef struct {
  bool realtime_mode;
  bool valid;
  bool locked;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;
  int64_t raw_offset_ns;
  /* The Gateway offset, carried forward on its published rate. */
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

/* The Gateway line mapped to this speaker's clock (spk_time.c). Audio
 * reads it through ptp_clock_get_snapshot().
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

/* Convert between remote PTP time and ESP-local monotonic time. */
bool ptp_clock_remote_to_local(uint64_t remote_ns, int64_t offset_ns,
                                      uint64_t *local_ns);
bool ptp_clock_local_to_remote(uint64_t local_ns, int64_t offset_ns,
                                      uint64_t *remote_ns);

/* Shairport-style buffered handover qualification. A transient GM must never
 * move the media anchor; only a qualified, different master may take it over. */
bool ptp_clock_handover_ready(bool have_local_anchor, bool clock_locked,
                                     uint64_t anchor_clock_id,
                                     uint64_t current_master_clock_id,
                                     uint32_t mastership_age_ms,
                                     uint32_t last_anchor_age_ms);
