/* SPDX-License-Identifier: Apache-2.0 */
/* Host tests of mdns_airplay: the examples of spec/spec.md and broken
 * inputs, with the fakes in fakes/. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakes.h"
#include "mdns_airplay.h"
#include "mdns_airplay_txt.h"

static int failures;
static int checks;

#define CHECK(cond)                                                        \
  do {                                                                     \
    checks++;                                                              \
    if (!(cond)) {                                                         \
      failures++;                                                          \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,     \
              #cond);                                                      \
    }                                                                      \
  } while (0)

#define CHECK_STR(actual, expected)                                        \
  do {                                                                     \
    checks++;                                                              \
    const char *a_ = (actual);                                             \
    const char *e_ = (expected);                                           \
    if (strcmp(a_, e_) != 0) {                                             \
      failures++;                                                          \
      fprintf(stderr, "%s:%d: got\n  [%s]\nexpected\n  [%s]\n", __FILE__,  \
              __LINE__, a_, e_);                                           \
    }                                                                      \
  } while (0)

#define CHECK_CONTAINS(haystack, needle)                                   \
  do {                                                                     \
    checks++;                                                              \
    if (!strstr((haystack), (needle))) {                                   \
      failures++;                                                          \
      fprintf(stderr, "%s:%d: [%s] does not contain [%s]\n", __FILE__,    \
              __LINE__, (haystack), (needle));                             \
    }                                                                      \
  } while (0)

/* ---- Expected values from the spec ---- */

#define PK_EXAMPLE                                                         \
  "01080f161d242b323940474e555c636a71787f868d949ba2a9b0b7bec5ccd3da"

#define AIRPLAY_TXT(deviceid, pk, pi)                                      \
  "deviceid=" deviceid " features=0x401C4A00,0x1C340 flags=0x4"            \
  " model=AudioAccessory6,1 pk=" pk " pi=" pi                              \
  " srcvers=377.40.00 vv=2 acl=0"

#define RAOP_TXT(pk)                                                       \
  "am=AudioAccessory6,1 cn=0,1,2,3 da=true et=0,3,5"                       \
  " ft=0x401C4A00,0x1C340 md=0,2 pk=" pk                                   \
  " sf=0x4 tp=UDP vn=65537 vs=377.40.00 vv=2"

#define EXAMPLE_AIRPLAY_TXT                                                \
  AIRPLAY_TXT("AA:BB:CC:01:02:03", PK_EXAMPLE,                             \
              "11111111-2222-4333-8444-555555555555")
#define EXAMPLE_RAOP_TXT RAOP_TXT(PK_EXAMPLE)

static const char *service_line(const char *instance, const char *type,
                                const char *txt)
{
  static char line[2][FAKE_LINE_LEN];
  static int which;
  which ^= 1;
  snprintf(line[which], sizeof line[which],
           "mdns_service_add(\"%s\", \"%s\", \"_tcp\", 7000, %s)", instance,
           type, txt);
  return line[which];
}

static char *repeat(char *buf, const char *unit, int n)
{
  buf[0] = '\0';
  for (int i = 0; i < n; i++) {
    strcat(buf, unit);
  }
  return buf;
}

/* ---- Spec example ---- */

static void test_spec_example(void)
{
  fake_reset();
  mdns_airplay_init();

  CHECK(fake_call_count == 4);
  CHECK_STR(fake_calls[0], "mdns_init()");
  CHECK_STR(fake_calls[1], "mdns_hostname_set(\"h-Living-Room\")");
  CHECK_STR(fake_calls[2],
            service_line("Living Room", "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3], service_line("AABBCC010203@Living Room", "_raop",
                                        EXAMPLE_RAOP_TXT));

  CHECK(fake_log_count == 1);
  CHECK_STR(fake_logs[0], "I mdns_airplay: AirPlay on mDNS as \"Living Room\""
                          " (h-Living-Room.local, port 7000)");

  /* Buffer sizes given to the firmware functions. */
  CHECK(fake_settings_len == 65);
  CHECK(fake_sanitize_len == 32);
  CHECK_STR(fake_sanitize_name, "Living Room");
  CHECK(fake_mac_len == 18);
  CHECK(fake_pi_len == 37);
}

