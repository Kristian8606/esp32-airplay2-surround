/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* The rules of the web server that need no ESP-IDF: the JSON answers, the
 * Basic auth check, the download sizes, the reset reason and PHY names.
 * Host tests run them directly. See spec/spec.md. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define WEB_RULES_DOWNLOAD_DEFAULT (1024u * 1024u)    /* 1 MiB */
#define WEB_RULES_DOWNLOAD_MAX (16u * 1024u * 1024u)  /* 16 MiB */
#define WEB_RULES_SSID_MAX 32

/* What GET /api/system/info reports. Strings may be NULL (sent as ""). */
typedef struct {
  const char *ip;
  const char *mac;
  const char *device_name;
  bool wifi_connected;
  const char *role;
  uint32_t free_heap;
  bool have_ap; /* the wifi_* keys below only when true */
  const char *wifi_ssid;
  uint8_t wifi_bssid[6];
  int wifi_rssi;
  int wifi_channel;
  const char *wifi_phy;
  const char *firmware_version;
  const char *firmware_build;
  const char *reset_reason;
  int64_t uptime_s;
} web_rules_system_info_t;

/* {"info":{...},"success":true}; NULL when out of memory. */
cJSON *web_rules_system_info_json(const web_rules_system_info_t *info);

/* {"success":true} */
cJSON *web_rules_ok_json(void);
/* {"success":false,"error":"<error>"} */
cJSON *web_rules_error_json(const char *error);

/* The Wi-Fi scan answer, built in three steps so that the scan records need
 * not be copied: {"networks":[...],"success":true}. ssid is read up to its
 * first NUL, at most ssid_cap bytes and at most WEB_RULES_SSID_MAX. add and
 * end return false when out of memory. */
cJSON *web_rules_scan_begin(void);
bool web_rules_scan_add(cJSON *root, const uint8_t *ssid, size_t ssid_cap,
                        int rssi, int channel);
bool web_rules_scan_end(cJSON *root);

/* The value of "bytes=" (NULL when missing) made into a download size. */
uint32_t web_rules_download_size(const char *value);

/* esp_reset_reason_t (ESP_RST_UNKNOWN = 0 ... ESP_RST_SDIO = 10). */
const char *web_rules_reset_reason_name(int reason);

/* From the scan record's PHY flags. */
const char *web_rules_phy_name(bool phy_11n, bool phy_11g, bool phy_11b);

/* Whether an Authorization header value is Basic auth for user "admin" with
 * this password. The compare takes the same time for any header whose
 * credentials have the same length. */
bool web_rules_basic_auth_ok(const char *header, const char *password);
