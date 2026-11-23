/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
/* Messages to the P4 (1.7), format in common/gw_c6/gw_c6_proto.h.
 *
 * One task sends everything, so the order on the link is the order of events:
 *   - HELLO once before the first TICK, and again after every HELLO_REQ;
 *   - TICK right after every pulse edge (about 1 ms later: the task sleeps
 *     until the next edge), with the C6 clock at the edge and the TSF from the
 *     TSF line, plus any PTP records still waiting;
 *   - PTP as soon as c6_ptp has a record: the frame path wakes the task, so a
 *     record does not wait up to SURROUND_TIME_INTERVAL_MS for the next TICK.
 *     The P4 matches a record to its packet by type, sequenceId and source, so
 *     it may come just before or just after the packet itself. */
#include <stdint.h>

#include "esp_err.h"
#include "gw_c6_proto.h"

/* features: GW_C6_FEAT_* of the parts that started. Needs the parts it lists. */
esp_err_t c6_link_start(uint8_t features);

/* Counters for the statistics (1.8). */
typedef struct {
  uint32_t hello;        /* HELLO sent */
  uint32_t ticks;        /* TICK sent */
  uint32_t ticks_missed; /* edges without a TICK (the task was late a whole period) */
  uint32_t ptp_msgs;     /* PTP messages sent */
  uint32_t ptp_recs;     /* records sent, in TICK and PTP */
  uint32_t send_fail;    /* peer data sends that failed (message lost) */
} c6_link_stats_t;
void c6_link_get_stats(c6_link_stats_t *out);
