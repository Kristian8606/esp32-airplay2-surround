/* SPDX-License-Identifier: Apache-2.0 */

#include "web_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mbedtls/base64.h"

#define STR(s) ((s) != NULL ? (s) : "")

static const char *const RESET_REASONS[] = {
    "unknown",  "poweron",  "external",  "software", "panic", "int_wdt",
    "task_wdt", "other_wdt", "deepsleep", "brownout", "sdio",
};

cJSON *web_rules_system_info_json(const web_rules_system_info_t *in) {
  if (in == NULL) {
    return NULL;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON *info = cJSON_AddObjectToObject(root, "info");
  bool ok = info != NULL;
  ok = ok && cJSON_AddStringToObject(info, "ip", STR(in->ip));
  ok = ok && cJSON_AddStringToObject(info, "mac", STR(in->mac));
  ok = ok && cJSON_AddStringToObject(info, "device_name", STR(in->device_name));
  ok = ok && cJSON_AddBoolToObject(info, "wifi_connected", in->wifi_connected);
  ok = ok && cJSON_AddStringToObject(info, "role", STR(in->role));
  ok = ok && cJSON_AddNumberToObject(info, "free_heap", in->free_heap);
  if (in->have_ap) {
    const uint8_t *b = in->wifi_bssid;
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1],
             b[2], b[3], b[4], b[5]);
    ok = ok && cJSON_AddStringToObject(info, "wifi_ssid", STR(in->wifi_ssid));
    ok = ok && cJSON_AddStringToObject(info, "wifi_bssid", bssid);
    ok = ok && cJSON_AddNumberToObject(info, "wifi_rssi", in->wifi_rssi);
    ok = ok && cJSON_AddNumberToObject(info, "wifi_channel", in->wifi_channel);
    ok = ok && cJSON_AddStringToObject(info, "wifi_phy", STR(in->wifi_phy));
  }
  ok = ok && cJSON_AddStringToObject(info, "firmware_version",
                                     STR(in->firmware_version));
  ok = ok && cJSON_AddStringToObject(info, "firmware_build",
                                     STR(in->firmware_build));
  ok = ok && cJSON_AddStringToObject(info, "reset_reason", STR(in->reset_reason));
  ok = ok && cJSON_AddNumberToObject(info, "uptime_s", (double)in->uptime_s);
  ok = ok && cJSON_AddTrueToObject(root, "success");
  if (!ok) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

