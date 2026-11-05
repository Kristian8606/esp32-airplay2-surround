/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Host tests of src/wifi_ctl.c: the station and access point examples of
 * README.md on a simulated clock, with a simulated radio world. */
#include <stdlib.h>

#include "check.h"
#include "wifi_ctl.h"

#define MAX_LOG 256
#define SCAN_MS 2500     /* a scan takes this long */
#define ATTEMPT_MS 300   /* an attempt ends this long after it starts */
#define DHCP_MS 200      /* address this long after associating */
#define DROP_MS 50       /* disconnect() to its WIFI_EVENT_STA_DISCONNECTED */
#define MAX_WORLD 8
#define MAX_QUEUE 16

typedef struct {
  char ssid[33];
  uint8_t bssid[6];
  uint8_t channel;
  int8_t rssi;
  wl_sec_t sec;
  bool hidden;
  bool up;
} world_ap_t;

typedef struct {
  char ssid[33];
  bool has_password;
  bool password_right;
} sim_saved_t;

typedef enum { Q_SCAN_DONE, Q_CONNECT_RESULT, Q_GOT_IP, Q_DROPPED } q_type_t;

typedef struct {
  int64_t at;
  q_type_t type;
  bool ok;
  uint8_t bssid[6];
  uint8_t channel;
} q_event_t;

typedef struct {
  int64_t at;
  wl_target_t target;
  uintptr_t sp; /* stack depth at connect() */
} attempt_log_t;

typedef struct {
  int64_t at;
  bool on;
  wl_ap_why_t why;
} ap_log_t;

typedef struct {
  wl_ctl_t ctl;
  wl_ctl_ops_t ops;
  int64_t now;
  int64_t retry_at; /* -1: not armed */
  int64_t ap_timer_at;
  int64_t dhcp_at;
  int64_t scan_timer_at;
  int64_t scan_ms;    /* SCAN_MS unless a test changes it */
  int64_t attempt_ms; /* ATTEMPT_MS likewise */
  q_event_t queue[MAX_QUEUE];
  size_t queue_len;

  world_ap_t world[MAX_WORLD];
  size_t world_len;
  sim_saved_t saved[WL_MAX_SAVED];
  size_t saved_len;
  bool dhcp_works;

  bool held;
  bool user_busy;
  bool scan_start_fails;
  bool connect_start_fails;
  bool dns_fails;
  bool disconnect_fails;
  bool scan_hangs;            /* no SCAN_DONE by itself */
  int64_t done_after_stop_ms; /* SCAN_DONE this long after a stop; -1 none */
  bool scan_running;          /* the glue's "a scan of ours runs" */
  int scan_stops, scan_ends;
  int64_t scan_stop_at[MAX_LOG];
  int disconnects;
  int64_t disconnect_at[MAX_LOG];
  bool dns_running;
  bool ap_is_on;
  int dns_starts, dns_stops;
  int double_dns_start; /* must stay 0 */
  int bad_release;      /* must stay 0 */

  attempt_log_t attempts[MAX_LOG];
  size_t attempts_len;
  int64_t scans[MAX_LOG];
  size_t scans_len;
  ap_log_t ap_log[MAX_LOG];
  size_t ap_log_len;
} sim_t;

static sim_t *S; /* ops ctx */

static void push(sim_t *s, q_event_t e) {
  if (s->queue_len < MAX_QUEUE) {
    s->queue[s->queue_len++] = e;
  }
}

static bool op_try_acquire(void *ctx) {
  sim_t *s = ctx;
  if (s->held || s->user_busy) {
    return false;
  }
  s->held = true;
  return true;
}

static void op_release(void *ctx) {
  sim_t *s = ctx;
  if (!s->held) {
    s->bad_release++;
  }
  s->held = false;
}

static bool op_start_scan(void *ctx) {
  sim_t *s = ctx;
  if (!s->held) {
    s->bad_release++; /* scanning without the right */
  }
  if (s->scan_start_fails) {
    return false;
  }
  if (s->scans_len < MAX_LOG) {
    s->scans[s->scans_len++] = s->now;
  }
  s->scan_running = true;
  if (!s->scan_hangs) {
    push(s, (q_event_t){.at = s->now + s->scan_ms, .type = Q_SCAN_DONE});
  }
  return true;
}

static void op_stop_scan(void *ctx) {
  sim_t *s = ctx;
  if (s->scan_stops < MAX_LOG) {
    s->scan_stop_at[s->scan_stops] = s->now;
  }
  s->scan_stops++;
  if (s->done_after_stop_ms >= 0) {
    push(s, (q_event_t){.at = s->now + s->done_after_stop_ms,
                        .type = Q_SCAN_DONE});
  }
}

static void op_end_scan(void *ctx) {
  sim_t *s = ctx;
  s->scan_ends++;
  s->scan_running = false;
}

static void op_arm_scan_timer(void *ctx, uint32_t ms) {
  sim_t *s = ctx;
  s->scan_timer_at = s->now + ms;
}
static void op_cancel_scan_timer(void *ctx) {
  ((sim_t *)ctx)->scan_timer_at = -1;
}

static bool op_connect(void *ctx, const wl_target_t *t) {
  sim_t *s = ctx;
  if (!s->held) {
    s->bad_release++;
  }
  volatile char here = 0;
  if (s->attempts_len < MAX_LOG) {
    s->attempts[s->attempts_len++] =
        (attempt_log_t){s->now, *t, (uintptr_t)&here};
  }
  if (s->connect_start_fails) {
    return false;
  }
  q_event_t e = {.at = s->now + s->attempt_ms, .type = Q_CONNECT_RESULT};
  if (t->saved_index < s->saved_len) {
    const sim_saved_t *net = &s->saved[t->saved_index];
    int best = -1;
    for (size_t i = 0; i < s->world_len; i++) {
      const world_ap_t *w = &s->world[i];
      bool match = w->up && strcmp(w->ssid, net->ssid) == 0 &&
                   wl_sec_allowed(net->has_password, w->sec);
      if (t->by_bssid) {
        match = match && memcmp(w->bssid, t->bssid, 6) == 0 &&
                w->channel == t->channel;
      }
      if (match && (best < 0 || w->rssi > s->world[best].rssi)) {
        best = (int)i;
      }
    }
    if (best >= 0 && (!net->has_password || net->password_right)) {
      e.ok = true;
      memcpy(e.bssid, s->world[best].bssid, 6);
      e.channel = s->world[best].channel;
    }
  }
  push(s, e);
  return true;
}

