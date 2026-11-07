/* SPDX-License-Identifier: Apache-2.0 */

/* Host tests of the settings against the fake NVS. Every test runs in its own
 * process, so the settings' RAM state starts fresh each time. */

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fake_nvs.h"
#include "sdkconfig.h"
#include "settings.h"

#ifdef CONFIG_AIRPLAY_MULTICHANNEL
#define EXPECTED_MC_SPEAKER CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER
#else
#define EXPECTED_MC_SPEAKER (-1)
#endif

#define NS "airplay"
#define LIST_SIZE 796

/* ---- checks ---- */

#define FAIL(...)                                                              \
  do {                                                                         \
    fprintf(stderr, "  %s:%d: ", __FILE__, __LINE__);                          \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
    exit(1);                                                                   \
  } while (0)

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL("CHECK(%s)", #cond);                                                \
  } while (0)

#define CHECK_INT(actual, expected)                                            \
  do {                                                                         \
    long long a_ = (long long)(actual), e_ = (long long)(expected);            \
    if (a_ != e_)                                                              \
      FAIL("%s is %lld (0x%llx), expected %lld (0x%llx)", #actual, a_,         \
           (unsigned long long)a_, e_, (unsigned long long)e_);                \
  } while (0)

#define CHECK_STR(actual, expected)                                            \
  do {                                                                         \
    const char *a_ = (actual), *e_ = (expected);                               \
    if (strcmp(a_, e_) != 0)                                                   \
      FAIL("%s is \"%s\", expected \"%s\"", #actual, a_, e_);                  \
  } while (0)

#define CHECK_NO_HANDLES() CHECK_INT(fake_nvs_handles_open(), 0)

/* ---- network list helpers, written apart from the code under test ---- */

typedef struct {
  const char *ssid;
  const char *password;
} net_t;

static void make_list(uint8_t out[LIST_SIZE], uint16_t count, uint16_t last,
                      uint16_t next, const net_t *nets) {
  memset(out, 0, LIST_SIZE);
  const uint8_t magic[4] = {0x49, 0x46, 0x49, 0x57};
  memcpy(out, magic, 4);
  out[4] = 1;
  out[6] = (uint8_t)count;
  out[7] = (uint8_t)(count >> 8);
  out[8] = (uint8_t)last;
  out[9] = (uint8_t)(last >> 8);
  out[10] = (uint8_t)next;
  out[11] = (uint8_t)(next >> 8);
  for (uint16_t i = 0; i < count && i < 8; i++) {
    memcpy(out + 12 + i * 98, nets[i].ssid, strlen(nets[i].ssid));
    memcpy(out + 12 + i * 98 + 33, nets[i].password,
           strlen(nets[i].password));
  }
}

static void put_list(const uint8_t *blob, size_t len) {
  fake_nvs_put(NS, "wifi_list", FAKE_NVS_BLOB, blob, len);
}

static void check_stored_list_at(const uint8_t expected[LIST_SIZE],
                                 const char *file, int line) {
  uint8_t stored[LIST_SIZE + 8];
  size_t len = 0;
  if (!fake_nvs_peek(NS, "wifi_list", FAKE_NVS_BLOB, stored, sizeof(stored),
                     &len)) {
    fprintf(stderr, "  %s:%d: no wifi_list blob\n", file, line);
    exit(1);
  }
  if (len != LIST_SIZE) {
    fprintf(stderr, "  %s:%d: wifi_list is %zu bytes\n", file, line, len);
    exit(1);
  }
  for (size_t i = 0; i < LIST_SIZE; i++) {
    if (stored[i] != expected[i]) {
      fprintf(stderr, "  %s:%d: wifi_list byte %zu is %02x, expected %02x\n",
              file, line, i, stored[i], expected[i]);
      exit(1);
    }
  }
}
#define CHECK_STORED_LIST(expected) check_stored_list_at(expected, __FILE__, __LINE__)

static void check_header_at(const uint8_t header[12], const char *file,
                            int line) {
  uint8_t stored[LIST_SIZE];
  if (!fake_nvs_peek(NS, "wifi_list", FAKE_NVS_BLOB, stored, sizeof(stored),
                     NULL) ||
      memcmp(stored, header, 12) != 0) {
    fprintf(stderr, "  %s:%d: wifi_list header differs\n", file, line);
    exit(1);
  }
}
#define CHECK_HEADER(...)                                                      \
  do {                                                                         \
    const uint8_t h_[12] = {__VA_ARGS__};                                      \
    check_header_at(h_, __FILE__, __LINE__);                                   \
  } while (0)

static const char *stored_str(const char *key) {
  static char buf[4096];
  size_t len = 0;
  if (!fake_nvs_peek(NS, key, FAKE_NVS_STR, buf, sizeof(buf) - 1, &len)) {
    return "<missing>";
  }
  buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
  return buf;
}

static int32_t stored_i32(const char *key) {
  int32_t v = 0;
  if (!fake_nvs_peek(NS, key, FAKE_NVS_I32, &v, sizeof(v), NULL)) {
    FAIL("no i32 %s", key);
  }
  return v;
}

static void check_network(size_t index, const char *ssid,
                          const char *password) {
  settings_wifi_network_t n;
  memset(&n, 0xAA, sizeof(n));
  CHECK_INT(settings_get_wifi_network(index, &n), ESP_OK);
  CHECK(memchr(n.ssid, 0, sizeof(n.ssid)) != NULL);
  CHECK(memchr(n.password, 0, sizeof(n.password)) != NULL);
  CHECK_STR(n.ssid, ssid);
  CHECK_STR(n.password, password);
}

static void check_no_networks(void) {
  settings_wifi_network_t n;
  CHECK_INT(settings_get_wifi_network_count(), 0);
  CHECK(!settings_has_wifi_credentials());
  CHECK_INT(settings_get_wifi_network(0, &n), ESP_ERR_NOT_FOUND);
}

static const net_t FULL[8] = {{"S0", "pass0000"}, {"S1", "pass1111"},
                              {"S2", "pass2222"}, {"S3", "pass3333"},
                              {"S4", "pass4444"}, {"S5", "pass5555"},
                              {"S6", "pass6666"}, {"S7", "pass7777"}};

/* ---- Wi-Fi: the examples ---- */

static void test_wifi_example_add_and_update(void) {
  uint8_t expected[LIST_SIZE];

  CHECK_INT(settings_set_wifi_credentials("NetA", "password1"), ESP_OK);
  CHECK_HEADER(0x49, 0x46, 0x49, 0x57, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
               0x00, 0x00);
  make_list(expected, 1, 0, 0, (net_t[]){{"NetA", "password1"}});
  CHECK_STORED_LIST(expected);
  CHECK_STR(stored_str("wifi_ssid"), "NetA");
  CHECK_STR(stored_str("wifi_pass"), "password1");

  CHECK_INT(settings_set_wifi_credentials("NetB", "password2"), ESP_OK);
  CHECK_HEADER(0x49, 0x46, 0x49, 0x57, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00,
               0x00, 0x00);
  make_list(expected, 2, 1, 0,
            (net_t[]){{"NetA", "password1"}, {"NetB", "password2"}});
  CHECK_STORED_LIST(expected);
  CHECK_STR(stored_str("wifi_ssid"), "NetB");
  CHECK_STR(stored_str("wifi_pass"), "password2");

  CHECK_INT(settings_set_wifi_credentials("NetA", "newpass11"), ESP_OK);
  CHECK_HEADER(0x49, 0x46, 0x49, 0x57, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,
               0x00, 0x00);
  make_list(expected, 2, 0, 0,
            (net_t[]){{"NetA", "newpass11"}, {"NetB", "password2"}});
  CHECK_STORED_LIST(expected);
  CHECK_STR(stored_str("wifi_ssid"), "NetA");
  CHECK_STR(stored_str("wifi_pass"), "newpass11");

  /* Reading the list back: slot order. */
  CHECK_INT(settings_get_wifi_network_count(), 2);
  CHECK(settings_has_wifi_credentials());
  check_network(0, "NetA", "newpass11");
  check_network(1, "NetB", "password2");
  settings_wifi_network_t n;
  CHECK_INT(settings_get_wifi_network(2, &n), ESP_ERR_NOT_FOUND);
  CHECK_INT(settings_get_wifi_network(SIZE_MAX, &n), ESP_ERR_NOT_FOUND);

  CHECK_INT(fake_nvs_uncommitted_closes(), 0);
  CHECK_INT(fake_nvs_commit_count(), 3);
  CHECK_STR(fake_log_text(), "");
}

static void test_wifi_example_full_list(void) {
  uint8_t blob[LIST_SIZE], expected[LIST_SIZE];
  make_list(blob, 8, 7, 0, FULL);
  put_list(blob, sizeof(blob));

  CHECK_INT(settings_set_wifi_credentials("Y", "passwordY"), ESP_OK);
  CHECK_INT(settings_set_wifi_credentials("Z", "passwordZ"), ESP_OK);
  CHECK_HEADER(0x49, 0x46, 0x49, 0x57, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00,
               0x02, 0x00);
  net_t nets[8];
  memcpy(nets, FULL, sizeof(nets));
  nets[0] = (net_t){"Y", "passwordY"};
  nets[1] = (net_t){"Z", "passwordZ"};
  make_list(expected, 8, 1, 2, nets);
  CHECK_STORED_LIST(expected);

  CHECK_INT(settings_get_wifi_network_count(), 8);
  check_network(0, "Y", "passwordY");
  check_network(1, "Z", "passwordZ");
  check_network(7, "S7", "pass7777");
  settings_wifi_network_t n;
  CHECK_INT(settings_get_wifi_network(8, &n), ESP_ERR_NOT_FOUND);
}

static void test_wifi_full_list_eight_more(void) {
  uint8_t blob[LIST_SIZE], expected[LIST_SIZE];
  make_list(blob, 8, 7, 0, FULL);
  put_list(blob, sizeof(blob));

  char ssid[8][8], pass[8][16];
  net_t nets[8];
  for (int i = 0; i < 8; i++) {
    snprintf(ssid[i], sizeof(ssid[i]), "N%d", i);
    snprintf(pass[i], sizeof(pass[i]), "secret-%d", i);
    nets[i] = (net_t){ssid[i], pass[i]};
    CHECK_INT(settings_set_wifi_credentials(ssid[i], pass[i]), ESP_OK);
  }
  make_list(expected, 8, 7, 0, nets);
  CHECK_STORED_LIST(expected);
  for (int i = 0; i < 8; i++) {
    check_network((size_t)i, ssid[i], pass[i]);
  }
}

static void test_wifi_full_list_next_wraps(void) {
  uint8_t blob[LIST_SIZE], expected[LIST_SIZE];
  make_list(blob, 8, 2, 7, FULL);
  put_list(blob, sizeof(blob));
  CHECK_INT(settings_set_wifi_credentials("W", ""), ESP_OK);
  net_t nets[8];
  memcpy(nets, FULL, sizeof(nets));
  nets[7] = (net_t){"W", ""};
  make_list(expected, 8, 7, 0, nets);
  CHECK_STORED_LIST(expected);

  /* An ssid already there is updated in place, "next" stays. */
  CHECK_INT(settings_set_wifi_credentials("S3", "changed!"), ESP_OK);
  nets[3] = (net_t){"S3", "changed!"};
  make_list(expected, 8, 3, 0, nets);
  CHECK_STORED_LIST(expected);
}

static void test_wifi_update_clears_old_password_bytes(void) {
  uint8_t expected[LIST_SIZE];
  CHECK_INT(settings_set_wifi_credentials("Net", "a-long-password-here"),
            ESP_OK);
  CHECK_INT(settings_set_wifi_credentials("Net", "short"), ESP_OK);
  make_list(expected, 1, 0, 0, (net_t[]){{"Net", "short"}});
  CHECK_STORED_LIST(expected);
}

static void test_wifi_ssid_compared_byte_for_byte(void) {
  CHECK_INT(settings_set_wifi_credentials("NetA", "password1"), ESP_OK);
  CHECK_INT(settings_set_wifi_credentials("neta", "password2"), ESP_OK);
  CHECK_INT(settings_set_wifi_credentials("NetA ", "password3"), ESP_OK);
  CHECK_INT(settings_get_wifi_network_count(), 3);
}

static void test_wifi_arguments(void) {
  char s32[33], s33[34], p64[65], p65[66];
  memset(s32, 's', 32);
  s32[32] = '\0';
  memset(s33, 's', 33);
  s33[33] = '\0';
  memset(p64, 'p', 64);
  p64[64] = '\0';
  memset(p65, 'p', 65);
  p65[65] = '\0';

  CHECK_INT(settings_set_wifi_credentials(NULL, "password"),
            ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_set_wifi_credentials("", "password"),
            ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_set_wifi_credentials(s33, "password"),
            ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_set_wifi_credentials("Net", p65), ESP_ERR_INVALID_ARG);
  CHECK_INT(fake_nvs_set_count(), 0);
  CHECK_INT(fake_nvs_open_count(), 0);

  CHECK_INT(settings_set_wifi_credentials(s32, p64), ESP_OK);
  CHECK_INT(settings_set_wifi_credentials("Short", "1234567"), ESP_OK);
  CHECK_INT(settings_set_wifi_credentials("Open", NULL), ESP_OK);
  CHECK_STR(stored_str("wifi_ssid"), "Open");
  CHECK_STR(stored_str("wifi_pass"), "");

  uint8_t expected[LIST_SIZE];
  make_list(expected, 3, 2, 0,
            (net_t[]){{s32, p64}, {"Short", "1234567"}, {"Open", ""}});
  CHECK_STORED_LIST(expected);
  check_network(0, s32, p64);
  check_network(2, "Open", "");

  CHECK_INT(settings_get_wifi_network(0, NULL), ESP_ERR_INVALID_ARG);
}

/* ---- Wi-Fi: invalid lists ---- */

typedef struct {
  const char *what;
  size_t len;
  void (*mutate)(uint8_t *blob);
} bad_list_t;

static void bad_magic(uint8_t *b) { b[3] = 0x58; }
static void bad_magic_order(uint8_t *b) { b[0] = 0x57, b[3] = 0x49; }
static void bad_version_0(uint8_t *b) { b[4] = 0; }
static void bad_version_2(uint8_t *b) { b[4] = 2; }
static void bad_version_high(uint8_t *b) { b[5] = 1; }
static void bad_count_9(uint8_t *b) { b[6] = 9; }
static void bad_count_high(uint8_t *b) { b[7] = 1; }
static void bad_last_eq_count(uint8_t *b) { b[8] = 2; }
static void bad_last_high(uint8_t *b) { b[9] = 1; }
static void bad_next_8(uint8_t *b) { b[10] = 8; }
static void bad_next_high(uint8_t *b) { b[11] = 1; }
static void bad_ssid_no_nul(uint8_t *b) { memset(b + 12 + 98, 'x', 33); }
static void bad_password_no_nul(uint8_t *b) {
  memset(b + 12 + 33, 'y', 65);
}
static void no_change(uint8_t *b) { (void)b; }

static const bad_list_t BAD_LISTS[] = {
    {"magic", LIST_SIZE, bad_magic},
    {"magic byte order", LIST_SIZE, bad_magic_order},
    {"version 0", LIST_SIZE, bad_version_0},
    {"version 2", LIST_SIZE, bad_version_2},
    {"version 0x101", LIST_SIZE, bad_version_high},
    {"count 9", LIST_SIZE, bad_count_9},
    {"count 0x102", LIST_SIZE, bad_count_high},
    {"last-set == count", LIST_SIZE, bad_last_eq_count},
    {"last-set 0x100", LIST_SIZE, bad_last_high},
    {"next 8", LIST_SIZE, bad_next_8},
    {"next 0x100", LIST_SIZE, bad_next_high},
    {"slot 1 ssid without NUL", LIST_SIZE, bad_ssid_no_nul},
    {"slot 0 password without NUL", LIST_SIZE, bad_password_no_nul},
    {"795 bytes", LIST_SIZE - 1, no_change},
    {"797 bytes", LIST_SIZE + 1, no_change},
    {"0 bytes", 0, no_change},
};

static void put_bad_list(const bad_list_t *bad) {
  uint8_t blob[LIST_SIZE + 1];
  memset(blob, 0, sizeof(blob));
  make_list(blob, 2, 1, 0, (net_t[]){{"NetA", "password1"}, {"NetB", "pw"}});
  bad->mutate(blob);
  put_list(blob, bad->len);
}

static void test_wifi_invalid_lists(void) {
  for (size_t i = 0; i < sizeof(BAD_LISTS) / sizeof(BAD_LISTS[0]); i++) {
    fake_nvs_reset();
    put_bad_list(&BAD_LISTS[i]);
    if (settings_get_wifi_network_count() != 0) {
      FAIL("invalid list (%s) was read", BAD_LISTS[i].what);
    }
    check_no_networks();

    /* With the older single network, that one counts. */
    fake_nvs_put_str(NS, "wifi_ssid", "Old");
    fake_nvs_put_str(NS, "wifi_pass", "oldpass1");
    CHECK_INT(settings_get_wifi_network_count(), 1);
    check_network(0, "Old", "oldpass1");

    /* init migrates the older one over the invalid list. */
    CHECK_INT(settings_init(), ESP_OK);
    uint8_t expected[LIST_SIZE];
    make_list(expected, 1, 0, 0, (net_t[]){{"Old", "oldpass1"}});
    CHECK_STORED_LIST(expected);
    CHECK_NO_HANDLES();
  }
}

static void test_wifi_invalid_list_replaced_on_set(void) {
  for (size_t i = 0; i < sizeof(BAD_LISTS) / sizeof(BAD_LISTS[0]); i++) {
    fake_nvs_reset();
    put_bad_list(&BAD_LISTS[i]);
    CHECK_INT(settings_init(), ESP_OK);
    CHECK_INT(fake_nvs_set_count(), 0); /* nothing to migrate */
    CHECK_INT(settings_set_wifi_credentials("New", "password"), ESP_OK);
    uint8_t expected[LIST_SIZE];
    make_list(expected, 1, 0, 0, (net_t[]){{"New", "password"}});
    CHECK_STORED_LIST(expected);
  }
}

static void test_wifi_list_wrong_type(void) {
  fake_nvs_put_str(NS, "wifi_list", "not a blob");
  check_no_networks();
  CHECK_INT(settings_set_wifi_credentials("New", "password"), ESP_OK);
  uint8_t expected[LIST_SIZE];
  make_list(expected, 1, 0, 0, (net_t[]){{"New", "password"}});
  CHECK_STORED_LIST(expected);
}

static void test_wifi_valid_edge_lists(void) {
  uint8_t blob[LIST_SIZE];

  /* count 0: last-set is not checked; a valid empty list wins over the
   * older network. */
  make_list(blob, 0, 5, 3, NULL);
  put_list(blob, sizeof(blob));
  fake_nvs_put_str(NS, "wifi_ssid", "Old");
  check_no_networks();
  CHECK_INT(settings_set_wifi_credentials("New", "password"), ESP_OK);
  uint8_t expected[LIST_SIZE];
  make_list(expected, 1, 0, 3, (net_t[]){{"New", "password"}});
  CHECK_STORED_LIST(expected);

  /* Bytes after the NUL and in unused slots are not checked. */
  fake_nvs_reset();
  make_list(blob, 1, 0, 7, (net_t[]){{"Net", "pw"}});
  blob[12 + 10] = 'g';
  blob[12 + 33 + 60] = 'h';
  memset(blob + 12 + 98 * 5, 'z', 98);
  put_list(blob, sizeof(blob));
  CHECK_INT(settings_get_wifi_network_count(), 1);
  check_network(0, "Net", "pw");

  /* An empty ssid in a valid list is returned as it is. */
  fake_nvs_reset();
  make_list(blob, 3, 2, 0,
            (net_t[]){{"A", "pa"}, {"", "orphan"}, {"C", "pc"}});
  put_list(blob, sizeof(blob));
  CHECK_INT(settings_get_wifi_network_count(), 3);
  check_network(1, "", "orphan");

  /* 32-byte ssid and 64-byte password fill their fields but for the NUL. */
  fake_nvs_reset();
  char s32[33], p64[65];
  memset(s32, 'S', 32);
  s32[32] = '\0';
  memset(p64, 'P', 64);
  p64[64] = '\0';
  make_list(blob, 8, 7, 7, (net_t[]){{s32, p64}, {"1", ""}, {"2", ""},
                                     {"3", ""}, {"4", ""}, {"5", ""},
                                     {"6", ""}, {s32, p64}});
  put_list(blob, sizeof(blob));
  CHECK_INT(settings_get_wifi_network_count(), 8);
  check_network(7, s32, p64);
}

/* ---- Wi-Fi: the older single network ---- */

static void test_wifi_older_network(void) {
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_put_str(NS, "wifi_pass", "homepass");
  CHECK_INT(settings_get_wifi_network_count(), 1);
  CHECK(settings_has_wifi_credentials());
  check_network(0, "Home", "homepass");
  settings_wifi_network_t n;
  CHECK_INT(settings_get_wifi_network(1, &n), ESP_ERR_NOT_FOUND);
  CHECK_INT(fake_nvs_set_count(), 0);

  /* Without wifi_pass: an open network. */
  fake_nvs_reset();
  fake_nvs_put_str(NS, "wifi_ssid", "Cafe");
  CHECK_INT(settings_get_wifi_network_count(), 1);
  check_network(0, "Cafe", "");

  /* An empty wifi_ssid is no network. */
  fake_nvs_reset();
  fake_nvs_put_str(NS, "wifi_ssid", "");
  fake_nvs_put_str(NS, "wifi_pass", "x");
  check_no_networks();

  /* A set without init starts from the older network. */
  fake_nvs_reset();
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_put_str(NS, "wifi_pass", "homepass");
  CHECK_INT(settings_set_wifi_credentials("Work", "workpass"), ESP_OK);
  uint8_t expected[LIST_SIZE];
  make_list(expected, 2, 1, 0,
            (net_t[]){{"Home", "homepass"}, {"Work", "workpass"}});
  CHECK_STORED_LIST(expected);
  CHECK_STR(stored_str("wifi_ssid"), "Work");
}

static void test_wifi_older_network_too_long(void) {
  /* Spec answer 1: such a network is not there. */
  char s40[41];
  memset(s40, 's', 40);
  s40[40] = '\0';
  fake_nvs_put_str(NS, "wifi_ssid", s40);
  check_no_networks();

  fake_nvs_reset();
  char p70[71];
  memset(p70, 'p', 70);
  p70[70] = '\0';
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_put_str(NS, "wifi_pass", p70);
  check_no_networks();
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 0);
  CHECK_STR(fake_log_text(), "");

  /* 33 bytes is one too many; 32 and 64 are taken. */
  fake_nvs_reset();
  char s33[34];
  memset(s33, 's', 33);
  s33[33] = '\0';
  fake_nvs_put_str(NS, "wifi_ssid", s33);
  check_no_networks();
  fake_nvs_reset();
  char p65[66];
  memset(p65, 'p', 65);
  p65[65] = '\0';
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_put_str(NS, "wifi_pass", p65);
  check_no_networks();
}