cJSON *web_rules_ok_json(void) {
  cJSON *root = cJSON_CreateObject();
  if (cJSON_AddTrueToObject(root, "success") == NULL) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

cJSON *web_rules_error_json(const char *error) {
  cJSON *root = cJSON_CreateObject();
  if (cJSON_AddFalseToObject(root, "success") == NULL ||
      cJSON_AddStringToObject(root, "error", STR(error)) == NULL) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

cJSON *web_rules_scan_begin(void) {
  cJSON *root = cJSON_CreateObject();
  if (cJSON_AddArrayToObject(root, "networks") == NULL) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

bool web_rules_scan_add(cJSON *root, const uint8_t *ssid, size_t ssid_cap,
                        int rssi, int channel) {
  cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "networks");
  if (!cJSON_IsArray(list)) {
    return false;
  }
  size_t cap = ssid_cap < WEB_RULES_SSID_MAX ? ssid_cap : WEB_RULES_SSID_MAX;
  size_t len = ssid != NULL ? strnlen((const char *)ssid, cap) : 0;
  char name[WEB_RULES_SSID_MAX + 1];
  if (len > 0) {
    memcpy(name, ssid, len);
  }
  name[len] = '\0';

  cJSON *net = cJSON_CreateObject();
  if (net == NULL || !cJSON_AddItemToArray(list, net)) {
    cJSON_Delete(net);
    return false;
  }
  return cJSON_AddStringToObject(net, "ssid", name) != NULL &&
         cJSON_AddNumberToObject(net, "rssi", rssi) != NULL &&
         cJSON_AddNumberToObject(net, "channel", channel) != NULL;
}

bool web_rules_scan_end(cJSON *root) {
  return root != NULL && cJSON_AddTrueToObject(root, "success") != NULL;
}

uint32_t web_rules_download_size(const char *value) {
  if (value == NULL) {
    return WEB_RULES_DOWNLOAD_DEFAULT;
  }
  const char *p = value;
  bool negative = false;
  if (*p == '-') {
    negative = true;
    p++;
  }
  if (*p < '0' || *p > '9') {
    return WEB_RULES_DOWNLOAD_DEFAULT; /* not a number */
  }
  uint64_t n = 0;
  for (; *p >= '0' && *p <= '9'; p++) {
    if (n <= WEB_RULES_DOWNLOAD_MAX) { /* stops growing; no overflow */
      n = n * 10 + (uint64_t)(*p - '0');
    }
  }
  if (*p != '\0') {
    return WEB_RULES_DOWNLOAD_DEFAULT; /* not a number */
  }
  if (negative || n < 1) {
    return WEB_RULES_DOWNLOAD_DEFAULT;
  }
  if (n > WEB_RULES_DOWNLOAD_MAX) {
    return WEB_RULES_DOWNLOAD_MAX;
  }
  return (uint32_t)n;
}

const char *web_rules_reset_reason_name(int reason) {
  if (reason < 0 ||
      reason >= (int)(sizeof(RESET_REASONS) / sizeof(RESET_REASONS[0]))) {
    return "unknown";
  }
  return RESET_REASONS[reason];
}

const char *web_rules_phy_name(bool phy_11n, bool phy_11g, bool phy_11b) {
  if (phy_11n) {
    return "11n";
  }
  if (phy_11g) {
    return "11g";
  }
  if (phy_11b) {
    return "11b";
  }
  return "?";
}

static bool is_b64_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '+' || c == '/';
}

bool web_rules_basic_auth_ok(const char *header, const char *password) {
  static const char USER[] = "admin:";
  const size_t user_len = sizeof(USER) - 1;

  if (header == NULL) {
    return false;
  }
  if (password == NULL) {
    password = "";
  }
  /* credentials = auth-scheme 1*SP token68 (RFC 7235, 7617); the scheme is
   * case-insensitive. */
  if (strncasecmp(header, "Basic", 5) != 0 || header[5] != ' ') {
    return false;
  }
  const char *token = header + 5;
  while (*token == ' ') {
    token++;
  }
  size_t len = strlen(token);
  while (len > 0 && (token[len - 1] == ' ' || token[len - 1] == '\t')) {
    len--;
  }
  /* Only canonical, padded base64: the decoder is never given whitespace or
   * other characters it might skip. */
  if (len == 0 || len % 4 != 0) {
    return false;
  }
  size_t pad = 0;
  if (token[len - 1] == '=') {
    pad = token[len - 2] == '=' ? 2 : 1;
  }
  for (size_t i = 0; i < len - pad; i++) {
    if (!is_b64_char(token[i])) {
      return false;
    }
  }

  const size_t pass_len = strlen(password);
  const size_t want = user_len + pass_len;
  if (len / 4 * 3 - pad != want) {
    return false; /* only the length is told apart by time */
  }
  const size_t cap = want + 3;
  unsigned char *plain = malloc(cap);
  if (plain == NULL) {
    return false;
  }
  size_t got = 0;
  int ret = mbedtls_base64_decode(plain, cap, &got,
                                  (const unsigned char *)token, len);
  bool ok = false;
  if (ret == 0 && got == want) {
    unsigned char diff = 0;
    for (size_t i = 0; i < user_len; i++) {
      diff |= (unsigned char)(plain[i] ^ (unsigned char)USER[i]);
    }
    for (size_t i = 0; i < pass_len; i++) {
      diff |= (unsigned char)(plain[user_len + i] ^ (unsigned char)password[i]);
    }
    ok = diff == 0;
  }
  volatile unsigned char *wipe = plain;
  for (size_t i = 0; i < cap; i++) {
    wipe[i] = 0;
  }
  free(plain);
  return ok;
}