static void test_board_example(void)
{
  static const uint8_t pk[32] = {
      0xc6, 0x53, 0x9e, 0x94, 0x02, 0x2b, 0x22, 0xc7, 0x38, 0x30, 0x39,
      0x8a, 0x99, 0x46, 0x98, 0x43, 0x8e, 0x61, 0x44, 0x8b, 0x19, 0x55,
      0x21, 0xa9, 0xc7, 0xff, 0x83, 0xf5, 0x85, 0xba, 0xf4, 0x4f};
#define BOARD_PK                                                           \
  "c6539e94022b22c73830398a994698438e61448b195521a9c7ff83f585baf44f"

  fake_reset();
  fake.settings_name = "ESP32 AirPlay EC8";
  fake.mac = "14:C1:9F:27:BE:C8";
  fake.pairing_id = "079479db-7e21-4838-8197-534cbc1bf687";
  memcpy(fake.public_key, pk, sizeof pk);
  fake.host_prefix = "";
  mdns_airplay_init();

  CHECK(fake_call_count == 4);
  CHECK_STR(fake_calls[1], "mdns_hostname_set(\"ESP32-AirPlay-EC8\")");
  CHECK_STR(fake_calls[2],
            service_line("ESP32 AirPlay EC8", "_airplay",
                         AIRPLAY_TXT("14:C1:9F:27:BE:C8", BOARD_PK,
                                     "079479db-7e21-4838-8197-534cbc1bf687")));
  CHECK_STR(fake_calls[3], service_line("14C19F27BEC8@ESP32 AirPlay EC8",
                                        "_raop", RAOP_TXT(BOARD_PK)));
  CHECK_STR(fake_logs[0], "I mdns_airplay: AirPlay on mDNS as "
                          "\"ESP32 AirPlay EC8\" (ESP32-AirPlay-EC8.local,"
                          " port 7000)");
}

/* ---- Cut names ---- */

static void check_names(const char *name, const char *airplay,
                        const char *raop)
{
  char out[MDNS_AIRPLAY_LABEL_MAX + 1];
  mdns_airplay_airplay_instance(name, out);
  CHECK_STR(out, airplay);
  CHECK(strlen(out) <= MDNS_AIRPLAY_LABEL_MAX);
  mdns_airplay_raop_instance("AA:BB:CC:01:02:03", name, out);
  CHECK_STR(out, raop);
  CHECK(strlen(out) <= MDNS_AIRPLAY_LABEL_MAX);
}

static void test_cut_names(void)
{
  char name[256], a[256], r[256], tmp[256];

  /* 80 x n */
  repeat(name, "n", 80);
  repeat(a, "n", 63);
  snprintf(r, sizeof r, "AABBCC010203@%s", repeat(tmp, "n", 50));
  check_names(name, a, r);

  /* 40 x я (80 bytes) */
  repeat(name, "я", 40);
  CHECK(strlen(name) == 80);
  repeat(a, "я", 31);
  CHECK(strlen(a) == 62);
  snprintf(r, sizeof r, "AABBCC010203@%s", repeat(tmp, "я", 25));
  check_names(name, a, r);

  /* x and 40 x я (81 bytes) */
  snprintf(name, sizeof name, "x%s", repeat(tmp, "я", 40));
  CHECK(strlen(name) == 81);
  snprintf(a, sizeof a, "x%s", repeat(tmp, "я", 31));
  CHECK(strlen(a) == 63);
  snprintf(r, sizeof r, "AABBCC010203@x%s", repeat(tmp, "я", 24));
  CHECK(strlen(r) == 13 + 49);
  check_names(name, a, r);

  /* Хол x: as it is */
  CHECK(strlen("Хол x") == 8);
  check_names("Хол x", "Хол x", "AABBCC010203@Хол x");

  /* The fallback name follows the same rules. */
  check_names(MDNS_AIRPLAY_FALLBACK_NAME, "AirPlay Surround Gateway",
              "AABBCC010203@AirPlay Surround Gateway");

  /* Exactly 63 bytes stays whole; 3- and 4-byte sequences. */
  repeat(name, "n", 63);
  check_names(name, name, "AABBCC010203@nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"
                          "nnnnnnnnnnnnnnn");
  repeat(name, "€", 22); /* 66 bytes */
  repeat(a, "€", 21);    /* 63 */
  snprintf(r, sizeof r, "AABBCC010203@%s", repeat(tmp, "€", 16)); /* 48 */
  check_names(name, a, r);
  repeat(name, "😀", 16); /* 64 bytes */
  repeat(a, "😀", 15);    /* 60 */
  snprintf(r, sizeof r, "AABBCC010203@%s", repeat(tmp, "😀", 12)); /* 48 */
  check_names(name, a, r);
}