static void test_wifi_older_network_at_limits(void) {
  char s32[33], p64[65];
  memset(s32, 's', 32);
  s32[32] = '\0';
  memset(p64, 'p', 64);
  p64[64] = '\0';
  fake_nvs_put_str(NS, "wifi_ssid", s32);
  fake_nvs_put_str(NS, "wifi_pass", p64);
  CHECK_INT(settings_get_wifi_network_count(), 1);
  check_network(0, s32, p64);
  CHECK_INT(settings_init(), ESP_OK);
  uint8_t expected[LIST_SIZE];
  make_list(expected, 1, 0, 0, (net_t[]){{s32, p64}});
  CHECK_STORED_LIST(expected);
}

static void test_wifi_older_network_password_unreadable(void) {
  /* Spec answer 1: any wifi_pass error but "not found" → no network. */
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_put_i32(NS, "wifi_pass", 1234); /* wrong type */
  check_no_networks();
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 0);
  CHECK(!fake_nvs_has_key(NS, "wifi_list"));

  /* A set then starts from an empty list. */
  CHECK_INT(settings_set_wifi_credentials("New", "password"), ESP_OK);
  uint8_t expected[LIST_SIZE];
  make_list(expected, 1, 0, 0, (net_t[]){{"New", "password"}});
  CHECK_STORED_LIST(expected);
}