static void op_arm_retry(void *ctx, uint32_t ms) {
  sim_t *s = ctx;
  s->retry_at = s->now + ms;
}
static void op_cancel_retry(void *ctx) { ((sim_t *)ctx)->retry_at = -1; }
static void op_arm_ap_timer(void *ctx, uint32_t ms) {
  sim_t *s = ctx;
  s->ap_timer_at = s->now + ms;
}
static void op_cancel_ap_timer(void *ctx) { ((sim_t *)ctx)->ap_timer_at = -1; }

static void op_arm_dhcp_timer(void *ctx, uint32_t ms) {
  sim_t *s = ctx;
  s->dhcp_at = s->now + ms;
}
static void op_cancel_dhcp_timer(void *ctx) { ((sim_t *)ctx)->dhcp_at = -1; }

static bool op_disconnect(void *ctx) {
  sim_t *s = ctx;
  if (s->disconnects < MAX_LOG) {
    s->disconnect_at[s->disconnects] = s->now;
  }
  s->disconnects++;
  if (s->disconnect_fails) {
    return false;
  }
  push(s, (q_event_t){.at = s->now + DROP_MS, .type = Q_DROPPED});
  return true;
}

static void op_set_ap(void *ctx, bool on, wl_ap_why_t why) {
  sim_t *s = ctx;
  s->ap_is_on = on;
  if (s->ap_log_len < MAX_LOG) {
    s->ap_log[s->ap_log_len++] = (ap_log_t){s->now, on, why};
  }
}

static bool op_dns_start(void *ctx) {
  sim_t *s = ctx;
  if (s->dns_running) {
    s->double_dns_start++;
  }
  s->dns_starts++;
  if (s->dns_fails) {
    return false;
  }
  s->dns_running = true;
  return true;
}

static void op_dns_stop(void *ctx) {
  sim_t *s = ctx;
  s->dns_stops++;
  s->dns_running = false;
}

static sim_t *sim_new(void) {
  sim_t *s = calloc(1, sizeof(*s));
  s->retry_at = -1;
  s->ap_timer_at = -1;
  s->dhcp_at = -1;
  s->scan_timer_at = -1;
  s->done_after_stop_ms = -1;
  s->scan_ms = SCAN_MS;
  s->attempt_ms = ATTEMPT_MS;
  s->dhcp_works = true;
  s->ops = (wl_ctl_ops_t){
      .ctx = s,
      .try_acquire = op_try_acquire,
      .release = op_release,
      .start_scan = op_start_scan,
      .stop_scan = op_stop_scan,
      .end_scan = op_end_scan,
      .arm_scan_timer = op_arm_scan_timer,
      .cancel_scan_timer = op_cancel_scan_timer,
      .connect = op_connect,
      .arm_retry = op_arm_retry,
      .cancel_retry = op_cancel_retry,
      .arm_ap_timer = op_arm_ap_timer,
      .cancel_ap_timer = op_cancel_ap_timer,
      .arm_dhcp_timer = op_arm_dhcp_timer,
      .cancel_dhcp_timer = op_cancel_dhcp_timer,
      .disconnect = op_disconnect,
      .set_ap = op_set_ap,
      .dns_start = op_dns_start,
      .dns_stop = op_dns_stop,
  };
  S = s;
  return s;
}

static void sim_save(sim_t *s, const char *ssid, const char *password,
                     bool right) {
  sim_saved_t *n = &s->saved[s->saved_len++];
  snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
  n->has_password = password[0] != '\0';
  n->password_right = right;
}

static world_ap_t *sim_ap(sim_t *s, const char *ssid, uint8_t id, int8_t rssi,
                          wl_sec_t sec) {
  world_ap_t *w = &s->world[s->world_len++];
  snprintf(w->ssid, sizeof(w->ssid), "%s", ssid);
  const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, id};
  memcpy(w->bssid, bssid, 6);
  w->channel = (uint8_t)(id % 11 + 1);
  w->rssi = rssi;
  w->sec = sec;
  w->up = true;
  return w;
}

static void sim_start(sim_t *s) {
  wl_ctl_init(&s->ctl, &s->ops, s->saved_len);
  wl_ctl_start(&s->ctl);
}

/* The glue's scan done: the visible world, the saved SSIDs. */
static void deliver_scan(sim_t *s) {
  static const uint8_t no_ssid[33];
  wl_ap_t aps[MAX_WORLD];
  size_t n = 0;
  for (size_t i = 0; i < s->world_len; i++) {
    const world_ap_t *w = &s->world[i];
    if (!w->up) {
      continue;
    }
    aps[n].ssid = w->hidden ? no_ssid : (const uint8_t *)w->ssid;
    aps[n].rssi = w->rssi;
    memcpy(aps[n].bssid, w->bssid, 6);
    aps[n].channel = w->channel;
    aps[n].sec = w->sec;
    n++;
  }
  wl_saved_t saved[WL_MAX_SAVED];
  for (size_t i = 0; i < s->saved_len; i++) {
    saved[i] = (wl_saved_t){s->saved[i].ssid, s->saved[i].has_password};
  }
  /* As the glue: none of ours runs: ignored; a stopped one: no list. */
  if (!s->scan_running) {
    return;
  }
  s->scan_running = false;
  if (wl_ctl_wants_scan(&s->ctl)) {
    wl_ctl_on_scan_done(&s->ctl, aps, n, saved, s->saved_len);
  } else {
    wl_ctl_on_scan_done(&s->ctl, NULL, 0, NULL, 0);
  }
}

