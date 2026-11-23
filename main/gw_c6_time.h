#pragma once
/* Time from the C6 co-processor (P4 Gateway; phase 3 of ROADMAP_GATEWAY_P4).
 *
 * The C6 raises the WAKEUP line every SURROUND_TIME_INTERVAL_MS (its pulse,
 * made in hardware) and sends a TICK per edge over ESP-Hosted peer data:
 * the edge's number, its C6 clock value and the AP's TSF at it
 * (common/gw_c6/gw_c6_proto.h). Here:
 *   - the edge is captured in hardware: GPIO --ETM--> GPTimer CAPTURE; an
 *     IRAM interrupt on the same edge only reads the capture and, right
 *     after, the GPTimer and esp_timer together (3.1, 3.2);
 *   - edges are numbered as the C6 numbers them (the first edge after the
 *     C6's reset is 0), so a TICK is paired with its edge by number, also
 *     when the message comes late (3.3);
 *   - a line C6 clock -> local time over the last pairs (3.4);
 *   - each pair gives tsf_fit a sample: the edge's local time and the TSF at
 *     it (3.5, common/surround/tsf_fit.c). */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Early in app_main, before the C6 starts its pulse (the P4 resets the C6
 * when ESP-Hosted starts, and the C6 needs ~0.5 s to its first edge). */
esp_err_t gw_c6_time_start(void);

/* The newest edge not yet taken: local time (esp_timer, ns) of an edge and
 * the TSF (ns) at it. Each once; an older one not taken is dropped. False
 * when none is waiting. */
bool gw_c6_time_sample(int64_t *local_ns, int64_t *tsf_ns);

/* Local time (esp_timer, ns) at a C6 clock value, from the line (phase 4). */
bool gw_c6_time_c6_to_local(uint64_t c6_ticks, int64_t *local_ns);

/* apple_clock's receive-time hook (apple_clock_set_rx_time_hook): the local
 * time a PTP packet came from the air, from the C6's record of it (phase 4).
 * False when there is no record or no line yet. */
bool gw_c6_time_ptp_rx(uint8_t type, uint16_t seq, const uint8_t port_id[10],
                       int64_t socket_ns, int64_t *rx_ns);

typedef struct {
  bool hello;            /* the C6 answered with our firmware */
  char c6_name[32];
  char c6_version[32];
  uint8_t c6_features;
  uint32_t edges;        /* edges captured */
  uint32_t edges_missed; /* edges whose interrupt did not run (seen by the interval) */
  uint32_t restarts;     /* numbering started over (irregular interval or pairing lost) */
  uint32_t ticks;        /* TICK messages */
  uint32_t paired;       /* TICKs paired with their edge */
  uint32_t unpaired;     /* no captured edge with that number */
  uint32_t rejected;     /* pair failed the interval check */
  uint32_t no_tsf;       /* TICK without a valid TSF */
  uint32_t samples;      /* samples handed to tsf_fit */
  uint32_t ptp_records;  /* PTP receive records from the C6 */
  uint32_t ptp_found;    /* apple_clock asked and got the C6 time */
  uint32_t ptp_missed;   /* apple_clock asked, no record (or no line) */
  double line_rms_ns;    /* residual RMS of the C6 -> local line */
  double line_ppm;       /* C6 clock against the local one */
  double latency_ms;     /* last TICK's arrival after its edge */
} gw_c6_time_stats_t;
void gw_c6_time_get_stats(gw_c6_time_stats_t *out);