/* ---- settings_init ---- */

static void test_init_migrates(void) {
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_put_str(NS, "wifi_pass", "homepass");
  CHECK_INT(settings_init(), ESP_OK);
  uint8_t expected[LIST_SIZE];
  make_list(expected, 1, 0, 0, (net_t[]){{"Home", "homepass"}});
  CHECK_STORED_LIST(expected);
  CHECK_HEADER(0x49, 0x46, 0x49, 0x57, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
               0x00, 0x00);
  CHECK_STR(stored_str("wifi_ssid"), "Home");
  CHECK_STR(stored_str("wifi_pass"), "homepass");
  CHECK_INT(fake_nvs_set_count(), 1);
  CHECK_INT(fake_nvs_commit_count(), 1);
  CHECK_STR(fake_log_text(),
            "I settings: Migrated the saved Wi-Fi network into the network "
            "list\n");

  /* A second init finds the valid list and writes nothing. */
  fake_log_clear();
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 1);
  CHECK_STR(fake_log_text(), "");
  CHECK_INT(settings_get_wifi_network_count(), 1);
  check_network(0, "Home", "homepass");
}

static void test_init_migrates_without_password(void) {
  fake_nvs_put_str(NS, "wifi_ssid", "Cafe");
  CHECK_INT(settings_init(), ESP_OK);
  uint8_t expected[LIST_SIZE];
  make_list(expected, 1, 0, 0, (net_t[]){{"Cafe", ""}});
  CHECK_STORED_LIST(expected);
}