static void deliver(sim_t *s, q_event_t e) {
  switch (e.type) {
    case Q_SCAN_DONE:
      deliver_scan(s);
      break;
    case Q_CONNECT_RESULT:
      if (e.ok) {
        wl_ctl_on_connected(&s->ctl, e.bssid, e.channel);
        if (s->dhcp_works) {
          push(s, (q_event_t){.at = s->now + DHCP_MS, .type = Q_GOT_IP});
        }
      } else {
        wl_ctl_on_disconnected(&s->ctl);
      }
      break;
    case Q_GOT_IP:
      if (s->ctl.associated) {
        wl_ctl_on_got_ip(&s->ctl);
      }
      break;
    case Q_DROPPED:
      wl_ctl_on_disconnected(&s->ctl);
      break;
  }
}

/* Runs everything due up to the time `until`, in time order. */
static void sim_run_until(sim_t *s, int64_t until) {
  for (int guard = 0; guard < 100000; guard++) {
    int64_t t = INT64_MAX;
    /* 0..: queue index; timers: -1 retry, -2 ap, -3 dhcp, -4 scan */
    int which = -1;
    for (size_t i = 0; i < s->queue_len; i++) {
      if (s->queue[i].at < t) {
        t = s->queue[i].at;
        which = (int)i;
      }
    }
    if (s->retry_at >= 0 && s->retry_at < t) {
      t = s->retry_at;
      which = -1;
    }
    if (s->ap_timer_at >= 0 && s->ap_timer_at < t) {
      t = s->ap_timer_at;
      which = -2;
    }
    if (s->dhcp_at >= 0 && s->dhcp_at < t) {
      t = s->dhcp_at;
      which = -3;
    }
    if (s->scan_timer_at >= 0 && s->scan_timer_at < t) {
      t = s->scan_timer_at;
      which = -4;
    }
    if (t > until) {
      break;
    }
    s->now = t;
    if (which == -1) {
      s->retry_at = -1;
      wl_ctl_on_retry_timer(&s->ctl);
    } else if (which == -2) {
      s->ap_timer_at = -1;
      wl_ctl_on_ap_timer(&s->ctl);
    } else if (which == -3) {
      s->dhcp_at = -1;
      wl_ctl_on_dhcp_timer(&s->ctl);
    } else if (which == -4) {
      s->scan_timer_at = -1;
      wl_ctl_on_scan_timer(&s->ctl);
    } else {
      q_event_t e = s->queue[which];
      memmove(&s->queue[which], &s->queue[which + 1],
              (s->queue_len - (size_t)which - 1) * sizeof(q_event_t));
      s->queue_len--;
      deliver(s, e);
    }
  }
  s->now = until;
}

static bool sim_connected(const sim_t *s) {
  return s->ctl.associated && s->ctl.has_ip;
}

/* The access point the station is on drops it (router off, ...). */
static void sim_drop_link(sim_t *s) {
  if (s->ctl.associated) {
    wl_ctl_on_disconnected(&s->ctl);
  }
}

static void check_invariants(const sim_t *s, int line) {
  if (s->double_dns_start != 0 || s->bad_release != 0) {
    check_failures++;
    fprintf(stderr, "line %d: dns double start %d, bad release %d\n", line,
            s->double_dns_start, s->bad_release);
  }
  check_count++;
  if (s->dns_running && !s->ap_is_on) {
    check_failures++;
    fprintf(stderr, "line %d: DNS runs without the access point\n", line);
  }
  check_count++;
  /* Idle and not waiting: the right is given back. */
  if (s->ctl.op == WL_OP_NONE && s->held) {
    check_failures++;
    fprintf(stderr, "line %d: right kept while idle\n", line);
  }
}

#define INVARIANTS(s) check_invariants((s), __LINE__)

/* Example 1: no saved network. */
static void test_example1_no_saved(void) {
  sim_t *s = sim_new();
  sim_ap(s, "Home", 1, -50, WL_SEC_PSK);
  sim_start(s);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log_len, 1);
  CHECK_INT(s->ap_log[0].at, 0);
  CHECK_INT(s->ap_log[0].why, WL_AP_WHY_NO_SAVED);
  CHECK(s->dns_running);
  CHECK_INT(s->dns_starts, 1);
  sim_run_until(s, 60 * 60 * 1000);
  CHECK_INT(s->scans_len, 0); /* station idle */
  CHECK_INT(s->attempts_len, 0);
  CHECK(!sim_connected(s));
  CHECK(s->ap_is_on && s->ap_log_len == 1);
  CHECK(s->retry_at < 0);
  /* Stray events do nothing. */
  wl_ctl_on_retry_timer(&s->ctl);
  wl_ctl_on_disconnected(&s->ctl);
  wl_ctl_on_lost_ip(&s->ctl);
  wl_ctl_on_ap_timer(&s->ctl);
  wl_ctl_on_resume(&s->ctl);
  wl_ctl_start(&s->ctl);
  CHECK_INT(s->scans_len, 0);
  CHECK_INT(s->attempts_len, 0);
  CHECK_INT(s->dns_starts, 1);
  INVARIANTS(s);
  free(s);
}

/* Example 2: saved Home in range. */
static void test_example2_home(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  sim_start(s);
  CHECK(!s->ap_is_on);
  CHECK_INT(s->scans_len, 1);
  CHECK_INT(s->ap_timer_at, 30000);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  CHECK_INT(s->attempts_len, 1);
  CHECK(s->attempts[0].target.by_bssid);
  CHECK_INT(s->attempts[0].target.bssid[5], 1);
  CHECK_INT(s->ap_timer_at, -1);
  sim_run_until(s, 10 * 60 * 1000);
  CHECK_INT(s->ap_log_len, 0); /* never on */
  CHECK_INT(s->dns_starts, 0);
  CHECK_INT(s->scans_len, 1);
  INVARIANTS(s);
  free(s);
}

