/*
 * SPDX-License-Identifier: Apache-2.0
 */
/* Host tests of src/wifi_logic.c: the examples of README.md and broken
 * input. */
#include <limits.h>
#include <stdlib.h>

#include "check.h"
#include "wifi_logic.h"

static const uint8_t MAC[6] = {0xAA, 0xBB, 0xCC, 0x0A, 0x1B, 0x2C};
#define CANARY 0x5A

/* The host name rules written out step by step, as README.md has them, to
 * compare the real code with on every buffer length. */
static void ref_sanitize(const char *name, const uint8_t mac[6], char *out,
                         size_t out_len) {
  if (out == NULL || out_len == 0) {
    return;
  }
  size_t name_len = name != NULL ? strlen(name) : 0;
  char *t = malloc(name_len + 64);
  size_t n = 0;
  bool gap = false;
  for (size_t i = 0; i < name_len; i++) { /* steps 1 and 2 */
    unsigned char c = (unsigned char)name[i];
    bool keep = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z');
    if (keep) {
      t[n++] = (char)c;
      gap = false;
    } else if (!gap) {
      t[n++] = '-';
      gap = true;
    }
  }
  size_t start = 0; /* step 3 */
  while (start < n && t[start] == '-') {
    start++;
  }
  while (n > start && t[n - 1] == '-') {
    n--;
  }
  size_t len = n - start;
  size_t max = out_len - 1 < 31 ? out_len - 1 : 31; /* step 4 */
  if (len > max) {
    len = max;
  }
  while (len > 0 && t[start + len - 1] == '-') {
    len--;
  }
  if (len == 0) { /* step 5 */
    start = 0;
    len = (size_t)snprintf(t, 64, "esp32-airplay-%02x%02x%02x", mac[3], mac[4],
                           mac[5]);
    if (len > max) {
      len = max;
    }
    while (len > 0 && t[len - 1] == '-') {
      len--;
    }
  }
  memcpy(out, t + start, len);
  out[len] = '\0';
  free(t);
}

static void check_sanitize_all_lengths(const char *name, int line) {
  for (size_t len = 0; len <= 40; len++) {
    char got[48], want[48];
    memset(got, CANARY, sizeof(got));
    memset(want, CANARY, sizeof(want));
    wl_sanitize_hostname(name, MAC, got, len);
    ref_sanitize(name, MAC, want, len);
    check_count++;
    if (memcmp(got, want, sizeof(got)) != 0) {
      check_failures++;
      fprintf(stderr, "line %d: name \"%s\" out_len %zu: \"%.*s\" vs \"%.*s\"\n",
              line, name != NULL ? name : "(NULL)", len, (int)len, got,
              (int)len, want);
    }
    for (size_t i = len; i < sizeof(got); i++) { /* no write past out_len */
      if ((unsigned char)got[i] != CANARY) {
        check_failures++;
        fprintf(stderr, "line %d: wrote byte %zu of %zu\n", line, i, len);
        break;
      }
    }
    if (len > 0) {
      CHECK(memchr(got, '\0', len) != NULL);
      CHECK(strlen(got) <= WL_HOSTNAME_MAX_LEN);
      CHECK(strlen(got) == 0 || (got[0] != '-' && got[strlen(got) - 1] != '-'));
    }
  }
}

static void sanitize(const char *name, size_t out_len, char *out) {
  wl_sanitize_hostname(name, MAC, out, out_len);
}

