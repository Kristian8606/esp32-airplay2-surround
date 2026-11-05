/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Host tests of src/wifi_saved.c (the saved networks, read once at start)
 * with fake settings_* and a log that keeps every line. */
#include <stdarg.h>
#include <stdlib.h>

#include "check.h"
#include "esp_log.h"
#include "wifi_ctl.h"
#include "wifi_saved.h"

/* ---- Fake settings and log -------------------------------------------- */

#define FAKE_MAX 16

static struct {
  size_t count; /* what settings_get_wifi_network_count() says */
  struct {
    esp_err_t err;
    settings_wifi_network_t net; /* copied as raw bytes, NULs or not */
  } entry[FAKE_MAX];
  int reads;
} fake;

static char log_lines[64][256];
static int log_count;
static int warn_count;

static void fake_reset(void) {
  memset(&fake, 0, sizeof(fake));
  log_count = 0;
  warn_count = 0;
}

static void fake_add(const char *ssid, const char *password) {
  size_t i = fake.count++;
  snprintf(fake.entry[i].net.ssid, sizeof(fake.entry[i].net.ssid), "%s",
           ssid);
  snprintf(fake.entry[i].net.password, sizeof(fake.entry[i].net.password),
           "%s", password);
}

size_t settings_get_wifi_network_count(void) { return fake.count; }

esp_err_t settings_get_wifi_network(size_t index,
                                    settings_wifi_network_t *network) {
  fake.reads++;
  if (index >= FAKE_MAX) {
    return ESP_ERR_NOT_FOUND;
  }
  /* A failing read may still have written into the buffer. */
  memcpy(network, &fake.entry[index].net, sizeof(*network));
  return fake.entry[index].err;
}

const char *esp_err_to_name(esp_err_t code) {
  switch (code) {
    case ESP_ERR_NOT_FOUND:
      return "ESP_ERR_NOT_FOUND";
    case ESP_FAIL:
      return "ESP_FAIL";
    default:
      return "ERROR";
  }
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *fmt,
                   ...) {
  CHECK_STR(tag, "wifi");
  if (level == ESP_LOG_WARN) {
    warn_count++;
  }
  if (log_count < 64) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_lines[log_count++], sizeof(log_lines[0]), fmt, ap);
    va_end(ap);
  }
}

static bool logged(const char *part) {
  for (int i = 0; i < log_count; i++) {
    if (strstr(log_lines[i], part) != NULL) {
      return true;
    }
  }
  return false;
}

/* ---- The state machine at start, as wifi_init_apsta() starts it ------- */

static struct {
  int ap_on, scans, attempts, dns_starts, ap_timer_arms;
  wl_ap_why_t why;
} seen;

static bool t_true(void *ctx) {
  (void)ctx;
  return true;
}
static void t_none(void *ctx) { (void)ctx; }
static void t_ms(void *ctx, uint32_t ms) {
  (void)ctx;
  (void)ms;
}
static void t_ap_timer(void *ctx, uint32_t ms) {
  (void)ctx;
  (void)ms;
  seen.ap_timer_arms++;
}
static bool t_scan(void *ctx) {
  (void)ctx;
  seen.scans++;
  return true;
}
static bool t_connect(void *ctx, const wl_target_t *t) {
  (void)ctx;
  (void)t;
  seen.attempts++;
  return true;
}
static void t_set_ap(void *ctx, bool on, wl_ap_why_t why) {
  (void)ctx;
  if (on) {
    seen.ap_on++;
    seen.why = why;
  }
}
static bool t_dns(void *ctx) {
  (void)ctx;
  seen.dns_starts++;
  return true;
}

static const wl_ctl_ops_t t_ops = {
    .try_acquire = t_true,
    .release = t_none,
    .start_scan = t_scan,
    .stop_scan = t_none,
    .end_scan = t_none,
    .arm_scan_timer = t_ms,
    .cancel_scan_timer = t_none,
    .connect = t_connect,
    .arm_retry = t_ms,
    .cancel_retry = t_none,
    .arm_ap_timer = t_ap_timer,
    .cancel_ap_timer = t_none,
    .arm_dhcp_timer = t_ms,
    .cancel_dhcp_timer = t_none,
    .disconnect = t_true,
    .set_ap = t_set_ap,
    .dns_start = t_dns,
    .dns_stop = t_none,
};

/* Loads the fake settings and starts the state machine with the result. */
static size_t start_with_fake(settings_wifi_network_t *net,
                              wl_saved_t *saved) {
  memset(&seen, 0, sizeof(seen));
  size_t n = wifi_saved_load(net, saved);
  wl_ctl_t ctl;
  wl_ctl_init(&ctl, &t_ops, n);
  wl_ctl_start(&ctl);
  return n;
}

/* ---- Tests ------------------------------------------------------------ */

static void test_none_saved(void) {
  settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS];
  wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  fake_reset();
  CHECK_INT(start_with_fake(net, saved), 0);
  CHECK_INT(fake.reads, 0);
  CHECK_INT(warn_count, 0);
  CHECK_INT(seen.ap_on, 1);
  CHECK_INT(seen.why, WL_AP_WHY_NO_SAVED);
  CHECK_INT(seen.scans, 0);
}