/* Example 3: Home -70 and Home-2G -50. */
static void test_example3_strongest_network(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_save(s, "Home-2G", "secretpw", true);
  sim_ap(s, "Home", 1, -70, WL_SEC_PSK);
  sim_ap(s, "Home-2G", 2, -50, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  CHECK_INT(s->attempts_len, 1);
  CHECK_INT(s->attempts[0].target.saved_index, 1);
  CHECK_INT(s->attempts[0].target.bssid[5], 2);
  INVARIANTS(s);
  free(s);
}

/* Example 4: mesh Home, nodes -45 and -60; no roaming later. */
static void test_example4_mesh_no_roaming(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  world_ap_t *weak = sim_ap(s, "Home", 1, -60, WL_SEC_PSK);
  sim_ap(s, "Home", 2, -45, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  CHECK_INT(s->attempts[0].target.bssid[5], 2);
  CHECK_INT(s->attempts[0].target.channel, 3);
  weak->rssi = -20; /* the other node is stronger now */
  sim_run_until(s, 60 * 60 * 1000);
  CHECK_INT(s->attempts_len, 1);
  CHECK_INT(s->scans_len, 1);
  CHECK(sim_connected(s));
  INVARIANTS(s);
  free(s);
}

/* Example 5: saved network out of range at start-up, comes back later. */
static void test_example5_out_of_range(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  world_ap_t *home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  sim_start(s);
  sim_run_until(s, 29999);
  CHECK(!s->ap_is_on);
  sim_run_until(s, 30000);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 30000);
  CHECK_INT(s->ap_log[0].why, WL_AP_WHY_NO_ADDRESS);
  CHECK(s->dns_running);
  /* Not seen in the scan: tried by SSID alone. */
  CHECK(s->attempts_len >= 1);
  CHECK(!s->attempts[0].target.by_bssid);
  CHECK_INT(s->attempts[0].target.saved_index, 0);
  INVARIANTS(s);

  sim_run_until(s, 120000);
  home->up = true;
  sim_run_until(s, 120000 + 30000 + SCAN_MS + ATTEMPT_MS + DHCP_MS);
  CHECK(sim_connected(s));
  CHECK(!s->ap_is_on);
  CHECK(!s->dns_running);
  CHECK_INT(s->dns_starts, 1);
  CHECK_INT(s->dns_stops, 1);
  CHECK_INT(s->ap_log_len, 2);
  CHECK(!s->ap_log[1].on);
  INVARIANTS(s);
  free(s);
}

/* Example 6: the router restarts, gone for 90 s. */
static void test_example6_router_restart(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  world_ap_t *home = sim_ap(s, "Home", 7, -55, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 10000);
  CHECK(sim_connected(s));
  size_t before = s->attempts_len;
  size_t scans_before = s->scans_len;

  const int64_t t0 = 10000;
  home->up = false;
  sim_drop_link(s);
  CHECK(!sim_connected(s));
  sim_run_until(s, t0 + 90000);

  /* Six attempts on the same BSSID; each starts 0, 1, 2, 4, 8, 16 s after
   * the end of the one before (the first at once). */
  const int64_t gap_ms[6] = {0, 1000, 2000, 4000, 8000, 16000};
  int64_t expect = t0;
  for (size_t i = 0; i < 6; i++) {
    const attempt_log_t *a = &s->attempts[before + i];
    expect += gap_ms[i];
    CHECK_INT(a->at, expect);
    CHECK(a->target.by_bssid);
    CHECK_INT(a->target.bssid[5], 7);
    CHECK_INT(a->target.channel, 8);
    expect += ATTEMPT_MS;
  }
  /* Then a full choice (scan) 30 s later; nothing seen, so one round by
   * SSID alone. */
  CHECK_INT(s->scans_len, scans_before + 1);
  const int64_t scan1 = expect + 30000;
  CHECK_INT(s->scans[scans_before], scan1);
  CHECK_INT(s->attempts_len, before + 7);
  CHECK(!s->attempts[before + 6].target.by_bssid);
  CHECK_INT(s->attempts[before + 6].at, scan1 + SCAN_MS);
  /* Access point on 30 s after the address was lost. */
  CHECK_INT(s->ap_log_len, 1);
  CHECK_INT(s->ap_log[0].at, t0 + 30000);
  CHECK(s->dns_running);

  /* Back at t0 + 90 s; the next scan, 30 s after the round failed, finds
   * it. */
  home->up = true;
  const int64_t scan2 = scan1 + SCAN_MS + ATTEMPT_MS + 30000;
  sim_run_until(s, scan2 + SCAN_MS + ATTEMPT_MS + DHCP_MS);
  CHECK_INT(s->scans_len, scans_before + 2);
  CHECK_INT(s->scans[scans_before + 1], scan2);
  CHECK(sim_connected(s));
  CHECK(!s->ap_is_on);
  CHECK(!s->dns_running);
  /* The wait starts at 0 again: another drop is retried at once. */
  int64_t t1 = s->now;
  size_t n = s->attempts_len;
  sim_drop_link(s);
  CHECK_INT(s->attempts_len, n); /* "at once" comes as its own event */
  CHECK_INT(s->retry_at, t1);
  sim_run_until(s, t1);
  CHECK_INT(s->attempts_len, n + 1);
  CHECK_INT(s->attempts[n].at, t1);
  INVARIANTS(s);
  free(s);
}

/* Example 7: wrong saved password. */
static void test_example7_wrong_password(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "wrongpass", false);
  sim_ap(s, "Home", 3, -50, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 200000);
  CHECK(!sim_connected(s));
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 30000);
  CHECK(s->dns_running);
  /* Attempt starts: after the scan, then gaps 0, 1, 2, 4, 8, 16 s. */
  int64_t expect = SCAN_MS;
  const int64_t gap_ms[7] = {0, 0, 1000, 2000, 4000, 8000, 16000};
  for (size_t i = 0; i < 7; i++) {
    expect += gap_ms[i];
    CHECK_INT(s->attempts[i].at, expect);
    CHECK(s->attempts[i].target.by_bssid);
    expect += ATTEMPT_MS;
  }
  /* The 7th failure: scan after 30 s, and so on. */
  CHECK_INT(s->scans_len >= 2, 1);
  CHECK_INT(s->scans[1], expect + 30000);
  INVARIANTS(s);
  free(s);
}