static void test_sanitize_examples(void) {
  char out[64];
  char a40[41], a31[32], abc[37], abc_want[32];
  memset(a40, 'a', 40);
  a40[40] = '\0';
  memset(a31, 'a', 31);
  a31[31] = '\0';
  snprintf(abc, sizeof(abc), "abc!!!%s", "dddddddddddddddddddddddddddddd");
  snprintf(abc_want, sizeof(abc_want), "abc-%s", "ddddddddddddddddddddddddddd");
  CHECK_INT(strlen(abc), 3 + 3 + 30);
  CHECK_INT(strlen(abc_want), 31);

  const struct {
    const char *name;
    const char *want;
  } table[] = {
      {"Living Room", "Living-Room"},
      {"  Front--Left!! ", "Front-Left"},
      {"\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F 2", "2"}, /* Кухня 2 */
      {"\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F", "esp32-airplay-0a1b2c"},
      {"ESP32 AirPlay", "ESP32-AirPlay"},
      {a40, a31},
      {abc, abc_want},
      {"", "esp32-airplay-0a1b2c"},
      {NULL, "esp32-airplay-0a1b2c"},
      {"Front Left", "Front-Left"}, /* example 10 */
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    sanitize(table[i].name, 32, out);
    CHECK_STR(out, table[i].want);
  }

  sanitize("Living Room", 5, out);
  CHECK_STR(out, "Livi");
  sanitize("Living Room", 6, out);
  CHECK_STR(out, "Livin");
  sanitize("Living Room", 8, out);
  CHECK_STR(out, "Living");
  sanitize("Living Room", 1, out);
  CHECK_STR(out, "");
  /* The default name is cut the same way. */
  sanitize("", 15, out);
  CHECK_STR(out, "esp32-airplay");
  sanitize("!!", 16, out);
  CHECK_STR(out, "esp32-airplay-0");
  sanitize(NULL, 2, out);
  CHECK_STR(out, "e");
  /* A NULL MAC gives zeros instead of crashing. */
  wl_sanitize_hostname("", NULL, out, 32);
  CHECK_STR(out, "esp32-airplay-000000");
}

static void test_sanitize_edges(void) {
  /* out NULL or out_len 0: nothing, no crash. */
  wl_sanitize_hostname("Living Room", MAC, NULL, 32);
  char one = CANARY;
  wl_sanitize_hostname("Living Room", MAC, &one, 0);
  CHECK_INT((unsigned char)one, CANARY);

  const char *names[] = {
      "Living Room", "  Front--Left!! ", "\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F 2",
      "ESP32 AirPlay", "", NULL, "-", "---a---", "a-b", "a b c d e f g h i j k l m n o p",
      "\xff\xfe\xfd", "x\x01y\x7fz", "0123456789012345678901234567890123456789",
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa b", /* '-' lands on the cut */
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaa b",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    check_sanitize_all_lengths(names[i], __LINE__);
  }

  /* Random bytes, all lengths. */
  srand(1);
  for (int round = 0; round < 2000; round++) {
    char name[80];
    size_t len = (size_t)(rand() % 79);
    for (size_t i = 0; i < len; i++) {
      static const char pool[] = "aZ09 -!._\x80\xd0\xff";
      name[i] = (rand() % 3 == 0) ? (char)(1 + rand() % 255)
                                  : pool[rand() % (sizeof(pool) - 1)];
    }
    name[len] = '\0';
    check_sanitize_all_lengths(name, __LINE__);
  }

  /* A long name (no limit on the input). */
  size_t big = 100000;
  char *huge = malloc(big + 1);
  for (size_t i = 0; i < big; i++) {
    huge[i] = (i % 7 == 0) ? ' ' : 'q';
  }
  huge[big] = '\0';
  char out[32];
  wl_sanitize_hostname(huge, MAC, out, sizeof(out));
  CHECK_INT(strlen(out), 31);
  CHECK(out[30] != '-');
  free(huge);
}

static void test_mac_str(void) {
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
  for (size_t len = 0; len <= 24; len++) {
    char buf[32];
    memset(buf, CANARY, sizeof(buf));
    wl_mac_str(mac, buf, len);
    if (len == 0) {
      CHECK_INT((unsigned char)buf[0], CANARY);
    } else if (len < 18) {
      CHECK_STR(buf, "");
    } else {
      CHECK_STR(buf, "AA:BB:CC:01:02:03");
    }
    for (size_t i = len; i < sizeof(buf); i++) {
      CHECK_INT((unsigned char)buf[i], CANARY);
    }
  }
  wl_mac_str(mac, NULL, 18);
  char buf[18];
  wl_mac_str(NULL, buf, sizeof(buf));
  CHECK_STR(buf, "");
  const uint8_t lower[6] = {0x0a, 0xbc, 0xde, 0xf0, 0x00, 0xff};
  wl_mac_str(lower, buf, sizeof(buf));
  CHECK_STR(buf, "0A:BC:DE:F0:00:FF");
}