static void test_init_blank_writes_nothing(void) {
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 0);
  CHECK(!fake_nvs_has_key(NS, "wifi_list"));
  CHECK_STR(fake_log_text(), "");
  check_no_networks();
}

static void test_init_valid_list_writes_nothing(void) {
  uint8_t blob[LIST_SIZE];
  make_list(blob, 2, 0, 0, (net_t[]){{"A", "pa"}, {"B", "pb"}});
  put_list(blob, sizeof(blob));
  fake_nvs_put_str(NS, "wifi_ssid", "Other");
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 0);
  CHECK_STORED_LIST(blob);
  CHECK_INT(settings_get_wifi_network_count(), 2);
}

static void test_init_open_error(void) {
  fake_nvs_fail_open(ESP_ERR_NVS_NOT_INITIALIZED, false, NVS_READWRITE);
  CHECK_INT(settings_init(), ESP_ERR_NVS_NOT_INITIALIZED);
  fake_nvs_fail_open(ESP_FAIL, true, NVS_READONLY);
  CHECK_INT(settings_init(), ESP_FAIL);
  CHECK_INT(fake_nvs_set_count(), 0);
}

static void test_init_migration_write_error(void) {
  /* Spec answer 2: logged, init still ESP_OK, the next start tries again. */
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_fail_set(ESP_ERR_NVS_NOT_ENOUGH_SPACE, "wifi_list");
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_STR(fake_log_text(),
            "E settings: Wi-Fi network list not saved: "
            "ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
  CHECK(!fake_nvs_has_key(NS, "wifi_list"));
  check_network(0, "Home", "");
  CHECK_INT(settings_get_wifi_network_count(), 1);

  /* The next start. */
  fake_log_clear();
  fake_nvs_fail_set(ESP_OK, NULL);
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_STR(fake_log_text(),
            "I settings: Migrated the saved Wi-Fi network into the network "
            "list\n");
  uint8_t expected[LIST_SIZE];
  make_list(expected, 1, 0, 0, (net_t[]){{"Home", ""}});
  CHECK_STORED_LIST(expected);
}

static void test_init_migration_commit_error(void) {
  fake_nvs_put_str(NS, "wifi_ssid", "Home");
  fake_nvs_fail_commit(ESP_FAIL);
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_STR(fake_log_text(),
            "E settings: Wi-Fi network list not saved: ESP_FAIL\n");
  check_network(0, "Home", "");
}

/* ---- Wi-Fi: NVS errors ---- */

static void test_wifi_errors(void) {
  fake_nvs_fail_open(ESP_ERR_NVS_NOT_ENOUGH_SPACE, false, NVS_READWRITE);
  CHECK_INT(settings_set_wifi_credentials("Net", "password"),
            ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_STR(fake_log_text(), "E settings: Wi-Fi network not saved: "
                             "ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
  fake_nvs_fail_open(ESP_OK, false, NVS_READWRITE);

  const char *keys[] = {"wifi_list", "wifi_ssid", "wifi_pass"};
  for (int i = 0; i < 3; i++) {
    fake_log_clear();
    fake_nvs_fail_set(ESP_ERR_NVS_NOT_ENOUGH_SPACE, keys[i]);
    CHECK_INT(settings_set_wifi_credentials("Net", "password"),
              ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    CHECK_STR(fake_log_text(), "E settings: Wi-Fi network not saved: "
                               "ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
    CHECK_NO_HANDLES();
  }
  fake_nvs_fail_set(ESP_OK, NULL);

  fake_log_clear();
  fake_nvs_fail_commit(ESP_FAIL);
  CHECK_INT(settings_set_wifi_credentials("Net", "password"), ESP_FAIL);
  CHECK_STR(fake_log_text(), "E settings: Wi-Fi network not saved: ESP_FAIL\n");
  fake_nvs_fail_commit(ESP_OK);

  CHECK_INT(settings_set_wifi_credentials("Net", "password"), ESP_OK);
  CHECK_INT(settings_get_wifi_network_count(), 1);

  /* Spec answer 6: NVS that cannot be read, count 0, every index
   * ESP_ERR_NOT_FOUND (a list is saved meanwhile). */
  fake_log_clear();
  fake_nvs_fail_open(ESP_FAIL, false, NVS_READONLY);
  check_no_networks();
  for (size_t i = 0; i <= SETTINGS_MAX_WIFI_NETWORKS; i++) {
    settings_wifi_network_t n;
    CHECK_INT(settings_get_wifi_network(i, &n), ESP_ERR_NOT_FOUND);
  }
  CHECK_STR(fake_log_text(), "");
}

/* ---- device name ---- */

/* Into a buffer of exactly len bytes, so ASan sees any write past it. */
static void check_name(size_t len, const char *expected) {
  char *buf = malloc(len);
  memset(buf, 0x55, len);
  CHECK_INT(settings_get_device_name(buf, len), ESP_OK);
  CHECK(memchr(buf, 0, len) != NULL);
  if (strcmp(buf, expected) != 0) {
    FAIL("name with len %zu is \"%s\", expected \"%s\"", len, buf, expected);
  }
  free(buf);
}

static void test_name_default(void) {
  check_name(65, "ESP32 AirPlay EC8");
  fake_mac_set((const uint8_t[]){0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03}, ESP_OK);
  check_name(65, "ESP32 AirPlay 203");
  fake_mac_set((const uint8_t[]){0, 0, 0, 0, 0xF0, 0x0A}, ESP_OK);
  check_name(65, "ESP32 AirPlay 00A");
  fake_mac_set((const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, ESP_OK);
  check_name(65, "ESP32 AirPlay FFF");

  /* Namespace there, key missing; empty name; wrong type; unreadable. */
  fake_mac_set((const uint8_t[]){0x14, 0xC1, 0x9F, 0x27, 0xBE, 0xC8}, ESP_OK);
  fake_nvs_put_i32(NS, "volume_mdb", 0);
  check_name(65, "ESP32 AirPlay EC8");
  fake_nvs_put_str(NS, "device_name", "");
  check_name(65, "ESP32 AirPlay EC8");
  fake_nvs_put_i32(NS, "device_name", 7);
  check_name(65, "ESP32 AirPlay EC8");
  fake_nvs_put_str(NS, "device_name", "Kitchen");
  fake_nvs_fail_open(ESP_FAIL, true, NVS_READONLY);
  check_name(65, "ESP32 AirPlay EC8");
  CHECK_STR(fake_log_text(), "");
}

static void test_name_default_short_buffers(void) {
  check_name(1, "");
  check_name(2, "E");
  check_name(5, "ESP3");
  check_name(15, "ESP32 AirPlay ");
  check_name(17, "ESP32 AirPlay EC");
  check_name(18, "ESP32 AirPlay EC8");
  check_name(19, "ESP32 AirPlay EC8");
}

static void test_name_mac_error(void) {
  /* Spec answer 3: the MAC counts as all zeros. */
  fake_mac_set((const uint8_t[]){1, 2, 3, 4, 5, 6}, ESP_FAIL);
  check_name(65, "ESP32 AirPlay 000");
}

static void test_name_saved(void) {
  CHECK_INT(settings_set_device_name("Kitchen"), ESP_OK);
  CHECK_STR(stored_str("device_name"), "Kitchen");
  check_name(65, "Kitchen");
  check_name(4, "Kit");

  CHECK_INT(settings_set_device_name("Хол – ъгъл"), ESP_OK);
  check_name(65, "Хол – ъгъл");

  CHECK_INT(settings_set_device_name(""), ESP_OK);
  CHECK_STR(stored_str("device_name"), "");
  check_name(65, "ESP32 AirPlay EC8");

  CHECK_INT(settings_set_device_name("X"), ESP_OK);
  CHECK_INT(settings_set_device_name(NULL), ESP_OK);
  CHECK_STR(stored_str("device_name"), "");
  check_name(65, "ESP32 AirPlay EC8");

  char n64[65], n65[66];
  memset(n64, 'n', 64);
  n64[64] = '\0';
  memset(n65, 'n', 65);
  n65[65] = '\0';
  CHECK_INT(settings_set_device_name(n64), ESP_OK);
  check_name(65, n64);
  int writes = fake_nvs_set_count();
  CHECK_INT(settings_set_device_name(n65), ESP_ERR_INVALID_ARG);
  CHECK_INT(fake_nvs_set_count(), writes);
  CHECK_STR(stored_str("device_name"), n64);
  CHECK_INT(fake_nvs_uncommitted_closes(), 0);
}

static void test_name_cut_utf8(void) {
  /* "Café": C a f C3 A9 */
  fake_nvs_put_str(NS, "device_name", "Caf\xC3\xA9");
  check_name(4, "Caf");
  check_name(5, "Caf");
  check_name(6, "Caf\xC3\xA9");

  /* "€uro": E2 82 AC u r o */
  fake_nvs_put_str(NS, "device_name", "\xE2\x82\xAC" "uro");
  check_name(2, "");
  check_name(3, "");
  check_name(4, "\xE2\x82\xAC");
  check_name(5, "\xE2\x82\xAC" "u");

  /* 4-byte character after one ASCII byte. */
  fake_nvs_put_str(NS, "device_name", "a\xF0\x9F\x8E\xB5" "b");
  check_name(2, "a");
  check_name(3, "a");
  check_name(4, "a");
  check_name(5, "a");
  check_name(6, "a\xF0\x9F\x8E\xB5");

  /* Saved by the older firmware, longer than 64 bytes. */
  char longname[200] = "a";
  for (int i = 0; i < 50; i++) {
    strcat(longname, "\xD0\xB6"); /* "ж" */
  }
  fake_nvs_put_str(NS, "device_name", longname);
  char expected[100] = "a";
  for (int i = 0; i < 31; i++) {
    strcat(expected, "\xD0\xB6");
  }
  check_name(65, expected); /* 63 bytes: the 32nd "ж" would need 65 */

  /* Not UTF-8: cut byte-wise. */
  char junk[80];
  memset(junk, 0x80, 79);
  junk[79] = '\0';
  fake_nvs_put_str(NS, "device_name", junk);
  junk[64] = '\0';
  check_name(65, junk);
}

static void test_name_errors(void) {
  char buf[8];
  CHECK_INT(settings_get_device_name(NULL, 65), ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_get_device_name(buf, 0), ESP_ERR_INVALID_ARG);

  fake_nvs_fail_open(ESP_ERR_NVS_NOT_ENOUGH_SPACE, false, NVS_READWRITE);
  CHECK_INT(settings_set_device_name("Kitchen"), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_STR(fake_log_text(),
            "E settings: Device name not saved: ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
  fake_nvs_fail_open(ESP_OK, false, NVS_READWRITE);

  fake_log_clear();
  fake_nvs_fail_set(ESP_ERR_NVS_NOT_ENOUGH_SPACE, "device_name");
  CHECK_INT(settings_set_device_name("Kitchen"), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_STR(fake_log_text(),
            "E settings: Device name not saved: ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
  fake_nvs_fail_set(ESP_OK, NULL);

  fake_nvs_fail_commit(ESP_FAIL);
  CHECK_INT(settings_set_device_name("Kitchen"), ESP_FAIL);
}

/* ---- volume ---- */

static void test_volume_rounding(void) {
  static const struct {
    float db;
    int32_t mdb;
  } cases[] = {{-15.5f, -15500}, {-12.3456f, -12345}, {-0.0006f, 0},
               {-99.9995f, -99999}, {0.0f, 0}, {-144.0f, -144000},
               {-30.0f, -30000}, {6.25f, 6250}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    fake_nvs_reset();
    CHECK_INT(settings_set_volume(cases[i].db), ESP_OK);
    CHECK_INT(settings_persist_volume(), ESP_OK);
    CHECK_INT(stored_i32("volume_mdb"), cases[i].mdb);
    float db = 1;
    CHECK_INT(settings_get_volume(&db), ESP_OK);
    CHECK(db == cases[i].db);
  }
}

static void test_volume_persist_only_on_change(void) {
  CHECK_INT(settings_set_volume(-20.0f), ESP_OK);
  CHECK_INT(fake_nvs_open_count(), 0); /* RAM only */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 1);
  CHECK_INT(fake_nvs_commit_count(), 1);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 1);

  CHECK_INT(settings_set_volume(-20.0004f), ESP_OK); /* still -20000 */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 1);

  CHECK_INT(settings_set_volume(-21.0f), ESP_OK);
  CHECK_INT(settings_set_volume(-20.0f), ESP_OK); /* back before persist */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 1);

  CHECK_INT(settings_set_volume(-21.0f), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 2);
  CHECK_INT(stored_i32("volume_mdb"), -21000);
  CHECK_INT(fake_nvs_uncommitted_closes(), 0);
  CHECK_STR(fake_log_text(), "");
}

static void test_volume_read_from_nvs(void) {
  fake_nvs_put_i32(NS, "volume_mdb", -15500);
  float db = 0;
  CHECK_INT(settings_get_volume(&db), ESP_OK);
  CHECK(db == -15.5f);
  int opens = fake_nvs_open_count();
  CHECK_INT(settings_get_volume(&db), ESP_OK); /* from RAM now */
  CHECK_INT(fake_nvs_open_count(), opens);
  CHECK(db == -15.5f);
  /* Same as read: nothing written. */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 0);
}

static void test_volume_read_from_nvs_not_exact(void) {
  float db = 0;
  fake_nvs_put_i32(NS, "volume_mdb", -12345);
  CHECK_INT(settings_get_volume(&db), ESP_OK);
  CHECK(fabs(db - -12.345) < 1e-6);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 0);
}

static void test_volume_set_before_any_read(void) {
  /* NVS already holds the value, but it was never read: written again. */
  fake_nvs_put_i32(NS, "volume_mdb", -30000);
  CHECK_INT(settings_set_volume(-30.0f), ESP_OK);
  float db = 0;
  CHECK_INT(settings_get_volume(&db), ESP_OK);
  CHECK(db == -30.0f);
  CHECK_INT(fake_nvs_open_count(), 0);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 1);
}

static void test_volume_nothing_saved(void) {
  float db = 123.0f;
  CHECK_INT(settings_get_volume(&db), ESP_ERR_NVS_NOT_FOUND);
  CHECK(db == 123.0f);
  fake_nvs_put_i32(NS, "out_lat_us", 1); /* namespace there, key missing */
  CHECK_INT(settings_get_volume(&db), ESP_ERR_NVS_NOT_FOUND);
  CHECK(db == 123.0f);
  fake_nvs_put_u8(NS, "volume_mdb", 1); /* wrong type */
  CHECK(settings_get_volume(&db) != ESP_OK);
  CHECK(db == 123.0f);

  fake_nvs_fail_open(ESP_FAIL, false, NVS_READONLY);
  CHECK_INT(settings_get_volume(&db), ESP_FAIL);
  CHECK(db == 123.0f);
  fake_nvs_fail_open(ESP_OK, false, NVS_READONLY);

  /* Saved later (by a set elsewhere): found by the next get. */
  fake_nvs_put_i32(NS, "volume_mdb", -5000);
  CHECK_INT(settings_get_volume(&db), ESP_OK);
  CHECK(db == -5.0f);

  CHECK_INT(settings_get_volume(NULL), ESP_ERR_INVALID_ARG);
  CHECK_STR(fake_log_text(), "");
}

static void test_volume_persist_without_value(void) {
  /* Spec answer 4: nothing to write. */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_open_count(), 0);
  float db;
  CHECK_INT(settings_get_volume(&db), ESP_ERR_NVS_NOT_FOUND);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_set_count(), 0);
}

static void test_volume_persist_errors(void) {
  CHECK_INT(settings_set_volume(-10.0f), ESP_OK);
  fake_nvs_fail_open(ESP_ERR_NVS_NOT_ENOUGH_SPACE, false, NVS_READWRITE);
  CHECK_INT(settings_persist_volume(), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_STR(fake_log_text(),
            "E settings: Volume not saved: ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
  fake_nvs_fail_open(ESP_OK, false, NVS_READWRITE);

  fake_nvs_fail_set(ESP_ERR_NVS_NOT_ENOUGH_SPACE, "volume_mdb");
  CHECK_INT(settings_persist_volume(), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  fake_nvs_fail_set(ESP_OK, NULL);

  fake_nvs_fail_commit(ESP_FAIL);
  CHECK_INT(settings_persist_volume(), ESP_FAIL);
  fake_nvs_fail_commit(ESP_OK);

  /* A failed write is tried again. */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), -10000);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(fake_nvs_commit_count(), 1);
}

static void test_volume_out_of_range(void) {
  /* Spec answer 5: NaN is 0, outside int32_t saturates. */
  CHECK_INT(settings_set_volume(NAN), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), 0);
  CHECK_INT(settings_set_volume(INFINITY), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), INT32_MAX);
  CHECK_INT(settings_set_volume(-1e30f), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), INT32_MIN);
  CHECK_INT(settings_set_volume(1000.0f), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), 1000000);
  /* Products that still fit are the plain cast. */
  CHECK_INT(settings_set_volume(2147483.5f), ESP_OK); /* 2147483520 */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), 2147483520);
  CHECK_INT(settings_set_volume(2147483.75f), ESP_OK); /* rounds to 2^31 */
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), INT32_MAX);
  CHECK_INT(settings_set_volume(-2147483.5f), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), -2147483520);
  CHECK_INT(settings_set_volume(-INFINITY), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), INT32_MIN);
  CHECK_INT(settings_set_volume(-NAN), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), 0);
}

