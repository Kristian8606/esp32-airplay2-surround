/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_ctl.h"

#include <limits.h>
#include <string.h>

static void fail(wl_ctl_t *c);
static void ap_check(wl_ctl_t *c);

static void release(wl_ctl_t *c) {
  if (c->held) {
    c->held = false;
    c->ops->release(c->ops->ctx);
  }
}

static void ap_on(wl_ctl_t *c, wl_ap_why_t why) {
  if (c->ap_on) {
    return;
  }
  c->ap_on = true;
  c->ops->set_ap(c->ops->ctx, true, why);
  if (!c->dns_on) {
    c->dns_on = c->ops->dns_start(c->ops->ctx);
  }
}

static void ap_off(wl_ctl_t *c) {
  if (!c->ap_on) {
    return;
  }
  if (c->dns_on) {
    c->dns_on = false;
    c->ops->dns_stop(c->ops->ctx);
  }
  c->ap_on = false;
  c->ap_clients = 0;
  c->ops->set_ap(c->ops->ctx, false, WL_AP_WHY_NO_ADDRESS);
}

/* The access point turns on after 30 s without address, but never in the
 * middle of an attempt or a scan (own or wifi_scan()): then right after it.
 */
static void ap_check(wl_ctl_t *c) {
  if (!c->ap_due) {
    return;
  }
  if (c->has_ip || c->ap_on) {
    c->ap_due = false;
    return;
  }
  if (c->op != WL_OP_NONE) {
    return; /* checked again when it ends */
  }
  bool took = false;
  if (!c->held) {
    if (!c->ops->try_acquire(c->ops->ctx)) {
      /* wifi_scan() runs; wl_ctl_on_resume() follows, this is a fallback. */
      c->ops->arm_ap_timer(c->ops->ctx, WL_BUSY_POLL_MS);
      return;
    }
    c->held = true;
    took = true;
  }
  c->ap_due = false;
  ap_on(c, WL_AP_WHY_NO_ADDRESS);
  if (took) {
    release(c);
  }
}

static void lose_ip(wl_ctl_t *c) {
  if (!c->has_ip) {
    return;
  }
  c->has_ip = false;
  if (!c->ap_on) {
    c->ops->arm_ap_timer(c->ops->ctx, WL_NO_IP_AP_MS);
  }
}

/* Whether a round by SSID alone has a network after the current one. */
static bool round_has_next(const wl_ctl_t *c) {
  return c->round && c->round_index + 1 < c->saved_count;
}

/* One attempt on the current target, or on the current network of the round
 * by SSID alone. When one can't even start, the round goes on with the next
 * network at once: in a loop, not by recursion, because this runs on the
 * event loop task's small stack. */
static void attempt(wl_ctl_t *c) {
  for (;;) {
    wl_target_t t;
    if (c->round) {
      memset(&t, 0, sizeof(t));
      t.saved_index = c->round_index;
    } else {
      t = c->target;
    }
    c->op = WL_OP_CONNECT;
    if (c->ops->connect(c->ops->ctx, &t)) {
      return;
    }
    c->op = WL_OP_NONE;
    ap_check(c);
    if (!round_has_next(c)) {
      fail(c);
      return;
    }
    c->round_index++;
  }
}

static void full_choice(wl_ctl_t *c) {
  c->op = WL_OP_SCAN;
  if (!c->ops->start_scan(c->ops->ctx)) {
    c->op = WL_OP_NONE;
    fail(c);
    return;
  }
  c->ops->arm_scan_timer(c->ops->ctx, WL_SCAN_TIMEOUT_MS);
}

/* The stopped scan is over (its SCAN_DONE or 1 s more): a failure. */
static void stopped_scan_over(wl_ctl_t *c) {
  c->ops->cancel_scan_timer(c->ops->ctx);
  c->ops->end_scan(c->ops->ctx);
  c->op = WL_OP_NONE;
  fail(c);
}

static void run_due(wl_ctl_t *c) {
  if (!c->due || c->op != WL_OP_NONE || c->associated) {
    return;
  }
  if (!c->held) {
    if (!c->ops->try_acquire(c->ops->ctx)) {
      /* wifi_scan() runs; wl_ctl_on_resume() follows, this is a fallback. */
      c->ops->arm_retry(c->ops->ctx, WL_BUSY_POLL_MS);
      return;
    }
    c->held = true;
  }
  ap_check(c); /* a due access point goes first */
  c->due = false;
  c->ops->cancel_retry(c->ops->ctx);
  if (c->next_action == WL_RETRY_SAME && (c->have_target || c->round)) {
    c->round_index = 0;
    attempt(c);
  } else {
    full_choice(c);
  }
}

/* An attempt or scan failed: count it and wait as wl_retry_after() says.
 * Also "at once" goes through arm_retry(0), never straight back into
 * run_due(): no recursion on the event loop task's stack. */
static void fail(wl_ctl_t *c) {
  ap_check(c);
  if (c->failures < UINT_MAX) {
    c->failures++;
  }
  wl_retry_t r = wl_retry_after(c->failures, c->ap_clients > 0);
  c->next_action = r.action;
  release(c);
  c->ops->arm_retry(c->ops->ctx, r.delay_ms);
}

/* In a round by SSID alone the next saved network follows at once; the
 * round as a whole counts as one failure. */
static void attempt_failed(wl_ctl_t *c) {
  ap_check(c);
  if (round_has_next(c)) {
    c->round_index++;
    attempt(c);
  } else {
    fail(c);
  }
}