static void test_ip_str(void) {
  const uint32_t ip = wl_ipv4(192, 168, 1, 23);
  const uint32_t widest = wl_ipv4(255, 255, 255, 255);
  char buf[32];

  CHECK_INT(wl_ip_str(true, ip, NULL, 16), ESP_ERR_INVALID_ARG);
  CHECK_INT(wl_ip_str(false, ip, NULL, 0), ESP_ERR_INVALID_ARG);
  for (size_t len = 0; len <= 24; len++) {
    for (int connected = 0; connected <= 1; connected++) {
      memset(buf, CANARY, sizeof(buf));
      esp_err_t err = wl_ip_str(connected, widest, buf, len);
      if (len < 16) {
        CHECK_INT(err, ESP_ERR_INVALID_SIZE);
      } else if (!connected) {
        CHECK_INT(err, ESP_ERR_INVALID_STATE);
      } else {
        CHECK_INT(err, ESP_OK);
      }
      if (len > 0) {
        CHECK_STR(buf, err == ESP_OK ? "255.255.255.255" : "");
      } else {
        CHECK_INT((unsigned char)buf[0], CANARY);
      }
      for (size_t i = len; i < sizeof(buf); i++) {
        CHECK_INT((unsigned char)buf[i], CANARY);
      }
    }
  }
  CHECK_INT(wl_ip_str(true, ip, buf, 16), ESP_OK);
  CHECK_STR(buf, "192.168.1.23");
  CHECK_INT(wl_ip_str(true, 0, buf, 16), ESP_OK);
  CHECK_STR(buf, "0.0.0.0");

  /* 192.168.4.1 in network byte order: the bytes in that order in memory. */
  uint32_t ap = wl_ipv4(192, 168, 4, 1);
  const uint8_t want[4] = {192, 168, 4, 1};
  CHECK(memcmp(&ap, want, 4) == 0);
}

static void test_ap_params(void) {
  wl_ap_params_t p;
  wl_ap_params(NULL, NULL, &p);
  CHECK_INT(p.ssid_len, strlen("ESP32-AirPlay-Setup"));
  CHECK(memcmp(p.ssid, "ESP32-AirPlay-Setup", p.ssid_len) == 0);
  CHECK(!p.wpa2 && !p.bad_ssid && !p.bad_password && p.password_len == 0);

  wl_ap_params("", NULL, &p);
  CHECK(p.bad_ssid);
  CHECK(memcmp(p.ssid, "ESP32-AirPlay-Setup", p.ssid_len) == 0);

  char s32[33], s33[34];
  memset(s32, 'S', 32);
  s32[32] = '\0';
  memset(s33, 'S', 33);
  s33[33] = '\0';
  wl_ap_params(s32, NULL, &p);
  CHECK(!p.bad_ssid && p.ssid_len == 32 && memcmp(p.ssid, s32, 32) == 0);
  wl_ap_params(s33, NULL, &p);
  CHECK(p.bad_ssid && p.ssid_len == strlen("ESP32-AirPlay-Setup"));
  wl_ap_params("x", NULL, &p);
  CHECK(!p.bad_ssid && p.ssid_len == 1 && p.ssid[0] == 'x');

  char pw[70];
  for (size_t len = 0; len <= 66; len++) {
    memset(pw, 'p', len);
    pw[len] = '\0';
    wl_ap_params(NULL, pw, &p);
    bool ok = len >= 8 && len <= 63;
    CHECK_INT(p.wpa2, ok);
    CHECK_INT(p.bad_password, !ok);
    CHECK_INT(p.password_len, ok ? len : 0);
  }
}

