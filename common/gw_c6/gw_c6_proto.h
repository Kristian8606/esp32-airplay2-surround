#pragma once

/* P4 Gateway <-> its C6 co-processor: messages over ESP-Hosted peer data
 * (custom RPC: esp_hosted_send_custom_data / esp_hosted_register_custom_callback
 * on the P4, eh_cp_feat_peer_data_send / _register_callback on the C6).
 *
 * The peer data msg_id is the message type. Every payload starts with
 * gw_c6_proto: a side that sees another version ignores the message (the P4
 * then asks for HELLO and reports the C6 firmware as not matching).
 * Payloads are packed, little endian (both chips are RISC-V).
 *
 * C6 -> P4:
 *   HELLO  name and version of the C6 firmware, its clock and pulse period.
 *          Once before the first TICK and after every HELLO_REQ.
 *   TICK   one per pulse edge on the WAKEUP line (every
 *          SURROUND_TIME_INTERVAL_MS): the C6 clock at the edge (exact) and
 *          the AP's TSF at the edge. PTP records still waiting are appended.
 *   PTP    PTP receive records, sent as soon as they are taken (normally
 *          one per PTP packet), so they do not wait for the next TICK.
 * P4 -> C6:
 *   HELLO_REQ  empty after the proto byte; the C6 answers with HELLO.
 *
 * Every time is a count of the C6 clock ("c6 ticks", HELLO.clock_hz). */

#include <assert.h>
#include <stdint.h>

#define GW_C6_PROTO 1

/* peer data msg_id ("GC" + type). 0xFFFFFFFF is reserved by ESP-Hosted. */
#define GW_C6_MSG_HELLO     0x47430001U /* C6 -> P4 */
#define GW_C6_MSG_TICK      0x47430002U /* C6 -> P4 */
#define GW_C6_MSG_PTP       0x47430003U /* C6 -> P4 */
#define GW_C6_MSG_HELLO_REQ 0x47430081U /* P4 -> C6 */

/* HELLO.features: what runs on the C6. A part that failed to start stays off
 * (the C6 firmware never aborts), and the P4 sees it here. */
#define GW_C6_FEAT_CLOCK 0x01 /* the C6 clock */
#define GW_C6_FEAT_PULSE 0x02 /* the pulse on WAKEUP; TICKs are sent */
#define GW_C6_FEAT_TSF   0x04 /* the TSF line */
#define GW_C6_FEAT_PTP   0x08 /* PTP receive records */

#define GW_C6_NAME_LEN 32

typedef struct __attribute__((packed)) {
  uint8_t proto;              /* GW_C6_PROTO */
  uint8_t features;           /* GW_C6_FEAT_* */
  uint16_t reserved;          /* 0 */
  uint32_t clock_hz;          /* C6 clock rate */
  uint32_t pulse_period;      /* C6 ticks from one edge to the next, measured; 0 = no pulse */
  char name[GW_C6_NAME_LEN];  /* esp_app_desc project_name, NUL terminated */
  char version[GW_C6_NAME_LEN];
  char idf_ver[GW_C6_NAME_LEN];
} gw_c6_hello_t;
static_assert(sizeof(gw_c6_hello_t) == 12 + 3 * GW_C6_NAME_LEN, "gw_c6_hello_t layout");

/* TICK.flags */
#define GW_C6_TICK_CONNECTED 0x01 /* the station is associated */
#define GW_C6_TICK_TSF_VALID 0x02 /* edge_tsf_ns and tsf_span_ns are valid */
#define GW_C6_TICK_RESTART   0x04 /* new association or a TSF step since the last TICK:
                                     TSF values before and after do not form one line */

/* gw_c6_ptp_rec_t.dir. Only receive: sends are timestamped on the P4 (1.6). */
#define GW_C6_DIR_RX 0

typedef struct __attribute__((packed)) {
  uint8_t dir;        /* GW_C6_DIR_RX */
  uint8_t type;       /* PTP messageType */
  uint16_t seq;       /* PTP sequenceId */
  uint8_t src_id[10]; /* PTP sourcePortIdentity */
  uint64_t c6_ticks;  /* C6 clock when the frame came from the air */
} gw_c6_ptp_rec_t;
static_assert(sizeof(gw_c6_ptp_rec_t) == 22, "gw_c6_ptp_rec_t layout");

typedef struct __attribute__((packed)) {
  uint8_t proto;          /* GW_C6_PROTO */
  uint8_t flags;          /* GW_C6_TICK_* */
  uint16_t seq;           /* pulse number (low 16 bits; 0 = first edge) */
  uint64_t edge_c6_ticks; /* C6 clock at the edge (exact) */
  uint64_t edge_tsf_ns;   /* AP's TSF at the edge, ns */
  uint32_t tsf_span_ns;   /* smallest pairing span in the TSF line (quality) */
  uint8_t n_ptp;          /* gw_c6_ptp_rec_t records that follow */
  /* gw_c6_ptp_rec_t ptp[n_ptp]; */
} gw_c6_tick_t;
static_assert(sizeof(gw_c6_tick_t) == 25, "gw_c6_tick_t layout");

typedef struct __attribute__((packed)) {
  uint8_t proto; /* GW_C6_PROTO */
  uint8_t n_ptp; /* gw_c6_ptp_rec_t records that follow */
  /* gw_c6_ptp_rec_t ptp[n_ptp]; */
} gw_c6_ptp_t;
static_assert(sizeof(gw_c6_ptp_t) == 2, "gw_c6_ptp_t layout");

/* The most records in one TICK or PTP message. */
#define GW_C6_MAX_PTP 16