/* ---- speaker audio settings ---- */

static void check_speaker_defaults(void) {
  int32_t us = 7, ref = 7, speaker = 7, trim = 7, delay = 7;
  bool on = true;
  CHECK_INT(settings_get_output_latency_us(&us), ESP_OK);
  CHECK_INT(us, CONFIG_AIRPLAY_OUTPUT_LATENCY_US);
  CHECK_INT(settings_get_loudness(&on, &ref), ESP_OK);
  CHECK(!on);
  CHECK_INT(ref, 0);
  CHECK_INT(settings_get_mc_speaker(&speaker), ESP_OK);
  CHECK_INT(speaker, EXPECTED_MC_SPEAKER);
  CHECK_INT(settings_get_surround_tuning(&trim, &delay), ESP_OK);
  CHECK_INT(trim, 0);
  CHECK_INT(delay, 0);
  CHECK_NO_HANDLES();
}

static void test_speaker_defaults(void) {
  check_speaker_defaults(); /* no namespace */
  fake_nvs_put_str(NS, "device_name", "x");
  check_speaker_defaults(); /* no keys */
  fake_nvs_fail_open(ESP_FAIL, true, NVS_READONLY);
  check_speaker_defaults(); /* NVS cannot be read */
  fake_nvs_fail_open(ESP_OK, true, NVS_READONLY);
  /* Wrong types. */
  fake_nvs_put_u8(NS, "out_lat_us", 1);
  fake_nvs_put_i32(NS, "loud_en", 1);
  fake_nvs_put_str(NS, "loud_ref", "1");
  fake_nvs_put_u8(NS, "mc_speaker", 1);
  fake_nvs_put_u8(NS, "sur_trim", 1);
  fake_nvs_put_u8(NS, "sur_delay", 1);
  check_speaker_defaults();
  CHECK_STR(fake_log_text(), "");
  CHECK_INT(fake_nvs_set_count(), 0);
}