static void test_sec_allowed(void) {
  CHECK(wl_sec_allowed(false, WL_SEC_OPEN));
  CHECK(!wl_sec_allowed(false, WL_SEC_PSK));
  CHECK(!wl_sec_allowed(false, WL_SEC_OTHER));
  CHECK(!wl_sec_allowed(true, WL_SEC_OPEN));
  CHECK(wl_sec_allowed(true, WL_SEC_PSK));
  CHECK(!wl_sec_allowed(true, WL_SEC_OTHER));
}

/* An SSID in a buffer of exactly its length: reading past it is a bug that
 * AddressSanitizer reports. */
static uint8_t *exact_ssid(const char *s, size_t len) {
  uint8_t *p = malloc(len > 0 ? len : 1);
  memcpy(p, s, len);
  return p;
}

static wl_ap_t ap(const void *ssid, int8_t rssi, uint8_t last, wl_sec_t sec) {
  wl_ap_t a = {.ssid = ssid, .rssi = rssi, .channel = (uint8_t)(last % 13 + 1),
               .sec = sec, .bssid = {2, 0, 0, 0, 0, last}};
  return a;
}

static void test_ssid(void) {
  char a[40] = "Home";
  CHECK_INT(wl_ssid_len(NULL), 0);
  CHECK_INT(wl_ssid_len(""), 0);
  CHECK_INT(wl_ssid_len(a), 4);
  memset(a, 'x', sizeof(a));
  CHECK_INT(wl_ssid_len(a), 32); /* no NUL: stops at 32 */
  CHECK(wl_ssid_equal("Home", "Home"));
  CHECK(!wl_ssid_equal("Home", "Home-2G"));
  CHECK(!wl_ssid_equal("Home", "home"));
  CHECK(wl_ssid_equal("", NULL));
  /* 32 bytes and no NUL vs. the same 32 bytes with a NUL. */
  char nul32[33];
  memset(nul32, 'x', 32);
  nul32[32] = '\0';
  uint8_t *raw = exact_ssid(nul32, 32);
  CHECK(wl_ssid_equal(raw, nul32));
  CHECK_INT(wl_ssid_len(raw), 32);
  free(raw);
}

