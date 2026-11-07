/* SPDX-License-Identifier: Apache-2.0 */

/* The rules that need no ESP-IDF: JSON answers, Basic auth, download sizes,
 * reset reason and PHY names. */

#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "check.h"
#include "web_rules.h"

int check_failures;
int check_count;

static void check_json(cJSON *j, const char *want) {
  char *text = j != NULL ? cJSON_PrintUnformatted(j) : NULL;
  CHECK_STR(text, want);
  cJSON_free(text);
  cJSON_Delete(j);
}

static void test_download_size(void) {
  CHECK_EQ_INT(web_rules_download_size(NULL), 1048576);
  CHECK_EQ_INT(web_rules_download_size("1048576"), 1048576);   /* spec */
  CHECK_EQ_INT(web_rules_download_size("999999999"), 16777216); /* spec */
  CHECK_EQ_INT(web_rules_download_size("1"), 1);
  CHECK_EQ_INT(web_rules_download_size("2048"), 2048);
  CHECK_EQ_INT(web_rules_download_size("16777216"), 16777216);
  CHECK_EQ_INT(web_rules_download_size("16777217"), 16777216);
  CHECK_EQ_INT(web_rules_download_size("0"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("-0"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("-5"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("-99999999999999999999"), 1048576);
  CHECK_EQ_INT(web_rules_download_size(""), 1048576);
  CHECK_EQ_INT(web_rules_download_size("-"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("abc"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("12abc"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("1.5"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("+5"), 1048576);
  CHECK_EQ_INT(web_rules_download_size(" 5"), 1048576);
  CHECK_EQ_INT(web_rules_download_size("0x10"), 1048576);
  CHECK_EQ_INT(
      web_rules_download_size("123456789012345678901234567890123456789012"),
      16777216);
  CHECK_EQ_INT(web_rules_download_size("000000000000000000000000000000007"), 7);
}

static void test_reset_reason(void) {
  static const char *const want[] = {
      "unknown",  "poweron",   "external",  "software", "panic", "int_wdt",
      "task_wdt", "other_wdt", "deepsleep", "brownout", "sdio",
  };
  for (int i = 0; i <= 10; i++) {
    CHECK_STR(web_rules_reset_reason_name(i), want[i]);
  }
  CHECK_STR(web_rules_reset_reason_name(11), "unknown"); /* ESP_RST_USB */
  CHECK_STR(web_rules_reset_reason_name(15), "unknown");
  CHECK_STR(web_rules_reset_reason_name(-1), "unknown");
  CHECK_STR(web_rules_reset_reason_name(1 << 30), "unknown");
}

static void test_phy_name(void) {
  CHECK_STR(web_rules_phy_name(true, true, true), "11n");
  CHECK_STR(web_rules_phy_name(true, false, false), "11n");
  CHECK_STR(web_rules_phy_name(false, true, true), "11g");
  CHECK_STR(web_rules_phy_name(false, false, true), "11b");
  CHECK_STR(web_rules_phy_name(false, false, false), "?");
}

static bool auth(const char *scheme_and_space, const char *plain,
                 const char *password) {
  char header[1024];
  char *token = b64_encode((const unsigned char *)plain, strlen(plain));
  snprintf(header, sizeof(header), "%s%s", scheme_and_space, token);
  free(token);
  return web_rules_basic_auth_ok(header, password);
}

static void test_basic_auth(void) {
  const char *pw = "s3cret";
  CHECK(auth("Basic ", "admin:s3cret", pw));
  CHECK(auth("basic ", "admin:s3cret", pw)); /* scheme is case-insensitive */
  CHECK(auth("BASIC   ", "admin:s3cret", pw));
  CHECK(web_rules_basic_auth_ok("Basic YWRtaW46czNjcmV0", pw));
  CHECK(web_rules_basic_auth_ok("Basic YWRtaW46czNjcmV0  ", pw));
  /* padding: "admin:pw1" is 9 bytes (none), "admin:pw" 8 (one '='),
   * "admin:p" 7 (two) */
  CHECK(auth("Basic ", "admin:pw1", "pw1"));
  CHECK(auth("Basic ", "admin:pw", "pw"));
  CHECK(auth("Basic ", "admin:p", "p"));
  CHECK(auth("Basic ", "admin:a:b", "a:b")); /* ':' in the password */
  CHECK(auth("Basic ", "admin:p\xc3\xa4ss", "p\xc3\xa4ss"));
  CHECK(auth("Basic ", "admin:", ""));

  CHECK(!auth("Basic ", "admin:s3crex", pw));
  CHECK(!auth("Basic ", "admin:S3cret", pw));
  CHECK(!auth("Basic ", "admin:s3cre", pw));
  CHECK(!auth("Basic ", "admin:s3crett", pw));
  CHECK(!auth("Basic ", "admin:s3cret ", pw));
  CHECK(!auth("Basic ", "Admin:s3cret", pw));
  CHECK(!auth("Basic ", "root:s3cret", pw));
  CHECK(!auth("Basic ", "admins3cret", pw));
  CHECK(!auth("Basic ", ":s3cret", pw));
  CHECK(!auth("Basic ", "admin:", pw));
  CHECK(!auth("Basic ", "", pw));
  CHECK(!auth("Bearer ", "admin:s3cret", pw));
  CHECK(!auth("Basic\t", "admin:s3cret", pw));
  CHECK(!auth("Basic", "admin:s3cret", pw)); /* no space */
  CHECK(!auth("XBasic ", "admin:s3cret", pw));
  CHECK(!auth(" Basic ", "admin:s3cret", pw));

  /* "admin:s\0cret" has the same length as "admin:s3cret" */
  char *token = b64_encode((const unsigned char *)"admin:s\0cret", 12);
  char header[128];
  snprintf(header, sizeof(header), "Basic %s", token);
  free(token);
  CHECK(!web_rules_basic_auth_ok(header, pw));

  CHECK(!web_rules_basic_auth_ok(NULL, pw));
  CHECK(!web_rules_basic_auth_ok("", pw));
  CHECK(!web_rules_basic_auth_ok("Basic", pw));
  CHECK(!web_rules_basic_auth_ok("Basic ", pw));
  CHECK(!web_rules_basic_auth_ok("Basic     ", pw));
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46czNjcmV", pw));   /* len 15 */
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46czNjcmV0=", pw)); /* len 17 */
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46czNj!mV0", pw));
  CHECK(!web_rules_basic_auth_ok("Basic YWRt aW46czNjcmV0", pw));
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46\nczNjcmV0", pw));
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46czNjcmV0\t x", pw));
  CHECK(!web_rules_basic_auth_ok("Basic YW=taW46czNjcmV0", pw));
  CHECK(!web_rules_basic_auth_ok("Basic ====", pw));
  CHECK(!web_rules_basic_auth_ok("Basic ===", pw));
  CHECK(!web_rules_basic_auth_ok("Basic =", pw));
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46cHc=x", "pw"));
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46cA===", "p"));
  CHECK(!web_rules_basic_auth_ok("Basic YWRtaW46czNjcmV0\xff", pw));

  /* A very long header. */
  size_t n = 20000;
  char *big = malloc(n + 1);
  memcpy(big, "Basic ", 6);
  memset(big + 6, 'A', n - 6);
  big[n] = '\0';
  CHECK(!web_rules_basic_auth_ok(big, pw));
  free(big);
}

static void test_system_info_json(void) {
  /* The example of the spec [black box, board]. */
  web_rules_system_info_t in = {
      .ip = "192.168.1.98",
      .mac = "14:C1:9F:27:BE:C8",
      .device_name = "ESP32 AirPlay EC8",
      .wifi_connected = true,
      .role = "gateway",
      .free_heap = 2956072,
      .have_ap = true,
      .wifi_ssid = "UniFi",
      .wifi_bssid = {0x8a, 0x78, 0x48, 0x7e, 0xd0, 0x97},
      .wifi_rssi = -35,
      .wifi_channel = 11,
      .wifi_phy = "11n",
      .firmware_version = "v4.5.61",
      .firmware_build = "v4.5.61 29a5ab1d",
      .reset_reason = "software",
      .uptime_s = 1013,
  };
  check_json(
      web_rules_system_info_json(&in),
      "{\"info\":{\"ip\":\"192.168.1.98\",\"mac\":\"14:C1:9F:27:BE:C8\","
      "\"device_name\":\"ESP32 AirPlay EC8\",\"wifi_connected\":true,"
      "\"role\":\"gateway\",\"free_heap\":2956072,\"wifi_ssid\":\"UniFi\","
      "\"wifi_bssid\":\"8a:78:48:7e:d0:97\",\"wifi_rssi\":-35,"
      "\"wifi_channel\":11,\"wifi_phy\":\"11n\","
      "\"firmware_version\":\"v4.5.61\","
      "\"firmware_build\":\"v4.5.61 29a5ab1d\","
      "\"reset_reason\":\"software\",\"uptime_s\":1013},\"success\":true}");

  web_rules_system_info_t bare = {.free_heap = 0, .uptime_s = 0};
  check_json(web_rules_system_info_json(&bare),
             "{\"info\":{\"ip\":\"\",\"mac\":\"\",\"device_name\":\"\","
             "\"wifi_connected\":false,\"role\":\"\",\"free_heap\":0,"
             "\"firmware_version\":\"\",\"firmware_build\":\"\","
             "\"reset_reason\":\"\",\"uptime_s\":0},\"success\":true}");

  web_rules_system_info_t odd = bare;
  odd.device_name = "a\"b\\c\n";
  odd.free_heap = 4294967295u;
  odd.uptime_s = 4000000000LL;
  cJSON *j = web_rules_system_info_json(&odd);
  char *text = cJSON_PrintUnformatted(j);
  CHECK(strstr(text, "\"device_name\":\"a\\\"b\\\\c\\n\"") != NULL);
  CHECK(strstr(text, "\"free_heap\":4294967295") != NULL);
  CHECK(strstr(text, "\"uptime_s\":4000000000") != NULL);
  cJSON_free(text);
  cJSON_Delete(j);

  CHECK(web_rules_system_info_json(NULL) == NULL);
}

static void test_simple_json(void) {
  check_json(web_rules_ok_json(), "{\"success\":true}");
  check_json(web_rules_error_json("Invalid SSID"),
             "{\"success\":false,\"error\":\"Invalid SSID\"}");
  check_json(web_rules_error_json(NULL), "{\"success\":false,\"error\":\"\"}");
}

static void test_scan_json(void) {
  cJSON *r = web_rules_scan_begin();
  CHECK(web_rules_scan_end(r));
  check_json(r, "{\"networks\":[],\"success\":true}");

  uint8_t full[33];
  memset(full, 'S', sizeof(full)); /* no NUL at all */
  uint8_t empty[33] = {0};
  uint8_t home[33] = "HomeNet";
  r = web_rules_scan_begin();
  CHECK(web_rules_scan_add(r, home, sizeof(home), -40, 1));
  CHECK(web_rules_scan_add(r, empty, sizeof(empty), -90, 13));
  CHECK(web_rules_scan_add(r, full, sizeof(full), -1, 6));
  CHECK(web_rules_scan_add(r, full, 4, -2, 7));
  CHECK(web_rules_scan_add(r, NULL, 0, -3, 8));
  CHECK(web_rules_scan_end(r));
  check_json(r,
             "{\"networks\":[{\"ssid\":\"HomeNet\",\"rssi\":-40,\"channel\":1},"
             "{\"ssid\":\"\",\"rssi\":-90,\"channel\":13},"
             "{\"ssid\":\"SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS\",\"rssi\":-1,"
             "\"channel\":6},"
             "{\"ssid\":\"SSSS\",\"rssi\":-2,\"channel\":7},"
             "{\"ssid\":\"\",\"rssi\":-3,\"channel\":8}],\"success\":true}");

  CHECK(!web_rules_scan_add(NULL, home, sizeof(home), 0, 0));
  CHECK(!web_rules_scan_end(NULL));
  cJSON *not_scan = cJSON_CreateObject();
  CHECK(!web_rules_scan_add(not_scan, home, sizeof(home), 0, 0));
  cJSON_Delete(not_scan);
}

int main(void) {
  RUN(test_download_size);
  RUN(test_reset_reason);
  RUN(test_phy_name);
  RUN(test_basic_auth);
  RUN(test_system_info_json);
  RUN(test_simple_json);
  RUN(test_scan_json);
  CHECK_MAIN_END();
}