/* Example 9: hidden saved network Attic, among other hidden ones. */
static void test_example9_hidden(void) {
  sim_t *s = sim_new();
  sim_save(s, "Cellar", "secretpw", true);
  sim_save(s, "Barn", "", true);
  sim_save(s, "Attic", "secretpw", true);
  world_ap_t *attic = sim_ap(s, "Attic", 9, -60, WL_SEC_PSK);
  attic->hidden = true;
  sim_ap(s, "Neighbour", 4, -40, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 10000);
  CHECK(sim_connected(s));
  /* A round by SSID alone, one after the other without waiting. */
  CHECK_INT(s->attempts_len, 3);
  for (size_t i = 0; i < 3; i++) {
    CHECK(!s->attempts[i].target.by_bssid);
    CHECK_INT(s->attempts[i].target.saved_index, i);
    CHECK_INT(s->attempts[i].at, SCAN_MS + (int64_t)i * ATTEMPT_MS);
  }
  /* After a drop "the same access point" is the BSSID it joined. */
  sim_drop_link(s);
  sim_run_until(s, s->now);
  CHECK_INT(s->attempts_len, 4);
  CHECK(s->attempts[3].target.by_bssid);
  CHECK_INT(s->attempts[3].target.bssid[5], 9);
  CHECK_INT(s->attempts[3].target.saved_index, 2);
  INVARIANTS(s);
  free(s);
}

/* 8 saved networks, none in range: the whole round is one failure. */
static void test_eight_saved_round(void) {
  sim_t *s = sim_new();
  char name[8];
  for (int i = 0; i < 8; i++) {
    snprintf(name, sizeof(name), "net%d", i);
    sim_save(s, name, "secretpw", true);
  }
  sim_start(s);
  sim_run_until(s, SCAN_MS + 8 * ATTEMPT_MS - 1);
  CHECK_INT(s->attempts_len, 8);
  for (size_t i = 0; i < 8; i++) {
    CHECK_INT(s->attempts[i].target.saved_index, i);
  }
  /* Failure 1: the round again at once. */
  sim_run_until(s, SCAN_MS + 16 * ATTEMPT_MS);
  CHECK_INT(s->attempts_len, 16);
  CHECK_INT(s->attempts[8].target.saved_index, 0);
  CHECK_INT(s->attempts[8].at, SCAN_MS + 8 * ATTEMPT_MS);
  /* Failure 2: 1 s later. */
  sim_run_until(s, SCAN_MS + 16 * ATTEMPT_MS + 999);
  CHECK_INT(s->attempts_len, 16);
  sim_run_until(s, SCAN_MS + 16 * ATTEMPT_MS + 1000);
  CHECK_INT(s->attempts_len, 17);
  INVARIANTS(s);
  free(s);
}

/* A round of 8 networks whose attempts can't even start: all 8 at once in
 * a loop on the same stack depth (no recursion on the event loop task's
 * small stack), one failure, and "at once" comes as a new event. */
static void test_round_no_recursion(void) {
  sim_t *s = sim_new();
  char name[8];
  for (int i = 0; i < 8; i++) {
    snprintf(name, sizeof(name), "net%d", i);
    sim_save(s, name, "secretpw", true);
  }
  s->connect_start_fails = true;
  sim_start(s);
  sim_run_until(s, SCAN_MS);
  CHECK_INT(s->attempts_len, 8 + 8); /* the round, then failure 1: again */
  CHECK_INT(s->ctl.failures, 2);
  CHECK_INT(s->retry_at, SCAN_MS + 1000);
  /* Each round on one stack depth (recursion would add a frame per
   * network). */
  for (size_t round = 0; round < 2; round++) {
    for (size_t i = 1; i < 8; i++) {
      CHECK_INT(s->attempts[round * 8 + i].sp, s->attempts[round * 8].sp);
    }
  }
  INVARIANTS(s);
  free(s);
}

/* The saved password "" joins only an open network; a password never an
 * open one. */