/* Through mdns_airplay_init(): the 65-byte settings buffer gives at most 64
 * bytes of a long name, and the cut still gives the spec's results. */
static void test_cut_names_through_init(void)
{
  char name[256], a[256], r[256], tmp[256];

  fake_reset();
  fake.settings_name = repeat(name, "n", 80);
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  repeat(a, "n", 63);
  snprintf(r, sizeof r, "AABBCC010203@%s", repeat(tmp, "n", 50));
  CHECK_STR(fake_calls[2], service_line(a, "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3], service_line(r, "_raop", EXAMPLE_RAOP_TXT));

  fake_reset();
  fake.settings_name = repeat(name, "я", 40);
  mdns_airplay_init();
  repeat(a, "я", 31);
  snprintf(r, sizeof r, "AABBCC010203@%s", repeat(tmp, "я", 25));
  CHECK_STR(fake_calls[2], service_line(a, "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3], service_line(r, "_raop", EXAMPLE_RAOP_TXT));

  /* The settings cut this one inside a я: 64 bytes ending in a lead byte. */
  fake_reset();
  snprintf(name, sizeof name, "x%s", repeat(tmp, "я", 40));
  fake.settings_name = name;
  mdns_airplay_init();
  snprintf(a, sizeof a, "x%s", repeat(tmp, "я", 31));
  snprintf(r, sizeof r, "AABBCC010203@x%s", repeat(tmp, "я", 24));
  CHECK_STR(fake_calls[2], service_line(a, "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3], service_line(r, "_raop", EXAMPLE_RAOP_TXT));
}

/* ---- Fallback name ---- */

/* The calls and the success line with the fallback name; the success line
 * is the last log line. */
static void check_fallback(void)
{
  CHECK(fake_call_count == 4);
  CHECK(fake_sanitize_count == 1);
  CHECK_STR(fake_sanitize_name, "");
  CHECK_STR(fake_calls[1], "mdns_hostname_set(\"h-\")");
  CHECK_STR(fake_calls[2], service_line("AirPlay Surround Gateway",
                                        "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3],
            service_line("AABBCC010203@AirPlay Surround Gateway", "_raop",
                         EXAMPLE_RAOP_TXT));
  CHECK(fake_log_count_level('I') == 1);
  CHECK_STR(fake_logs[fake_log_count - 1],
            "I mdns_airplay: AirPlay on mDNS as \"AirPlay Surround Gateway\""
            " (h-.local, port 7000)");
}

static void test_settings_error(void)
{
  fake_reset();
  fake.settings_ret = ESP_ERR_NOT_FOUND; /* leaves 0xAA garbage, no NUL */
  mdns_airplay_init();
  check_fallback();
  CHECK(fake_log_count == 2);
  CHECK_STR(fake_logs[0], "E mdns_airplay: settings_get_device_name failed:"
                          " ESP_ERR_NOT_FOUND");
}

static void test_empty_name(void)
{
  fake_reset();
  fake.settings_name = "";
  mdns_airplay_init();
  check_fallback();
  CHECK(fake_log_count == 1);
  CHECK(fake_log_count_level('E') == 0);
}

/* A settings error together with errors of the mdns_* calls: every error
 * is its own line, and there is no success line. */
static void test_settings_and_mdns_errors(void)
{
  fake_reset();
  fake.settings_ret = ESP_FAIL;
  fake.hostname_ret = ESP_ERR_INVALID_ARG;
  fake.raop_ret = ESP_ERR_NO_MEM;
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK(fake_log_count == 3);
  CHECK(fake_log_count_level('I') == 0);
  CHECK_STR(fake_logs[0],
            "E mdns_airplay: settings_get_device_name failed: ESP_FAIL");
  CHECK_STR(fake_logs[1], "E mdns_airplay: mdns_hostname_set(\"h-\")"
                          " failed: ESP_ERR_INVALID_ARG");
  CHECK_STR(fake_logs[2], "E mdns_airplay: mdns_service_add(_raop._tcp)"
                          " failed: ESP_ERR_NO_MEM");
}

/* mdns_init() fails: the settings are not read, so their error is not
 * logged either. */
static void test_init_error_before_settings(void)
{
  fake_reset();
  fake.init_ret = ESP_ERR_NO_MEM;
  fake.settings_ret = ESP_FAIL;
  mdns_airplay_init();
  CHECK(fake_call_count == 1);
  CHECK(fake_settings_len == 0);
  CHECK(fake_log_count == 1);
  CHECK_STR(fake_logs[0], "E mdns_airplay: mdns_init failed: ESP_ERR_NO_MEM");
}

/* ---- Errors of the mdns_* calls ---- */

static void test_init_error(void)
{
  fake_reset();
  fake.init_ret = ESP_FAIL;
  mdns_airplay_init();
  CHECK(fake_call_count == 1);
  CHECK_STR(fake_calls[0], "mdns_init()");
  CHECK(fake_log_count == 1);
  CHECK_STR(fake_logs[0], "E mdns_airplay: mdns_init failed: ESP_FAIL");
  CHECK(fake_sanitize_count == 0);
}

static void test_hostname_error(void)
{
  fake_reset();
  fake.hostname_ret = ESP_ERR_INVALID_ARG;
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK_STR(fake_calls[2],
            service_line("Living Room", "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3], service_line("AABBCC010203@Living Room", "_raop",
                                        EXAMPLE_RAOP_TXT));
  CHECK(fake_log_count == 1);
  CHECK_STR(fake_logs[0], "E mdns_airplay: mdns_hostname_set(\"h-Living-Room\")"
                          " failed: ESP_ERR_INVALID_ARG");
}

static void test_airplay_add_error(void)
{
  fake_reset();
  fake.airplay_ret = ESP_ERR_NO_MEM;
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK_STR(fake_calls[3], service_line("AABBCC010203@Living Room", "_raop",
                                        EXAMPLE_RAOP_TXT));
  CHECK(fake_log_count == 1);
  CHECK_STR(fake_logs[0], "E mdns_airplay: mdns_service_add(_airplay._tcp)"
                          " failed: ESP_ERR_NO_MEM");
}

static void test_raop_add_error(void)
{
  fake_reset();
  fake.raop_ret = ESP_FAIL;
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK(fake_log_count == 1);
  CHECK_STR(fake_logs[0], "E mdns_airplay: mdns_service_add(_raop._tcp)"
                          " failed: ESP_FAIL");
}

static void test_all_errors_after_init(void)
{
  fake_reset();
  fake.hostname_ret = ESP_FAIL;
  fake.airplay_ret = ESP_ERR_INVALID_ARG;
  fake.raop_ret = ESP_ERR_NO_MEM;
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK(fake_log_count == 3);
  CHECK(fake_log_count_level('E') == 3);
  CHECK(fake_log_count_level('I') == 0);
  CHECK_CONTAINS(fake_logs[0], "mdns_hostname_set");
  CHECK_CONTAINS(fake_logs[0], "ESP_FAIL");
  CHECK_CONTAINS(fake_logs[1], "_airplay._tcp");
  CHECK_CONTAINS(fake_logs[1], "ESP_ERR_INVALID_ARG");
  CHECK_CONTAINS(fake_logs[2], "_raop._tcp");
  CHECK_CONTAINS(fake_logs[2], "ESP_ERR_NO_MEM");
}

/* ---- Names with '=', '.' and spaces ---- */

static void test_special_characters(void)
{
  fake_reset();
  fake.settings_name = "a=b. c d.e=f ";
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK_STR(fake_calls[2],
            service_line("a=b. c d.e=f ", "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3], service_line("AABBCC010203@a=b. c d.e=f ", "_raop",
                                        EXAMPLE_RAOP_TXT));
  CHECK_STR(fake_sanitize_name, "a=b. c d.e=f ");

  fake_reset();
  fake.settings_name = "...";
  mdns_airplay_init();
  CHECK_STR(fake_calls[2], service_line("...", "_airplay", EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3],
            service_line("AABBCC010203@...", "_raop", EXAMPLE_RAOP_TXT));
}

/* ---- Broken inputs ---- */

static void test_settings_without_nul(void)
{
  char a[128];
  fake_reset();
  fake.settings_no_nul = true; /* 65 x 'a' */
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK(strlen(fake_sanitize_name) == 64);
  CHECK_STR(fake_calls[2],
            service_line(repeat(a, "a", 63), "_airplay", EXAMPLE_AIRPLAY_TXT));
}

static void test_mac_broken(void)
{
  char r[128], tmp[128];

  fake_reset();
  fake.mac = "";
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK_STR(fake_calls[2],
            service_line("Living Room", "_airplay",
                         AIRPLAY_TXT("", PK_EXAMPLE,
                                     "11111111-2222-4333-8444-555555555555")));
  CHECK_STR(fake_calls[3], service_line("@Living Room", "_raop",
                                        EXAMPLE_RAOP_TXT));

  /* No NUL: 17 x 'F' are kept, the name part shrinks to fit 63 bytes. */
  fake_reset();
  fake.mac_no_nul = true;
  fake.settings_name = repeat(tmp, "n", 64);
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  snprintf(r, sizeof r, "FFFFFFFFFFFFFFFFF@%s", repeat(tmp, "n", 45));
  CHECK(strlen(r) == 63);
  CHECK_CONTAINS(fake_calls[2], "deviceid=FFFFFFFFFFFFFFFFF ");
  CHECK_STR(fake_calls[3], service_line(r, "_raop", EXAMPLE_RAOP_TXT));

  /* Lower case and no colons: passed as it is. */
  fake_reset();
  fake.mac = "aabbcc010203";
  mdns_airplay_init();
  CHECK_CONTAINS(fake_calls[2], "deviceid=aabbcc010203 ");
  CHECK_STR(fake_calls[3], service_line("aabbcc010203@Living Room", "_raop",
                                        EXAMPLE_RAOP_TXT));

  /* Longer than a MAC: wifi_get_mac_str() is given 18 bytes. */
  fake_reset();
  fake.mac = "AA:BB:CC:01:02:03:04:05";
  mdns_airplay_init();
  CHECK_CONTAINS(fake_calls[2], "deviceid=AA:BB:CC:01:02:03 ");
}

static void test_pairing_id_without_nul(void)
{
  char pi[64];
  fake_reset();
  fake.pi_no_nul = true;
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  char expected[96];
  snprintf(expected, sizeof expected, " pi=%s ", repeat(pi, "9", 36));
  CHECK_CONTAINS(fake_calls[2], expected);
}

static void test_hostname_without_nul(void)
{
  char host[64];
  fake_reset();
  fake.host_mode = FAKE_HOST_NO_NUL;
  mdns_airplay_init();
  char expected[96];
  snprintf(expected, sizeof expected, "mdns_hostname_set(\"%s\")",
           repeat(host, "z", 31));
  CHECK_STR(fake_calls[1], expected);
}

static void test_invalid_utf8(void)
{
  char name[128], out[MDNS_AIRPLAY_LABEL_MAX + 1], a[128];

  /* Only continuation bytes past the limit: the cut leaves nothing, so the
   * fallback name is used for that instance. */
  memset(name, 0x80, 64);
  name[64] = '\0';
  mdns_airplay_airplay_instance(name, out);
  CHECK_STR(out, "AirPlay Surround Gateway");
  mdns_airplay_raop_instance("AA:BB:CC:01:02:03", name, out);
  CHECK_STR(out, "AABBCC010203@AirPlay Surround Gateway");

  /* Empty after the _raop cut only: each instance on its own. */
  memset(a, 0x80, 50);
  a[50] = '\0';
  snprintf(name, sizeof name, "a%sbbbbbbbbbbbbb", a);
  CHECK(strlen(name) == 64);
  mdns_airplay_airplay_instance(name, out);
  CHECK(strlen(out) == 63);
  CHECK(strncmp(out, name, 63) == 0);
  mdns_airplay_raop_instance("AA:BB:CC:01:02:03", name, out);
  CHECK_STR(out, "AABBCC010203@AirPlay Surround Gateway");

  /* With an empty MAC the name part may have 62 bytes. */
  mdns_airplay_raop_instance("", name, out);
  CHECK(strlen(out) == 63);
  CHECK(out[0] == '@' && strncmp(out + 1, name, 62) == 0);

  /* A lead byte without its continuation bytes stays at the end. */
  snprintf(name, sizeof name, "%s\xE0" "b", repeat(a, "a", 62));
  mdns_airplay_airplay_instance(name, out);
  snprintf(a, sizeof a, "%s\xE0", repeat(name, "a", 62));
  CHECK_STR(out, a);

  /* 0xFF and other bytes UTF-8 never uses are passed as they are. */
  mdns_airplay_airplay_instance("\xFF\xFE x", out);
  CHECK_STR(out, "\xFF\xFE x");

  /* An uncut name of continuation bytes only is passed as it is. */
  mdns_airplay_airplay_instance("\x80\x80", out);
  CHECK_STR(out, "\x80\x80");

  /* Through init, with the settings' 64 bytes of 0x80: both instances fall
   * back, the host name is made from the bytes as read. */
  fake_reset();
  memset(name, 0x80, 80);
  name[80] = '\0';
  fake.settings_name = name;
  mdns_airplay_init();
  CHECK(fake_call_count == 4);
  CHECK(strlen(fake_sanitize_name) == 64);
  CHECK_STR(fake_calls[2], service_line("AirPlay Surround Gateway", "_airplay",
                                        EXAMPLE_AIRPLAY_TXT));
  CHECK_STR(fake_calls[3],
            service_line("AABBCC010203@AirPlay Surround Gateway", "_raop",
                         EXAMPLE_RAOP_TXT));
  CHECK(fake_log_count == 1);
  CHECK_CONTAINS(fake_logs[0], "as \"AirPlay Surround Gateway\" (");
}

/* ---- The pure part on its own ---- */

static void test_utf8_prefix_len(void)
{
  CHECK(mdns_airplay_utf8_prefix_len("", 0) == 0);
  CHECK(mdns_airplay_utf8_prefix_len("abc", 0) == 0);
  CHECK(mdns_airplay_utf8_prefix_len("abc", 3) == 3);
  CHECK(mdns_airplay_utf8_prefix_len("abc", 2) == 2);
  CHECK(mdns_airplay_utf8_prefix_len("я", 1) == 0);
  CHECK(mdns_airplay_utf8_prefix_len("aя", 2) == 1);
  CHECK(mdns_airplay_utf8_prefix_len("€", 2) == 0);
  CHECK(mdns_airplay_utf8_prefix_len("😀x", 4) == 4);
  CHECK(mdns_airplay_utf8_prefix_len("😀x", 3) == 0);
}

static void test_build_txt(void)
{
  uint8_t pk[32];
  for (int i = 0; i < 32; i++) {
    pk[i] = (uint8_t)(i * 7 + 1);
  }
  mdns_airplay_txt_records_t rec;
  mdns_airplay_build_txt(&rec, "AA:BB:CC:01:02:03", pk,
                         "11111111-2222-4333-8444-555555555555");
  CHECK_STR(rec.pk, PK_EXAMPLE);
  CHECK_STR(rec.features, "0x401C4A00,0x1C340");
  CHECK_STR(rec.flags, "0x4");
  CHECK_STR(rec.vv, "2");

  static const char *airplay_keys[] = {"deviceid", "features", "flags",
                                       "model",    "pk",       "pi",
                                       "srcvers",  "vv",       "acl"};
  static const char *raop_keys[] = {"am", "cn", "da", "et", "ft", "md",
                                    "pk", "sf", "tp", "vn", "vs", "vv"};
  for (int i = 0; i < MDNS_AIRPLAY_AIRPLAY_TXT_COUNT; i++) {
    CHECK_STR(rec.airplay[i].key, airplay_keys[i]);
  }
  for (int i = 0; i < MDNS_AIRPLAY_RAOP_TXT_COUNT; i++) {
    CHECK_STR(rec.raop[i].key, raop_keys[i]);
  }
  CHECK_STR(rec.airplay[3].value, "AudioAccessory6,1");
  CHECK_STR(rec.raop[9].value, "65537");

  /* Over-long MAC and pairing id are cut to their buffers. */
  mdns_airplay_build_txt(&rec, "AA:BB:CC:01:02:03:04:05:06", pk,
                         "11111111-2222-4333-8444-555555555555-extra");
  CHECK_STR(rec.deviceid, "AA:BB:CC:01:02:03");
  CHECK_STR(rec.pi, "11111111-2222-4333-8444-555555555555");
}

static void test_null_names(void)
{
  char out[MDNS_AIRPLAY_LABEL_MAX + 1];
  mdns_airplay_airplay_instance(NULL, out);
  CHECK_STR(out, "");
  mdns_airplay_raop_instance(NULL, NULL, out);
  CHECK_STR(out, "@");
}

int main(void)
{
  test_spec_example();
  test_board_example();
  test_cut_names();
  test_cut_names_through_init();
  test_settings_error();
  test_empty_name();
  test_settings_and_mdns_errors();
  test_init_error_before_settings();
  test_init_error();
  test_hostname_error();
  test_airplay_add_error();
  test_raop_add_error();
  test_all_errors_after_init();
  test_special_characters();
  test_settings_without_nul();
  test_mac_broken();
  test_pairing_id_without_nul();
  test_hostname_without_nul();
  test_invalid_utf8();
  test_utf8_prefix_len();
  test_build_txt();
  test_null_names();
  fake_reset();

  printf("%d checks, %d failed\n", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
