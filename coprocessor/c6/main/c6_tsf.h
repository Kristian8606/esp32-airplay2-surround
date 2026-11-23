/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
/* TSF <-> C6 clock (1.4).
 *
 * A task pairs the AP's TSF with the C6 clock every SURROUND_TIME_INTERVAL_MS:
 * C6_TSF_TRIES times (clock, esp_wifi_get_tsf_time(), clock), and keeps the
 * pair with the smallest span. esp_wifi_get_tsf_time() is called from a task,
 * its documented path (it goes through the Wi-Fi task, about 37 us; 0.8).
 * A pair is dropped when the TSF is 0 (not connected, no beacon yet) or when
 * its span is more than C6_TSF_SPAN_MARGIN_US above the smallest span of the
 * last C6_TSF_SPAN_WINDOW pairs.
 *
 * A least-squares line over the last C6_TSF_FIT_N pairs (outliers rejected)
 * gives the TSF at any C6 clock value, in particular at a pulse edge. A new
 * association (other BSSID or reconnect) or a pair far off the line (the TSF
 * stepped) starts the line over and raises C6_TSF_FLAG_RESTART once. */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define C6_TSF_TRIES 8
#define C6_TSF_FIT_N 16          /* 2 s */
#define C6_TSF_MIN_FIT 4         /* pairs before the line is used */
#define C6_TSF_SPAN_WINDOW 80    /* 10 s of pairs for the smallest span */
#define C6_TSF_SPAN_MARGIN_US 4
#define C6_TSF_REJECT_NS 30000   /* a pair this far off the line is an outlier */
#define C6_TSF_JUMP_NS 500000    /* three in a row this far off: the TSF stepped */

#define C6_TSF_FLAG_CONNECTED 0x01 /* the station is associated */
#define C6_TSF_FLAG_VALID 0x02     /* the line is good enough to use */
#define C6_TSF_FLAG_RESTART 0x04   /* new association or TSF step since last read */

/* Needs c6_clock_start() and the default event loop. ESP_OK when the task runs. */
esp_err_t c6_tsf_start(void);

/* TSF (ns) at a C6 clock value from the current line; span_ns = the smallest
 * pairing span in the line (quality). flags as above; RESTART is cleared by
 * this call. Returns false when the line is not valid (then tsf_ns = 0). */
bool c6_tsf_at(uint64_t c6_ticks, int64_t *tsf_ns, uint32_t *span_ns, uint8_t *flags);
/* (c6_tsf_at and c6_tsf_get_stats: task context, after c6_tsf_start.) */

/* Counters for the statistics (1.8). */
typedef struct {
  uint32_t pairs, dropped_zero, dropped_span, outliers, restarts;
} c6_tsf_stats_t;
void c6_tsf_get_stats(c6_tsf_stats_t *out);