static void test_speaker_set_get(void) {
  CHECK_INT(settings_set_output_latency_us(-100000), ESP_OK);
  CHECK_INT(stored_i32("out_lat_us"), -100000);
  CHECK_INT(settings_set_loudness(true, -12), ESP_OK);
  uint8_t on_byte = 9;
  CHECK(fake_nvs_peek(NS, "loud_en", FAKE_NVS_U8, &on_byte, 1, NULL));
  CHECK_INT(on_byte, 1);
  CHECK_INT(stored_i32("loud_ref"), -12);
  CHECK_INT(settings_set_mc_speaker(-1), ESP_OK);
  CHECK_INT(stored_i32("mc_speaker"), -1);
  CHECK_INT(settings_set_surround_tuning(-35, 2500), ESP_OK);
  CHECK_INT(stored_i32("sur_trim"), -35);
  CHECK_INT(stored_i32("sur_delay"), 2500);
  CHECK_INT(fake_nvs_set_count(), 6);
  CHECK_INT(fake_nvs_commit_count(), 4);
  CHECK_INT(fake_nvs_uncommitted_closes(), 0);

  int32_t us, ref, speaker, trim, delay;
  bool on;
  CHECK_INT(settings_get_output_latency_us(&us), ESP_OK);
  CHECK_INT(us, -100000);
  CHECK_INT(settings_get_loudness(&on, &ref), ESP_OK);
  CHECK(on);
  CHECK_INT(ref, -12);
  CHECK_INT(settings_get_mc_speaker(&speaker), ESP_OK);
  CHECK_INT(speaker, -1);
  CHECK_INT(settings_get_surround_tuning(&trim, &delay), ESP_OK);
  CHECK_INT(trim, -35);
  CHECK_INT(delay, 2500);

  /* No range checks. */
  CHECK_INT(settings_set_output_latency_us(INT32_MIN), ESP_OK);
  CHECK_INT(settings_set_mc_speaker(99), ESP_OK);
  CHECK_INT(settings_set_surround_tuning(INT32_MAX, -1), ESP_OK);
  CHECK_INT(settings_get_output_latency_us(&us), ESP_OK);
  CHECK_INT(us, INT32_MIN);
  CHECK_INT(settings_get_mc_speaker(&speaker), ESP_OK);
  CHECK_INT(speaker, 99);

  CHECK_INT(settings_set_loudness(false, 3), ESP_OK);
  CHECK(fake_nvs_peek(NS, "loud_en", FAKE_NVS_U8, &on_byte, 1, NULL));
  CHECK_INT(on_byte, 0);
  CHECK_INT(settings_get_loudness(&on, &ref), ESP_OK);
  CHECK(!on);
  CHECK_INT(ref, 3);
  CHECK_STR(fake_log_text(), "");
}