static void test_all_read(void) {
  settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS];
  wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  fake_reset();
  fake_add("Home", "secretpw1");
  fake_add("Cafe", "");
  fake_add("Attic", "secretpw3");
  CHECK_INT(start_with_fake(net, saved), 3);
  CHECK_INT(warn_count, 0);
  CHECK_STR(saved[0].ssid, "Home");
  CHECK_STR(saved[1].ssid, "Cafe");
  CHECK_STR(saved[2].ssid, "Attic");
  CHECK(saved[0].ssid == net[0].ssid);
  CHECK(saved[0].has_password && !saved[1].has_password &&
        saved[2].has_password);
  CHECK_STR(net[2].password, "secretpw3");
  CHECK(saved[3].ssid == NULL);
  /* Saved networks: no access point at start, a scan. */
  CHECK_INT(seen.ap_on, 0);
  CHECK_INT(seen.scans, 1);
  CHECK_INT(seen.ap_timer_arms, 1);
}

/* Some can't be read: left out with a warning, the others keep their
 * order, and nothing of the bad ones (password) stays or is logged. */
static void test_some_fail(void) {
  settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS];
  wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  fake_reset();
  fake_add("n0", "password0");
  fake_add("n1", "password1");
  fake_add("", "password2"); /* empty SSID */
  fake_add("n3", "password3");
  fake_add("n4", "password4");
  fake.entry[1].err = ESP_ERR_NOT_FOUND;
  fake.entry[4].err = ESP_FAIL;
  CHECK_INT(start_with_fake(net, saved), 2);
  CHECK_INT(fake.reads, 5);
  CHECK_STR(saved[0].ssid, "n0");
  CHECK_STR(saved[1].ssid, "n3");
  CHECK_STR(net[1].password, "password3");
  CHECK_INT(warn_count, 3);
  CHECK(logged("saved network 1: ESP_ERR_NOT_FOUND: left out"));
  CHECK(logged("saved network 2: empty SSID: left out"));
  CHECK(logged("saved network 4: ESP_FAIL: left out"));
  for (int i = 0; i < log_count; i++) {
    CHECK(strstr(log_lines[i], "password") == NULL);
  }
  /* What failed was wiped: no "password1/2/4" anywhere in the copy. */
  const uint8_t *bytes = (const uint8_t *)net;
  for (size_t i = 0; i + 9 <= sizeof(net); i++) {
    CHECK(memcmp(bytes + i, "password1", 9) != 0 &&
          memcmp(bytes + i, "password2", 9) != 0 &&
          memcmp(bytes + i, "password4", 9) != 0);
  }
  CHECK_INT(seen.ap_on, 0);
  CHECK_INT(seen.scans, 1);
}

/* None can be read: as with no saved network, the access point at once;
 * never an abort (the test would not get here). */
static void test_none_readable(void) {
  settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS];
  wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  fake_reset();
  for (int i = 0; i < 8; i++) {
    fake_add(i == 3 ? "" : "net", "secretpw");
    fake.entry[i].err = i == 3 ? ESP_OK : ESP_FAIL;
  }
  CHECK_INT(start_with_fake(net, saved), 0);
  CHECK_INT(fake.reads, 8);
  CHECK_INT(warn_count, 9);
  CHECK(logged("none of the 8 saved networks can be read"));
  CHECK_INT(seen.ap_on, 1);
  CHECK_INT(seen.why, WL_AP_WHY_NO_SAVED);
  CHECK_INT(seen.dns_starts, 1);
  CHECK_INT(seen.scans, 0);
  CHECK_INT(seen.attempts, 0);
  CHECK_INT(seen.ap_timer_arms, 0);
  for (int i = 0; i < 8; i++) {
    CHECK(saved[i].ssid == NULL);
  }
}

/* More than 8 by count (broken settings): the first 8. */
static void test_too_many(void) {
  settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS];
  wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  fake_reset();
  char name[8];
  for (int i = 0; i < 12; i++) {
    snprintf(name, sizeof(name), "n%d", i);
    fake_add(name, "secretpw");
  }
  CHECK_INT(wifi_saved_load(net, saved), 8);
  CHECK_INT(fake.reads, 8);
  CHECK_STR(saved[7].ssid, "n7");
  CHECK(logged("12 saved networks: only the first 8 are used"));

  /* A count far too big, and reads past the end that fail. */
  fake_reset();
  fake.count = (size_t)-1;
  CHECK_INT(wifi_saved_load(net, saved), 0);
  CHECK_INT(fake.reads, 8);
}

/* SSID and password buffers with no NUL at all (broken settings): cut at
 * 32 and 64 bytes. */
static void test_no_nul(void) {
  settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS];
  wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS];
  fake_reset();
  fake.count = 1;
  memset(fake.entry[0].net.ssid, 'S', sizeof(fake.entry[0].net.ssid));
  memset(fake.entry[0].net.password, 'P', sizeof(fake.entry[0].net.password));
  CHECK_INT(wifi_saved_load(net, saved), 1);
  CHECK_INT(strlen(net[0].ssid), 32);
  CHECK_INT(strlen(net[0].password), 64);
  CHECK_INT(wl_ssid_len(saved[0].ssid), 32);
  CHECK(saved[0].has_password);
}

void test_saved(void) {
  test_none_saved();
  test_all_read();
  test_some_fail();
  test_none_readable();
  test_too_many();
  test_no_nul();
}