static void test_choose(void) {
  size_t si = 99;
  /* Example 3. */
  wl_ap_t list3[] = {ap("Home", -70, 1, WL_SEC_PSK),
                     ap("Home-2G", -50, 2, WL_SEC_PSK),
                     ap("Neighbour", -30, 3, WL_SEC_PSK)};
  wl_saved_t saved3[] = {{"Home", true}, {"Home-2G", true}};
  CHECK_INT(wl_choose(list3, 3, saved3, 2, &si), 1);
  CHECK_INT(si, 1);

  /* Example 4: mesh, two nodes; the -45 one. */
  wl_ap_t mesh[] = {ap("Home", -60, 1, WL_SEC_PSK),
                    ap("Home", -45, 2, WL_SEC_PSK)};
  wl_saved_t home[] = {{"Home", true}};
  CHECK_INT(wl_choose(mesh, 2, home, 1, &si), 1);
  CHECK_INT(si, 0);
  CHECK_INT(mesh[1].bssid[5], 2);

  /* Equal RSSI: the first in scan order. */
  wl_ap_t tie[] = {ap("A", -50, 1, WL_SEC_PSK), ap("B", -50, 2, WL_SEC_PSK),
                   ap("A", -50, 3, WL_SEC_PSK)};
  wl_saved_t ab[] = {{"B", true}, {"A", true}};
  CHECK_INT(wl_choose(tie, 3, ab, 2, &si), 0);
  CHECK_INT(si, 1);

  /* Nothing: empty lists, no saved SSID seen, NULLs. */
  si = 77;
  CHECK_INT(wl_choose(NULL, 0, home, 1, &si), -1);
  CHECK_INT(wl_choose(list3, 0, home, 1, &si), -1);
  CHECK_INT(wl_choose(list3, 3, home, 0, &si), -1);
  CHECK_INT(wl_choose(list3, 3, NULL, 1, &si), -1);
  wl_saved_t attic[] = {{"Attic", true}};
  CHECK_INT(wl_choose(list3, 3, attic, 1, &si), -1);
  CHECK_INT(si, 77); /* untouched */
  CHECK_INT(wl_choose(list3, 3, saved3, 2, NULL), 1);

  /* Hidden networks (empty SSID) never match, not even an empty saved one.
   */
  wl_ap_t hidden[] = {ap("", -20, 1, WL_SEC_PSK), ap("\0\0\0", -20, 2, WL_SEC_OPEN)};
  wl_saved_t empty[] = {{"", false}, {"", true}};
  CHECK_INT(wl_choose(hidden, 2, empty, 2, &si), -1);

  /* Security: "" only open; a password only WPA2/WPA3. */
  wl_ap_t sec[] = {ap("Cafe", -30, 1, WL_SEC_OPEN), ap("Cafe", -40, 2, WL_SEC_PSK),
                   ap("Cafe", -20, 3, WL_SEC_OTHER)};
  wl_saved_t cafe_pw[] = {{"Cafe", true}};
  wl_saved_t cafe_open[] = {{"Cafe", false}};
  CHECK_INT(wl_choose(sec, 3, cafe_pw, 1, &si), 1);
  CHECK_INT(wl_choose(sec, 3, cafe_open, 1, &si), 0);
  wl_ap_t wep_only[] = {ap("Cafe", -20, 3, WL_SEC_OTHER)};
  CHECK_INT(wl_choose(wep_only, 1, cafe_pw, 1, &si), -1);
  CHECK_INT(wl_choose(wep_only, 1, cafe_open, 1, &si), -1);

  /* 8 saved networks, the strongest seen is the 8th. */
  wl_saved_t eight[8] = {{"n0", true}, {"n1", true}, {"n2", true}, {"n3", true},
                         {"n4", true}, {"n5", true}, {"n6", true}, {"n7", true}};
  wl_ap_t seen[] = {ap("n3", -80, 1, WL_SEC_PSK), ap("x", -10, 2, WL_SEC_PSK),
                    ap("n7", -40, 3, WL_SEC_PSK), ap("n0", -60, 4, WL_SEC_PSK)};
  CHECK_INT(wl_choose(seen, 4, eight, 8, &si), 2);
  CHECK_INT(si, 7);

  /* SSIDs of exactly 32 bytes and no NUL, in the scan and in the saved
   * list; one differing in the last byte. */
  char s32[33];
  memset(s32, 'L', 32);
  s32[32] = '\0';
  uint8_t *exact = exact_ssid(s32, 32);
  char other[33];
  memcpy(other, s32, 33);
  other[31] = 'M';
  uint8_t *exact_other = exact_ssid(other, 32);
  char *saved_raw = (char *)exact_ssid(s32, 32);
  wl_ap_t long_list[] = {ap(exact_other, -10, 1, WL_SEC_PSK),
                         ap(exact, -50, 2, WL_SEC_PSK)};
  wl_saved_t long_saved[] = {{saved_raw, true}};
  CHECK_INT(wl_choose(long_list, 2, long_saved, 1, &si), 1);
  /* A 33-byte SSID cut to 32 is the same network (ESP-IDF has 32 at most).
   */
  char s_long[40];
  memset(s_long, 'L', 39);
  s_long[39] = '\0';
  wl_saved_t long_saved2[] = {{s_long, true}};
  CHECK_INT(wl_choose(long_list, 2, long_saved2, 1, &si), 1);
  free(exact);
  free(exact_other);
  free(saved_raw);

  /* RSSI extremes. */
  wl_ap_t ext[] = {ap("A", -128, 1, WL_SEC_PSK), ap("A", 127, 2, WL_SEC_PSK)};
  wl_saved_t a_only[] = {{"A", true}};
  CHECK_INT(wl_choose(ext, 2, a_only, 1, &si), 1);
  wl_ap_t only_min[] = {ap("A", -128, 1, WL_SEC_PSK)};
  CHECK_INT(wl_choose(only_min, 1, a_only, 1, &si), 0);
}