static void test_speaker_partial_and_nonzero(void) {
  fake_nvs_put_u8(NS, "loud_en", 2);
  bool on = false;
  int32_t ref = 7, trim = 7, delay = 7;
  CHECK_INT(settings_get_loudness(&on, &ref), ESP_OK);
  CHECK(on);
  CHECK_INT(ref, 0);
  fake_nvs_put_u8(NS, "loud_en", 0xFF);
  CHECK_INT(settings_get_loudness(&on, &ref), ESP_OK);
  CHECK(on);
  fake_nvs_put_i32(NS, "sur_delay", 800);
  CHECK_INT(settings_get_surround_tuning(&trim, &delay), ESP_OK);
  CHECK_INT(trim, 0);
  CHECK_INT(delay, 800);
}

static void test_speaker_one_key_of_two(void) {
  /* Spec answer 7: each key read on its own. */
  bool on = true;
  int32_t ref = 7, trim = 7, delay = 7;
  fake_nvs_put_i32(NS, "loud_ref", -6);
  fake_nvs_put_i32(NS, "sur_trim", -20);
  CHECK_INT(settings_get_loudness(&on, &ref), ESP_OK);
  CHECK(!on);
  CHECK_INT(ref, -6);
  CHECK_INT(settings_get_surround_tuning(&trim, &delay), ESP_OK);
  CHECK_INT(trim, -20);
  CHECK_INT(delay, 0);

  fake_nvs_reset();
  fake_nvs_put_u8(NS, "loud_en", 1);
  fake_nvs_put_i32(NS, "sur_delay", 1500);
  CHECK_INT(settings_get_loudness(&on, &ref), ESP_OK);
  CHECK(on);
  CHECK_INT(ref, 0);
  CHECK_INT(settings_get_surround_tuning(&trim, &delay), ESP_OK);
  CHECK_INT(trim, 0);
  CHECK_INT(delay, 1500);
}