void wl_ctl_init(wl_ctl_t *ctl, const wl_ctl_ops_t *ops, size_t saved_count) {
  memset(ctl, 0, sizeof(*ctl));
  ctl->ops = ops;
  ctl->saved_count = saved_count > WL_MAX_SAVED ? WL_MAX_SAVED : saved_count;
  ctl->next_action = WL_RETRY_FULL;
}

void wl_ctl_start(wl_ctl_t *c) {
  if (c->started) {
    return;
  }
  c->started = true;
  if (c->saved_count == 0) {
    ap_on(c, WL_AP_WHY_NO_SAVED);
    return;
  }
  c->ops->arm_ap_timer(c->ops->ctx, WL_NO_IP_AP_MS);
  c->next_action = WL_RETRY_FULL;
  c->due = true;
  run_due(c);
}

bool wl_ctl_wants_scan(const wl_ctl_t *c) { return c->op == WL_OP_SCAN; }

void wl_ctl_on_scan_done(wl_ctl_t *c, const wl_ap_t *aps, size_t ap_count,
                         const wl_saved_t *saved, size_t saved_count) {
  if (c->op == WL_OP_SCAN_STOPPING) {
    stopped_scan_over(c);
    return;
  }
  if (c->op != WL_OP_SCAN) {
    return;
  }
  c->ops->cancel_scan_timer(c->ops->ctx);
  c->op = WL_OP_NONE;
  ap_check(c);
  size_t saved_index = 0;
  int i = wl_choose(aps, ap_count, saved, saved_count, &saved_index);
  if (i >= 0) {
    c->round = false;
    c->have_target = true;
    memset(&c->target, 0, sizeof(c->target));
    c->target.saved_index = saved_index;
    c->target.by_bssid = true;
    memcpy(c->target.bssid, aps[i].bssid, sizeof(c->target.bssid));
    c->target.channel = aps[i].channel;
    attempt(c);
  } else if (c->saved_count > 0) {
    c->round = true;
    c->round_index = 0;
    attempt(c);
  } else {
    fail(c);
  }
}

void wl_ctl_on_connected(wl_ctl_t *c, const uint8_t bssid[6],
                         uint8_t channel) {
  size_t saved_index = c->round ? c->round_index : c->target.saved_index;
  c->op = WL_OP_NONE;
  c->associated = true;
  /* From now on "the same access point" is the one joined, also when it was
   * found by SSID alone. */
  c->round = false;
  c->have_target = true;
  memset(&c->target, 0, sizeof(c->target));
  c->target.saved_index = saved_index;
  c->target.by_bssid = true;
  memcpy(c->target.bssid, bssid, sizeof(c->target.bssid));
  c->target.channel = channel;
  c->dropping = false;
  c->ops->arm_dhcp_timer(c->ops->ctx, WL_DHCP_TIMEOUT_MS);
  ap_check(c);
  release(c);
}

void wl_ctl_on_disconnected(wl_ctl_t *c) {
  c->dropping = false;
  c->ops->cancel_dhcp_timer(c->ops->ctx);
  if (c->op == WL_OP_CONNECT) {
    c->op = WL_OP_NONE;
    c->associated = false;
    attempt_failed(c);
    return;
  }
  if (!c->associated) {
    return;
  }
  c->associated = false;
  lose_ip(c);
  fail(c);
}

void wl_ctl_on_got_ip(wl_ctl_t *c) {
  c->has_ip = true;
  c->failures = 0;
  c->due = false;
  c->ap_due = false;
  c->ops->cancel_retry(c->ops->ctx);
  c->ops->cancel_ap_timer(c->ops->ctx);
  c->ops->cancel_dhcp_timer(c->ops->ctx);
  ap_off(c);
}

void wl_ctl_on_lost_ip(wl_ctl_t *c) {
  if (c->has_ip && c->associated) {
    c->ops->arm_dhcp_timer(c->ops->ctx, WL_DHCP_TIMEOUT_MS);
  }
  lose_ip(c);
}

/* Associated, but no address 20 s after joining or after losing it: drop
 * the link; the disconnect counts as a failure. */
void wl_ctl_on_dhcp_timer(wl_ctl_t *c) {
  if (!c->associated || c->has_ip || c->dropping) {
    return;
  }
  c->dropping = true;
  if (!c->ops->disconnect(c->ops->ctx)) {
    c->dropping = false;
    c->associated = false;
    lose_ip(c);
    fail(c);
  }
}

/* The own scan has the limits of wifi_scan(): stopped after 10 s, then
 * its SCAN_DONE or 1 s more, and it counts as a failure. */
void wl_ctl_on_scan_timer(wl_ctl_t *c) {
  if (c->op == WL_OP_SCAN) {
    c->op = WL_OP_SCAN_STOPPING;
    c->ops->stop_scan(c->ops->ctx);
    c->ops->arm_scan_timer(c->ops->ctx, WL_SCAN_STOP_GRACE_MS);
  } else if (c->op == WL_OP_SCAN_STOPPING) {
    stopped_scan_over(c);
  }
}

void wl_ctl_on_ap_clients(wl_ctl_t *c, unsigned count) {
  c->ap_clients = c->ap_on ? count : 0;
}

void wl_ctl_on_retry_timer(wl_ctl_t *c) {
  if (!c->started || c->saved_count == 0 || c->associated ||
      c->op != WL_OP_NONE) {
    return;
  }
  c->due = true;
  run_due(c);
}

void wl_ctl_on_ap_timer(wl_ctl_t *c) {
  if (c->started && !c->has_ip && !c->ap_on) {
    c->ap_due = true;
    ap_check(c);
  }
}

void wl_ctl_on_resume(wl_ctl_t *c) {
  ap_check(c);
  run_due(c);
}