static void check_sorted_unique(const wl_ap_t *aps, const uint16_t *idx,
                                size_t n) {
  for (size_t i = 0; i < n; i++) {
    CHECK(wl_ssid_len(aps[idx[i]].ssid) > 0);
    if (i > 0) {
      CHECK(aps[idx[i - 1]].rssi >= aps[idx[i]].rssi);
    }
    for (size_t j = 0; j < i; j++) {
      CHECK(!wl_ssid_equal(aps[idx[i]].ssid, aps[idx[j]].ssid));
    }
  }
}

static void test_shape(void) {
  uint16_t idx[WL_SCAN_LIST_MAX + 8];

  /* Empty list; NULLs; out_max 0. */
  CHECK_INT(wl_shape_scan(NULL, 5, idx, 32), 0);
  wl_ap_t one[] = {ap("A", -1, 1, WL_SEC_PSK)};
  CHECK_INT(wl_shape_scan(one, 0, idx, 32), 0);
  CHECK_INT(wl_shape_scan(one, 1, NULL, 32), 0);
  CHECK_INT(wl_shape_scan(one, 1, idx, 0), 0);

  /* One per SSID (its strongest), hidden left out, strongest first. */
  wl_ap_t list[] = {
      ap("Home", -60, 1, WL_SEC_PSK), ap("", -10, 2, WL_SEC_PSK),
      ap("Cafe", -70, 3, WL_SEC_OPEN), ap("Home", -45, 4, WL_SEC_PSK),
      ap("\0hidden", -5, 5, WL_SEC_PSK), ap("Cafe", -75, 6, WL_SEC_OPEN),
      ap("Work", -45, 7, WL_SEC_PSK),
  };
  size_t n = wl_shape_scan(list, 7, idx, 32);
  CHECK_INT(n, 3);
  CHECK_INT(idx[0], 3); /* Home -45 (before Work -45: scan order) */
  CHECK_INT(idx[1], 6); /* Work -45 */
  CHECK_INT(idx[2], 2); /* Cafe -70 */
  check_sorted_unique(list, idx, n);

  /* Equal RSSI everywhere: scan order, first of each SSID. */
  wl_ap_t tie[] = {ap("B", -50, 1, WL_SEC_PSK), ap("A", -50, 2, WL_SEC_PSK),
                   ap("B", -50, 3, WL_SEC_PSK), ap("C", -50, 4, WL_SEC_PSK)};
  n = wl_shape_scan(tie, 4, idx, 32);
  CHECK_INT(n, 3);
  CHECK_INT(idx[0], 0);
  CHECK_INT(idx[1], 1);
  CHECK_INT(idx[2], 3);

  /* All hidden. */
  wl_ap_t hidden[] = {ap("", -10, 1, WL_SEC_PSK), ap("", -20, 2, WL_SEC_PSK)};
  CHECK_INT(wl_shape_scan(hidden, 2, idx, 32), 0);

  /* More than 32 SSIDs: the 32 strongest. */
  enum { MANY = 100 };
  static char names[MANY][8];
  wl_ap_t many[MANY];
  for (int i = 0; i < MANY; i++) {
    snprintf(names[i], sizeof(names[i]), "n%d", i);
    many[i] = ap(names[i], (int8_t)(-100 + (i * 37) % 90), (uint8_t)i,
                 WL_SEC_PSK);
  }
  memset(idx, 0xff, sizeof(idx));
  n = wl_shape_scan(many, MANY, idx, WL_SCAN_LIST_MAX);
  CHECK_INT(n, 32);
  CHECK_INT(idx[32], 0xffff); /* nothing past out_max */
  check_sorted_unique(many, idx, n);
  int weakest_in = many[idx[31]].rssi;
  int stronger_left_out = 0;
  for (int i = 0; i < MANY; i++) {
    bool in = false;
    for (size_t k = 0; k < n; k++) {
      in = in || idx[k] == i;
    }
    if (!in && many[i].rssi > weakest_in) {
      stronger_left_out++;
    }
  }
  CHECK_INT(stronger_left_out, 0);

  /* Exactly 32 distinct and 32 duplicates of one SSID. */
  wl_ap_t dup[64];
  for (int i = 0; i < 64; i++) {
    dup[i] = ap(i < 32 ? names[i] : "same", (int8_t)(-i), (uint8_t)i, WL_SEC_PSK);
  }
  n = wl_shape_scan(dup, 64, idx, 32);
  CHECK_INT(n, 32);
  check_sorted_unique(dup, idx, n);
  n = wl_shape_scan(dup + 32, 32, idx, 32);
  CHECK_INT(n, 1);
  CHECK_INT(idx[0], 0); /* "same" at -32 is the strongest */

  /* SSIDs of exactly 32 bytes and no NUL (AddressSanitizer checks reads). */
  char s32[33];
  memset(s32, 'Q', 32);
  s32[32] = '\0';
  uint8_t *a = exact_ssid(s32, 32);
  uint8_t *b = exact_ssid(s32, 32);
  s32[31] = 'R';
  uint8_t *c = exact_ssid(s32, 32);
  wl_ap_t longs[] = {ap(a, -70, 1, WL_SEC_PSK), ap(b, -60, 2, WL_SEC_PSK),
                     ap(c, -80, 3, WL_SEC_PSK)};
  n = wl_shape_scan(longs, 3, idx, 32);
  CHECK_INT(n, 2);
  CHECK_INT(idx[0], 1);
  CHECK_INT(idx[1], 2);
  free(a);
  free(b);
  free(c);
}