static void test_clear_output_latency(void) {
  CHECK_INT(settings_clear_output_latency(), ESP_OK); /* not there */
  CHECK_INT(settings_set_output_latency_us(12345), ESP_OK);
  CHECK_INT(settings_clear_output_latency(), ESP_OK);
  CHECK(!fake_nvs_has_key(NS, "out_lat_us"));
  int32_t us = 0;
  CHECK_INT(settings_get_output_latency_us(&us), ESP_OK);
  CHECK_INT(us, CONFIG_AIRPLAY_OUTPUT_LATENCY_US);
  CHECK_INT(settings_clear_output_latency(), ESP_OK);
  CHECK_INT(fake_nvs_uncommitted_closes(), 0);
  CHECK_STR(fake_log_text(), "");

  fake_nvs_fail_open(ESP_ERR_NVS_NOT_ENOUGH_SPACE, false, NVS_READWRITE);
  CHECK_INT(settings_clear_output_latency(), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_STR(fake_log_text(), "E settings: Output latency not saved: "
                             "ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
  fake_nvs_fail_open(ESP_OK, false, NVS_READWRITE);
  fake_nvs_fail_commit(ESP_FAIL);
  CHECK_INT(settings_clear_output_latency(), ESP_FAIL);
}

static void test_speaker_errors(void) {
  fake_nvs_fail_open(ESP_ERR_NVS_NOT_ENOUGH_SPACE, false, NVS_READWRITE);
  CHECK_INT(settings_set_output_latency_us(1), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_INT(settings_set_loudness(true, 1), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_INT(settings_set_mc_speaker(1), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_INT(settings_set_surround_tuning(1, 1), ESP_ERR_NVS_NOT_ENOUGH_SPACE);
  CHECK_STR(fake_log_text(),
            "E settings: Output latency not saved: ESP_ERR_NVS_NOT_ENOUGH_SPACE\n"
            "E settings: Loudness not saved: ESP_ERR_NVS_NOT_ENOUGH_SPACE\n"
            "E settings: Speaker not saved: ESP_ERR_NVS_NOT_ENOUGH_SPACE\n"
            "E settings: Surround tuning not saved: "
            "ESP_ERR_NVS_NOT_ENOUGH_SPACE\n");
  fake_nvs_fail_open(ESP_OK, false, NVS_READWRITE);

  const char *keys[] = {"out_lat_us", "loud_en", "loud_ref", "mc_speaker",
                        "sur_trim", "sur_delay"};
  for (int i = 0; i < 6; i++) {
    fake_nvs_fail_set(ESP_ERR_NVS_NOT_ENOUGH_SPACE, keys[i]);
    esp_err_t err = ESP_OK;
    switch (i) {
    case 0: err = settings_set_output_latency_us(1); break;
    case 1:
    case 2: err = settings_set_loudness(true, 1); break;
    case 3: err = settings_set_mc_speaker(1); break;
    default: err = settings_set_surround_tuning(1, 1); break;
    }
    CHECK_INT(err, ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    CHECK_NO_HANDLES();
  }
  fake_nvs_fail_set(ESP_OK, NULL);

  fake_nvs_fail_commit(ESP_FAIL);
  CHECK_INT(settings_set_output_latency_us(1), ESP_FAIL);
  CHECK_INT(settings_set_loudness(true, 1), ESP_FAIL);
  CHECK_INT(settings_set_mc_speaker(1), ESP_FAIL);
  CHECK_INT(settings_set_surround_tuning(1, 1), ESP_FAIL);
}

static void test_speaker_null_arguments(void) {
  bool on;
  int32_t v;
  CHECK_INT(settings_get_output_latency_us(NULL), ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_get_loudness(NULL, &v), ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_get_loudness(&on, NULL), ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_get_mc_speaker(NULL), ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_get_surround_tuning(NULL, &v), ESP_ERR_INVALID_ARG);
  CHECK_INT(settings_get_surround_tuning(&v, NULL), ESP_ERR_INVALID_ARG);
}

/* ---- keys and namespace ---- */

static void test_only_known_keys_in_airplay(void) {
  CHECK_INT(settings_init(), ESP_OK);
  CHECK_INT(settings_set_wifi_credentials("Net", "password"), ESP_OK);
  CHECK_INT(settings_set_device_name("Name"), ESP_OK);
  CHECK_INT(settings_set_volume(-1.0f), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(settings_set_output_latency_us(1), ESP_OK);
  CHECK_INT(settings_set_loudness(true, 1), ESP_OK);
  CHECK_INT(settings_set_mc_speaker(1), ESP_OK);
  CHECK_INT(settings_set_surround_tuning(1, 1), ESP_OK);
  const char *keys[] = {"device_name", "wifi_list",  "wifi_ssid",
                        "wifi_pass",   "volume_mdb", "out_lat_us",
                        "loud_en",     "loud_ref",   "mc_speaker",
                        "sur_trim",    "sur_delay"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (!fake_nvs_has_key(NS, keys[i])) {
      FAIL("key %s missing", keys[i]);
    }
  }
  CHECK_INT(fake_nvs_set_count(), 11);
}

static void test_no_log_on_reads(void) {
  settings_wifi_network_t n;
  char name[65];
  float db;
  int32_t v, w;
  bool on;
  for (int round = 0; round < 3; round++) {
    if (round == 1) {
      uint8_t blob[LIST_SIZE];
      make_list(blob, 1, 0, 0, (net_t[]){{"Net", "pw"}});
      put_list(blob, 795);
      fake_nvs_put_str(NS, "device_name", "\xE2\x82\xAC\xE2\x82\xAC");
    }
    if (round == 2) {
      fake_nvs_fail_open(ESP_FAIL, true, NVS_READONLY);
    }
    (void)settings_get_wifi_network_count();
    (void)settings_has_wifi_credentials();
    (void)settings_get_wifi_network(0, &n);
    (void)settings_get_device_name(name, 4);
    (void)settings_get_volume(&db);
    (void)settings_get_output_latency_us(&v);
    (void)settings_get_loudness(&on, &v);
    (void)settings_get_mc_speaker(&v);
    (void)settings_get_surround_tuning(&v, &w);
    CHECK_NO_HANDLES();
  }
  CHECK_STR(fake_log_text(), "");
  CHECK_INT(fake_nvs_set_count(), 0);
}

/* ---- several threads ---- */

#define THREADS 8
#define ROUNDS 40

static void *wifi_worker(void *arg) {
  int id = (int)(intptr_t)arg;
  char ssid[16], pass[16], name[65];
  for (int r = 0; r < ROUNDS; r++) {
    snprintf(ssid, sizeof(ssid), "T%d-%d", id, r);
    snprintf(pass, sizeof(pass), "pw-%d-%d", id, r);
    if (settings_set_wifi_credentials(ssid, pass) != ESP_OK) {
      abort();
    }
    size_t count = settings_get_wifi_network_count();
    if (count == 0 || count > 8) {
      abort();
    }
    settings_wifi_network_t n;
    for (size_t i = 0; i < 8; i++) {
      esp_err_t err = settings_get_wifi_network(i, &n);
      if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        abort();
      }
      if (err == ESP_OK && (memchr(n.ssid, 0, sizeof(n.ssid)) == NULL ||
                            n.ssid[0] == '\0')) {
        abort();
      }
    }
    if (settings_set_volume(-(float)(id * ROUNDS + r)) != ESP_OK ||
        settings_persist_volume() != ESP_OK) {
      abort();
    }
    float db;
    if (settings_get_volume(&db) != ESP_OK) {
      abort();
    }
    if (settings_get_device_name(name, sizeof(name)) != ESP_OK ||
        strncmp(name, "ESP32 AirPlay", 13) != 0) {
      abort();
    }
    int32_t us;
    (void)settings_get_output_latency_us(&us);
    (void)settings_set_surround_tuning(id, r);
  }
  return NULL;
}

static void *add_one(void *arg) {
  int id = (int)(intptr_t)arg;
  char ssid[8];
  snprintf(ssid, sizeof(ssid), "Only%d", id);
  if (settings_set_wifi_credentials(ssid, "password") != ESP_OK) {
    abort();
  }
  return NULL;
}

static void test_threads(void) {
  CHECK_INT(settings_init(), ESP_OK);

  /* Eight threads add one network each: none may be lost. */
  pthread_t t[THREADS];
  for (int i = 0; i < THREADS; i++) {
    CHECK(pthread_create(&t[i], NULL, add_one, (void *)(intptr_t)i) == 0);
  }
  for (int i = 0; i < THREADS; i++) {
    pthread_join(t[i], NULL);
  }
  CHECK_INT(settings_get_wifi_network_count(), 8);
  for (int id = 0; id < THREADS; id++) {
    char ssid[8];
    snprintf(ssid, sizeof(ssid), "Only%d", id);
    int found = 0;
    for (size_t i = 0; i < 8; i++) {
      settings_wifi_network_t n;
      CHECK_INT(settings_get_wifi_network(i, &n), ESP_OK);
      found += strcmp(n.ssid, ssid) == 0;
    }
    CHECK_INT(found, 1);
  }

  /* Everything at once. */
  for (int i = 0; i < THREADS; i++) {
    CHECK(pthread_create(&t[i], NULL, wifi_worker, (void *)(intptr_t)i) == 0);
  }
  for (int i = 0; i < THREADS; i++) {
    pthread_join(t[i], NULL);
  }
  CHECK_NO_HANDLES();
  CHECK_INT(fake_nvs_uncommitted_closes(), 0);
  CHECK_INT(settings_get_wifi_network_count(), 8);
  uint8_t blob[LIST_SIZE];
  size_t len = 0;
  CHECK(fake_nvs_peek(NS, "wifi_list", FAKE_NVS_BLOB, blob, sizeof(blob),
                      &len));
  CHECK_INT(len, LIST_SIZE);
  /* 8 * 40 sets after 8 adds to a full list: next has gone round. */
  CHECK_INT(blob[10] | (blob[11] << 8), (THREADS * ROUNDS) % 8);
  for (size_t i = 0; i < 8; i++) {
    settings_wifi_network_t a;
    CHECK_INT(settings_get_wifi_network(i, &a), ESP_OK);
    for (size_t j = i + 1; j < 8; j++) {
      settings_wifi_network_t b;
      CHECK_INT(settings_get_wifi_network(j, &b), ESP_OK);
      CHECK(strcmp(a.ssid, b.ssid) != 0);
    }
  }
  /* The volume in NVS is the one in RAM after the last persist. */
  float db;
  CHECK_INT(settings_get_volume(&db), ESP_OK);
  CHECK_INT(settings_persist_volume(), ESP_OK);
  CHECK_INT(stored_i32("volume_mdb"), (int32_t)(db * 1000));
  CHECK_STR(fake_log_text(), "");
}

/* ---- runner ---- */

typedef struct {
  const char *name;
  void (*run)(void);
} test_t;

#define T(fn) {#fn, fn}
static const test_t TESTS[] = {
    T(test_wifi_example_add_and_update),
    T(test_wifi_example_full_list),
    T(test_wifi_full_list_eight_more),
    T(test_wifi_full_list_next_wraps),
    T(test_wifi_update_clears_old_password_bytes),
    T(test_wifi_ssid_compared_byte_for_byte),
    T(test_wifi_arguments),
    T(test_wifi_invalid_lists),
    T(test_wifi_invalid_list_replaced_on_set),
    T(test_wifi_list_wrong_type),
    T(test_wifi_valid_edge_lists),
    T(test_wifi_older_network),
    T(test_wifi_older_network_too_long),
    T(test_wifi_older_network_at_limits),
    T(test_wifi_older_network_password_unreadable),
    T(test_init_migrates),
    T(test_init_migrates_without_password),
    T(test_init_blank_writes_nothing),
    T(test_init_valid_list_writes_nothing),
    T(test_init_open_error),
    T(test_init_migration_write_error),
    T(test_init_migration_commit_error),
    T(test_wifi_errors),
    T(test_name_default),
    T(test_name_default_short_buffers),
    T(test_name_mac_error),
    T(test_name_saved),
    T(test_name_cut_utf8),
    T(test_name_errors),
    T(test_volume_rounding),
    T(test_volume_persist_only_on_change),
    T(test_volume_read_from_nvs),
    T(test_volume_read_from_nvs_not_exact),
    T(test_volume_set_before_any_read),
    T(test_volume_nothing_saved),
    T(test_volume_persist_without_value),
    T(test_volume_persist_errors),
    T(test_volume_out_of_range),
    T(test_speaker_defaults),
    T(test_speaker_set_get),
    T(test_speaker_partial_and_nonzero),
    T(test_speaker_one_key_of_two),
    T(test_clear_output_latency),
    T(test_speaker_errors),
    T(test_speaker_null_arguments),
    T(test_only_known_keys_in_airplay),
    T(test_no_log_on_reads),
    T(test_threads),
};

int main(int argc, char **argv) {
  int failed = 0, run = 0;
  for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
    if (argc > 1 && strstr(TESTS[i].name, argv[1]) == NULL) {
      continue;
    }
    run++;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      fake_nvs_reset();
      TESTS[i].run();
      if (fake_nvs_handles_open() != 0) {
        fprintf(stderr, "  %d NVS handles left open\n",
                fake_nvs_handles_open());
        exit(1);
      }
      exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s %s\n", ok ? "PASS" : "FAIL", TESTS[i].name);
    failed += !ok;
  }
  printf("%d of %d passed\n", run - failed, run);
  return failed ? 1 : 0;
}