static void test_security_choice(void) {
  sim_t *s = sim_new();
  sim_save(s, "Cafe", "secretpw", true);
  sim_ap(s, "Cafe", 1, -30, WL_SEC_OPEN);
  sim_ap(s, "Cafe", 2, -60, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  CHECK_INT(s->attempts[0].target.bssid[5], 2);
  free(s);

  s = sim_new();
  sim_save(s, "Cafe", "", true);
  sim_ap(s, "Cafe", 1, -60, WL_SEC_OPEN);
  sim_ap(s, "Cafe", 2, -30, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  CHECK_INT(s->attempts[0].target.bssid[5], 1);
  free(s);

  /* Only the wrong kind in range: by SSID alone, which fails. */
  s = sim_new();
  sim_save(s, "Cafe", "secretpw", true);
  sim_ap(s, "Cafe", 1, -30, WL_SEC_OPEN);
  sim_start(s);
  sim_run_until(s, 5000);
  CHECK(!sim_connected(s));
  CHECK(s->attempts_len >= 1 && !s->attempts[0].target.by_bssid);
  INVARIANTS(s);
  free(s);
}

/* A due attempt waits for wifi_scan(), then goes. */
static void test_busy_user_scan(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  world_ap_t *home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  size_t n = s->attempts_len;

  /* wifi_scan() holds the right while the link drops. */
  CHECK(op_try_acquire(s));
  s->held = false;
  s->user_busy = true;
  home->up = false;
  sim_drop_link(s);
  sim_run_until(s, s->now);
  CHECK_INT(s->attempts_len, n); /* waits */
  CHECK_INT(s->retry_at, 5000 + WL_BUSY_POLL_MS);
  s->now = 5500;
  s->user_busy = false;
  wl_ctl_on_resume(&s->ctl);
  CHECK_INT(s->attempts_len, n + 1);
  CHECK_INT(s->attempts[n].at, 5500);
  CHECK_INT(s->retry_at, -1); /* the fallback poll is gone */
  sim_run_until(s, 5500 + ATTEMPT_MS); /* fails */
  CHECK_INT(s->retry_at, 5500 + ATTEMPT_MS + 1000);

  /* Without the resume event: the poll takes over. */
  s->user_busy = true;
  sim_run_until(s, 5500 + ATTEMPT_MS + 1000);
  CHECK_INT(s->attempts_len, n + 1);
  CHECK_INT(s->retry_at, 5500 + ATTEMPT_MS + 1000 + WL_BUSY_POLL_MS);
  s->user_busy = false;
  sim_run_until(s, 5500 + ATTEMPT_MS + 1000 + WL_BUSY_POLL_MS);
  CHECK_INT(s->attempts_len, n + 2);
  /* A resume with nothing due does nothing. */
  sim_run_until(s, s->now + ATTEMPT_MS);
  size_t m = s->attempts_len;
  wl_ctl_on_resume(&s->ctl);
  CHECK_INT(s->attempts_len, m);
  INVARIANTS(s);
  free(s);
}

/* A captive DNS that does not start: the access point stays on without it,
 * and there is no stop for it. */
static void test_dns_fails(void) {
  sim_t *s = sim_new();
  s->dns_fails = true;
  sim_save(s, "Home", "secretpw", true);
  world_ap_t *home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  sim_start(s);
  sim_run_until(s, 31000);
  CHECK(s->ap_is_on);
  CHECK_INT(s->dns_starts, 1);
  CHECK(!s->dns_running);
  home->up = true;
  sim_run_until(s, 200000);
  CHECK(sim_connected(s));
  CHECK(!s->ap_is_on);
  CHECK_INT(s->dns_stops, 0);
  INVARIANTS(s);
  free(s);
}

/* IP_EVENT_STA_LOST_IP while still associated: 20 s later the station
 * drops the link (a failure); the access point 30 s after the loss. */
static void test_lost_ip(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  CHECK_INT(s->dhcp_at, -1);
  wl_ctl_on_lost_ip(&s->ctl);
  CHECK(!sim_connected(s));
  CHECK_INT(s->ap_timer_at, 35000);
  CHECK_INT(s->dhcp_at, 25000);
  CHECK_INT(s->attempts_len, 1); /* still associated: no attempt */
  wl_ctl_on_lost_ip(&s->ctl);    /* twice: nothing new */
  CHECK_INT(s->dhcp_at, 25000);

  s->dhcp_works = false; /* the router gives no address any more */
  sim_run_until(s, 25000);
  CHECK_INT(s->disconnects, 1);
  CHECK_INT(s->disconnect_at[0], 25000);
  CHECK_INT(s->attempts_len, 1);
  sim_run_until(s, 25000 + DROP_MS);
  /* Failure 1: the same BSSID at once. */
  CHECK_INT(s->attempts_len, 2);
  CHECK_INT(s->attempts[1].at, 25000 + DROP_MS);
  CHECK(s->attempts[1].target.by_bssid);
  CHECK_INT(s->ctl.failures, 1);
  /* Joined again, no address: the access point at 30 s after the loss. */
  sim_run_until(s, 35000);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 35000);
  /* 20 s after that join: dropped again, failure 2, 1 s wait. */
  const int64_t join2 = 25000 + DROP_MS + ATTEMPT_MS;
  sim_run_until(s, join2 + 20000 + DROP_MS);
  CHECK_INT(s->disconnects, 2);
  CHECK_INT(s->disconnect_at[1], join2 + 20000);
  CHECK_INT(s->ctl.failures, 2);
  CHECK_INT(s->retry_at, join2 + 20000 + DROP_MS + 1000);
  INVARIANTS(s);
  free(s);

  /* The address comes back within 20 s: nothing is dropped. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  wl_ctl_on_lost_ip(&s->ctl);
  sim_run_until(s, 15000);
  wl_ctl_on_got_ip(&s->ctl);
  CHECK_INT(s->dhcp_at, -1);
  CHECK_INT(s->ap_timer_at, -1);
  sim_run_until(s, 100000);
  CHECK_INT(s->disconnects, 0);
  CHECK(sim_connected(s) && !s->ap_is_on);

  /* LOST_IP after the link is gone: no DHCP timer. */
  sim_drop_link(s);
  wl_ctl_on_lost_ip(&s->ctl);
  CHECK(s->dhcp_at < 0 || s->ctl.associated);
  INVARIANTS(s);
  free(s);

  /* disconnect() fails: counted as disconnected at once. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->dhcp_works = false;
  s->disconnect_fails = true;
  sim_start(s);
  sim_run_until(s, SCAN_MS + ATTEMPT_MS + 20000);
  CHECK_INT(s->disconnects, 1);
  CHECK(!s->ctl.associated);
  CHECK_INT(s->ctl.failures, 1);
  CHECK_INT(s->attempts_len, 2); /* failure 1: again at once */
  INVARIANTS(s);
  free(s);
}

/* Example 11: associated, DHCP gives no address. */
static void test_example11_no_dhcp(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->dhcp_works = false;
  sim_start(s);
  const int64_t joined = SCAN_MS + ATTEMPT_MS;
  sim_run_until(s, joined);
  CHECK(s->ctl.associated && !s->ctl.has_ip);
  CHECK_INT(s->dhcp_at, joined + 20000);
  sim_run_until(s, joined + 20000 - 1);
  CHECK_INT(s->disconnects, 0);
  sim_run_until(s, joined + 20000);
  CHECK_INT(s->disconnects, 1);
  sim_run_until(s, joined + 20000 + DROP_MS);
  CHECK(!s->ctl.associated);
  CHECK_INT(s->ctl.failures, 1);
  CHECK_INT(s->attempts_len, 2);
  CHECK_INT(s->attempts[1].at, joined + 20000 + DROP_MS);
  CHECK_INT(s->attempts[1].target.bssid[5], 1);
  /* The wait still counts up: failure 2 waits 1 s. */
  const int64_t joined2 = joined + 20000 + DROP_MS + ATTEMPT_MS;
  sim_run_until(s, joined2 + 20000 + DROP_MS);
  CHECK_INT(s->ctl.failures, 2);
  CHECK_INT(s->retry_at, joined2 + 20000 + DROP_MS + 1000);
  CHECK(s->ap_is_on); /* no address for 30 s */
  INVARIANTS(s);
  free(s);
}

/* Example 12: a phone on the setup access point while the saved network is
 * away: full choices every 60 s; 30 s again once it has left. */
static void test_example12_client_60s(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  world_ap_t *home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  sim_start(s);
  sim_run_until(s, 30000);
  CHECK(s->ap_is_on);
  sim_run_until(s, 31000);
  wl_ctl_on_ap_clients(&s->ctl, 1); /* the phone joins */
  sim_run_until(s, 400000);
  CHECK(s->scans_len >= 4);
  /* scans[0] at start-up, scans[1] after the 7th failure. */
  for (size_t i = 2; i < s->scans_len; i++) {
    CHECK_INT(s->scans[i] - s->scans[i - 1], SCAN_MS + ATTEMPT_MS + 60000);
  }
  /* The retry times before the full choice do not change. */
  const int64_t gap_ms[7] = {0, 0, 1000, 2000, 4000, 8000, 16000};
  int64_t expect = SCAN_MS;
  for (size_t i = 0; i < 7; i++) {
    expect += gap_ms[i];
    CHECK_INT(s->attempts[i].at, expect);
    expect += ATTEMPT_MS;
  }
  CHECK_INT(s->scans[1], expect + 60000);

  wl_ctl_on_ap_clients(&s->ctl, 0); /* the phone leaves */
  size_t n = s->scans_len;
  sim_run_until(s, s->scans[n - 1] + 200000);
  CHECK(s->scans_len >= n + 2);
  /* The wait already armed stays; then 30 s again. */
  CHECK_INT(s->scans[n + 1] - s->scans[n], SCAN_MS + ATTEMPT_MS + 30000);
  INVARIANTS(s);
  free(s);

  /* Without clients: 30 s. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  sim_start(s);
  sim_run_until(s, 400000);
  for (size_t i = 2; i < s->scans_len; i++) {
    CHECK_INT(s->scans[i] - s->scans[i - 1], SCAN_MS + ATTEMPT_MS + 30000);
  }
  free(s);

  /* Clients while the access point is off count as none. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  sim_start(s);
  sim_run_until(s, 5000);
  wl_ctl_on_ap_clients(&s->ctl, 3);
  CHECK_INT(s->ctl.ap_clients, 0);
  free(s);
}

/* The access point never turns on in the middle of an attempt or a scan:
 * right after it ends, before anything else starts. */
static void test_ap_waits_for_op(void) {
  /* An attempt runs at 30 s (attempts of 7 s: 26.5 s to 33.5 s). */
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  world_ap_t *home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  s->attempt_ms = 7000;
  sim_start(s);
  sim_run_until(s, 30000);
  CHECK(!s->ap_is_on);
  CHECK(s->ctl.op == WL_OP_CONNECT);
  sim_run_until(s, 33499);
  CHECK(!s->ap_is_on);
  sim_run_until(s, 33500);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 33500);
  CHECK(s->dns_running);
  CHECK_INT(s->attempts_len, 4); /* the next one only after */
  INVARIANTS(s);
  free(s);

  /* A scan runs at 30 s: the start-up scan takes 31 s here. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  s->scan_ms = 31000;
  sim_start(s);
  sim_run_until(s, 30000);
  CHECK(s->ctl.op == WL_OP_SCAN);
  CHECK(!s->ap_is_on);
  sim_run_until(s, 31000);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 31000);
  CHECK_INT(s->attempts_len, 1); /* the round started after the access point */
  CHECK_INT(s->attempts[0].at, 31000);
  INVARIANTS(s);
  free(s);

  /* wifi_scan() runs at 30 s: on right after it gives the right back. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  sim_start(s);
  sim_run_until(s, 29000);
  CHECK(s->ctl.op == WL_OP_NONE);
  s->user_busy = true;
  sim_run_until(s, 30000);
  CHECK(!s->ap_is_on);
  CHECK_INT(s->ap_timer_at, 30000 + WL_BUSY_POLL_MS);
  s->now = 30400;
  s->user_busy = false;
  wl_ctl_on_resume(&s->ctl);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 30400);
  CHECK(!s->held);
  free(s);

  /* Without the resume event the poll does it. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  home = sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  home->up = false;
  sim_start(s);
  sim_run_until(s, 29000);
  s->user_busy = true;
  sim_run_until(s, 30500);
  s->user_busy = false;
  sim_run_until(s, 31000);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 31000);
  INVARIANTS(s);
  free(s);
}

/* The station's own scan: stopped after 10 s, then its SCAN_DONE or 1 s
 * more; it counts as a failure. */
static void test_station_scan_limit(void) {
  /* No SCAN_DONE at all. */
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->scan_hangs = true;
  sim_start(s);
  CHECK_INT(s->scan_timer_at, WL_SCAN_TIMEOUT_MS);
  sim_run_until(s, 9999);
  CHECK_INT(s->scan_stops, 0);
  sim_run_until(s, 10000);
  CHECK_INT(s->scan_stops, 1);
  CHECK_INT(s->scan_stop_at[0], 10000);
  CHECK(s->ctl.op == WL_OP_SCAN_STOPPING);
  CHECK(s->held); /* keeps the right during the 1 s */
  sim_run_until(s, 10999);
  CHECK_INT(s->scan_ends, 0);
  CHECK_INT(s->scans_len, 1);
  sim_run_until(s, 11000);
  CHECK_INT(s->scan_ends, 1);
  CHECK_INT(s->ctl.failures, 1);
  /* Failure 1, no target yet: a full choice again at once. */
  CHECK_INT(s->scans_len, 2);
  CHECK_INT(s->scans[1], 11000);
  /* The second scan was started hanging too: stopped at 21 s, over at
   * 22 s. The third one works. */
  s->scan_hangs = false;
  sim_run_until(s, 21999);
  CHECK_INT(s->ctl.failures, 1);
  sim_run_until(s, 22000);
  CHECK_INT(s->ctl.failures, 2);
  CHECK_INT(s->retry_at, 23000); /* failure 2: 1 s */
  sim_run_until(s, 23000 + SCAN_MS + ATTEMPT_MS + DHCP_MS);
  CHECK(sim_connected(s));
  CHECK_INT(s->scan_timer_at, -1); /* cancelled by the SCAN_DONE */
  INVARIANTS(s);
  free(s);

  /* The SCAN_DONE comes 300 ms after the stop: no list, a failure. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->scan_hangs = true;
  s->done_after_stop_ms = 300;
  sim_start(s);
  sim_run_until(s, 10300);
  CHECK_INT(s->scan_ends, 1);
  CHECK_INT(s->ctl.failures, 1);
  CHECK_INT(s->attempts_len, 0); /* the partial list is not used */
  CHECK_INT(s->scans_len, 2);
  CHECK_INT(s->scans[1], 10300);
  CHECK_INT(s->scan_timer_at, 10300 + WL_SCAN_TIMEOUT_MS);
  INVARIANTS(s);
  free(s);

  /* A SCAN_DONE 1.5 s after the stop: the next scan of ours runs, so it is
   * taken as that scan's (accepted, see README.md); the next scan's own
   * SCAN_DONE then comes while none runs and is ignored. */
  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->scan_hangs = true;
  s->done_after_stop_ms = 1500;
  sim_start(s);
  sim_run_until(s, 11000);
  CHECK_INT(s->scans_len, 2);
  s->scan_hangs = false; /* nothing queued for scan 2 yet: it hangs */
  sim_run_until(s, 11500);
  CHECK_INT(s->attempts_len, 1); /* scan 2 "ended" with the late list */
  CHECK_INT(s->attempts[0].at, 11500);
  CHECK_INT(s->scan_timer_at, -1);
  sim_run_until(s, 20000);
  CHECK(sim_connected(s));
  INVARIANTS(s);
  free(s);

  /* No saved network seen and a stop: still one failure, no round. */
  s = sim_new();
  sim_save(s, "Attic", "secretpw", true);
  s->scan_hangs = true;
  sim_start(s);
  sim_run_until(s, 11000);
  CHECK_INT(s->attempts_len, 0);
  CHECK_INT(s->ctl.failures, 1);
  free(s);
}

/* The access point that comes due while a stopped scan waits its 1 s turns
 * on when it is over, before the next scan. */
static void test_ap_waits_for_stopped_scan(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->scan_hangs = true;
  sim_start(s);
  /* Scans at 0, 11 s and 23 s, each stopped after 10 s and over 1 s
   * later. */
  sim_run_until(s, 29999);
  CHECK(!s->ap_is_on);
  sim_run_until(s, 30000);
  CHECK(s->ctl.op == WL_OP_SCAN || s->ctl.op == WL_OP_SCAN_STOPPING);
  CHECK(!s->ap_is_on);
  /* Third scan from 23 s (failure 2 waits 1 s): stopped 33 s, over 34 s. */
  CHECK_INT(s->scans[2], 23000);
  sim_run_until(s, 33999);
  CHECK(!s->ap_is_on);
  CHECK(s->ctl.op == WL_OP_SCAN_STOPPING);
  sim_run_until(s, 34000);
  CHECK(s->ap_is_on);
  CHECK_INT(s->ap_log[0].at, 34000);
  INVARIANTS(s);
  free(s);
}

/* Starting a scan or an attempt fails at once. */
static void test_start_failures(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->scan_start_fails = true;
  sim_start(s);
  /* failure 1: at once (no target yet: scan), failure 2: 1 s */
  CHECK_INT(s->retry_at, 0);
  sim_run_until(s, 0);
  CHECK_INT(s->retry_at, 1000);
  CHECK_INT(s->scans_len, 0);
  s->scan_start_fails = false;
  sim_run_until(s, 10000);
  CHECK(sim_connected(s));
  free(s);

  s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  s->connect_start_fails = true;
  sim_start(s);
  sim_run_until(s, SCAN_MS);
  CHECK_INT(s->attempts_len, 2); /* one, then at once again */
  CHECK_INT(s->retry_at, SCAN_MS + 1000);
  s->connect_start_fails = false;
  sim_run_until(s, 10000);
  CHECK(sim_connected(s));
  INVARIANTS(s);
  free(s);
}

/* Events that do not fit the state are ignored. */
static void test_stray_events(void) {
  sim_t *s = sim_new();
  sim_save(s, "Home", "secretpw", true);
  sim_ap(s, "Home", 1, -55, WL_SEC_PSK);
  wl_ctl_init(&s->ctl, &s->ops, s->saved_len);
  /* Before the start. */
  wl_ctl_on_retry_timer(&s->ctl);
  wl_ctl_on_ap_timer(&s->ctl);
  wl_ctl_on_disconnected(&s->ctl);
  wl_ctl_on_scan_done(&s->ctl, NULL, 0, NULL, 0);
  CHECK_INT(s->scans_len + s->attempts_len + s->ap_log_len, 0);
  wl_ctl_start(&s->ctl);
  wl_ctl_start(&s->ctl);
  CHECK_INT(s->scans_len, 1);
  sim_run_until(s, 5000);
  CHECK(sim_connected(s));
  /* A scan done that is not ours, a retry timer while connected. */
  wl_ctl_on_scan_done(&s->ctl, NULL, 0, NULL, 0);
  wl_ctl_on_retry_timer(&s->ctl);
  wl_ctl_on_ap_timer(&s->ctl);
  CHECK_INT(s->attempts_len, 1);
  CHECK(!s->ap_is_on);
  /* More than 8 saved networks are cut to 8. */
  wl_ctl_t c;
  wl_ctl_init(&c, &s->ops, 100);
  CHECK_INT(c.saved_count, WL_MAX_SAVED);
  INVARIANTS(s);
  free(s);
}

void test_ctl(void) {
  test_example1_no_saved();
  test_example2_home();
  test_example3_strongest_network();
  test_example4_mesh_no_roaming();
  test_example5_out_of_range();
  test_example6_router_restart();
  test_example7_wrong_password();
  test_example9_hidden();
  test_eight_saved_round();
  test_round_no_recursion();
  test_security_choice();
  test_busy_user_scan();
  test_dns_fails();
  test_lost_ip();
  test_example11_no_dhcp();
  test_example12_client_60s();
  test_ap_waits_for_op();
  test_station_scan_limit();
  test_ap_waits_for_stopped_scan();
  test_start_failures();
  test_stray_events();
}