static void test_retry(void) {
  const uint32_t want_ms[] = {0, 0, 1000, 2000, 4000, 8000, 16000};
  for (int clients = 0; clients <= 1; clients++) {
    for (unsigned n = 0; n <= 6; n++) {
      wl_retry_t r = wl_retry_after(n, clients);
      CHECK_INT(r.action, WL_RETRY_SAME);
      CHECK_INT(r.delay_ms, want_ms[n]); /* the same with clients */
    }
    const unsigned full[] = {7, 8, 9, 100, 1000000, UINT_MAX};
    for (size_t i = 0; i < sizeof(full) / sizeof(full[0]); i++) {
      wl_retry_t r = wl_retry_after(full[i], clients);
      CHECK_INT(r.action, WL_RETRY_FULL);
      CHECK_INT(r.delay_ms, clients ? 60000 : 30000);
    }
  }
}

static void test_reason_names(void) {
  CHECK_STR(wl_reason_name(1), "UNSPECIFIED");
  CHECK_STR(wl_reason_name(15), "4WAY_HANDSHAKE_TIMEOUT");
  CHECK_STR(wl_reason_name(200), "BEACON_TIMEOUT");
  CHECK_STR(wl_reason_name(201), "NO_AP_FOUND");
  CHECK_STR(wl_reason_name(202), "AUTH_FAIL");
  CHECK_STR(wl_reason_name(212), "NO_AP_FOUND_RSSI");
  CHECK_STR(wl_reason_name(0), "UNKNOWN");
  CHECK_STR(wl_reason_name(40), "UNKNOWN");
  CHECK_STR(wl_reason_name(255), "UNKNOWN");
  CHECK_STR(wl_reason_name(256 + 15), "UNKNOWN");
}

void test_logic(void) {
  test_sanitize_examples();
  test_sanitize_edges();
  test_mac_str();
  test_ip_str();
  test_ap_params();
  test_sec_allowed();
  test_ssid();
  test_choose();
  test_shape();
  test_retry();
  test_reason_names();
}
