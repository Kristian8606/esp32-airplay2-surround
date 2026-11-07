/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* The station and access point rules of README.md as a state machine
 * without ESP-IDF. wifi.c feeds it the Wi-Fi events from the default event
 * loop task (one task, so no locks here) and carries out what it asks for
 * through wl_ctl_ops_t. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_logic.h"

/* Whom to try to join. */
typedef struct {
  size_t saved_index; /* saved network: SSID and password */
  bool by_bssid;      /* else by SSID alone */
  uint8_t bssid[6];   /* by_bssid only */
  uint8_t channel;    /* by_bssid only */
} wl_target_t;

typedef enum {
  WL_AP_WHY_NO_SAVED,   /* no saved network */
  WL_AP_WHY_NO_ADDRESS, /* the station had no address for 30 s */
} wl_ap_why_t;

typedef struct {
  void *ctx;
  /* The right to scan or to attempt, shared with wifi_scan(). Never waits.
   */
  bool (*try_acquire)(void *ctx);
  void (*release)(void *ctx);
  /* Starts a scan of all channels; its end comes as wl_ctl_on_scan_done().
   * false: could not start. */
  bool (*start_scan)(void *ctx);
  /* esp_wifi_scan_stop() on the own scan after its 10 s. */
  void (*stop_scan)(void *ctx);
  /* The stopped scan is given up: a SCAN_DONE for it from now on is
   * ignored, and the driver's list is cleared. */
  void (*end_scan)(void *ctx);
  /* The scan time limit (10 s, then 1 s), likewise
   * (wl_ctl_on_scan_timer()). */
  void (*arm_scan_timer)(void *ctx, uint32_t ms);
  void (*cancel_scan_timer)(void *ctx);
  /* Starts an attempt; its end comes as wl_ctl_on_connected() or
   * wl_ctl_on_disconnected(). false: could not start. */
  bool (*connect)(void *ctx, const wl_target_t *target);
  /* One retry timer: arming it again replaces the old time. Its end comes
   * as wl_ctl_on_retry_timer(), also for 0 ms: then from a new event, never
   * from inside this call. */
  void (*arm_retry)(void *ctx, uint32_t ms);
  void (*cancel_retry)(void *ctx);
  /* The 30 s access point timer, likewise (wl_ctl_on_ap_timer()). */
  void (*arm_ap_timer)(void *ctx, uint32_t ms);
  void (*cancel_ap_timer)(void *ctx);
  /* The 20 s DHCP timer, likewise (wl_ctl_on_dhcp_timer()). */
  void (*arm_dhcp_timer)(void *ctx, uint32_t ms);
  void (*cancel_dhcp_timer)(void *ctx);
  /* Drops the association; its end comes as wl_ctl_on_disconnected().
   * false: could not (counts as disconnected at once). */
  bool (*disconnect)(void *ctx);
  /* Access point on (Wi-Fi mode station + access point) or off (station
   * only). */
  void (*set_ap)(void *ctx, bool on, wl_ap_why_t why);
  /* Captive DNS. false: did not start. */
  bool (*dns_start)(void *ctx);
  void (*dns_stop)(void *ctx);
} wl_ctl_ops_t;

/* How long to wait before looking again when a due attempt (or the access
 * point) finds wifi_scan() busy and no wl_ctl_on_resume() came. */
#define WL_BUSY_POLL_MS 1000

typedef enum {
  WL_OP_NONE,
  WL_OP_SCAN,
  WL_OP_SCAN_STOPPING, /* stopped after 10 s: SCAN_DONE or 1 s more */
  WL_OP_CONNECT,
} wl_op_t;

typedef struct {
  const wl_ctl_ops_t *ops;
  size_t saved_count;
  bool started;
  bool held;       /* holds the right from try_acquire() */
  wl_op_t op;      /* own scan or attempt running */
  bool associated; /* station associated */
  bool has_ip;
  bool ap_on;
  bool ap_due;         /* 30 s without address, waits for an op to end */
  unsigned ap_clients; /* stations on the access point */
  bool dns_on;
  bool dropping; /* disconnect() asked for: no address after 20 s */
  bool due;                      /* next_action waits for the right */
  wl_retry_action_t next_action; /* after the retry timer */
  unsigned failures;             /* in a row, since the last address */
  bool have_target;
  wl_target_t target; /* the access point of "the same access point" */
  bool round;         /* trying the saved networks by SSID alone */
  size_t round_index;
} wl_ctl_t;

void wl_ctl_init(wl_ctl_t *ctl, const wl_ctl_ops_t *ops, size_t saved_count);

/* The station has started (WIFI_EVENT_STA_START). Only the first call does
 * something. */
void wl_ctl_start(wl_ctl_t *ctl);

/* Whether the state machine wants the results of the scan that ended
 * (false also for its own scan that was stopped). */
bool wl_ctl_wants_scan(const wl_ctl_t *ctl);
/* The own scan ended with these results (none on a failed or stopped
 * scan). */
void wl_ctl_on_scan_done(wl_ctl_t *ctl, const wl_ap_t *aps, size_t ap_count,
                         const wl_saved_t *saved, size_t saved_count);

void wl_ctl_on_connected(wl_ctl_t *ctl, const uint8_t bssid[6],
                         uint8_t channel);
void wl_ctl_on_disconnected(wl_ctl_t *ctl);
void wl_ctl_on_got_ip(wl_ctl_t *ctl);
void wl_ctl_on_lost_ip(wl_ctl_t *ctl);
void wl_ctl_on_retry_timer(wl_ctl_t *ctl);
void wl_ctl_on_ap_timer(wl_ctl_t *ctl);
void wl_ctl_on_dhcp_timer(wl_ctl_t *ctl);
void wl_ctl_on_scan_timer(wl_ctl_t *ctl);
/* Number of stations on the access point now. */
void wl_ctl_on_ap_clients(wl_ctl_t *ctl, unsigned count);
/* wifi_scan() gave the right back. */
void wl_ctl_on_resume(wl_ctl_t *ctl);
