/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* The parts of the Wi-Fi module that do not need ESP-IDF: host names, text
 * forms, choosing a network, the retry times and the scan list of the setup
 * page. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WL_HOSTNAME_MAX_LEN 31 /* = WIFI_HOSTNAME_MAX_LEN */
#define WL_SSID_MAX_LEN 32
#define WL_PASSWORD_MAX_LEN 64
#define WL_MAX_SAVED 8      /* = SETTINGS_MAX_WIFI_NETWORKS */
#define WL_SCAN_LIST_MAX 32 /* entries on the setup page */
#define WL_AP_MAX_CLIENTS 4
#define WL_NO_IP_AP_MS 30000 /* no address this long: access point on */
#define WL_DHCP_TIMEOUT_MS 20000 /* associated, no address: disconnect */
#define WL_SCAN_TIMEOUT_MS 10000 /* a scan is stopped after this long */
#define WL_SCAN_STOP_GRACE_MS 1000 /* then its SCAN_DONE is awaited this long */

#define WL_DEFAULT_AP_SSID "ESP32-AirPlay-Setup"
#define WL_DEFAULT_HOSTNAME_PREFIX "esp32-airplay-"

/* Security of an access point, as far as choosing a network cares. */
typedef enum {
  WL_SEC_OPEN,  /* no security at all */
  WL_SEC_PSK,   /* WPA2-PSK, WPA3-SAE or a mix of them (WPA/WPA2 too) */
  WL_SEC_OTHER, /* WEP, WPA only, enterprise, OWE, WAPI, ... */
} wl_sec_t;

/* One scan result. ssid points to at least WL_SSID_MAX_LEN bytes; it ends at
 * the first NUL or after WL_SSID_MAX_LEN bytes. An empty SSID is a hidden
 * network. */
typedef struct {
  const uint8_t *ssid;
  int8_t rssi;
  uint8_t bssid[6];
  uint8_t channel;
  wl_sec_t sec;
} wl_ap_t;

/* One saved network. ssid ends at the first NUL or after WL_SSID_MAX_LEN
 * bytes. */
typedef struct {
  const char *ssid;
  bool has_password;
} wl_saved_t;

/* SSID length: up to the first NUL, at most WL_SSID_MAX_LEN. NULL gives 0. */
size_t wl_ssid_len(const void *ssid);
/* Whether two SSIDs (as above) are the same bytes. */
bool wl_ssid_equal(const void *a, const void *b);

/* Whether a saved network with or without a password may join an access
 * point of this security: "" only open, a password only WL_SEC_PSK. */
bool wl_sec_allowed(bool has_password, wl_sec_t sec);

/* Host name from a device name (rules in README.md); mac is the station
 * MAC, used when nothing is left of the name. */
void wl_sanitize_hostname(const char *name, const uint8_t mac[6], char *out,
                          size_t out_len);

/* "AA:BB:CC:01:02:03". out NULL: nothing; len < 18: "" (when len > 0). */
void wl_mac_str(const uint8_t mac[6], char *out, size_t len);

/* Dotted IPv4 address; ip in network byte order. Results as for
 * wifi_get_ip_str(). */
esp_err_t wl_ip_str(bool connected, uint32_t ip, char *out, size_t len);

/* a.b.c.d in network byte order, on any host. */
uint32_t wl_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/* Access point SSID and password from the arguments of wifi_init_apsta(). */
typedef struct {
  uint8_t ssid[WL_SSID_MAX_LEN];
  uint8_t ssid_len;
  uint8_t password[WL_PASSWORD_MAX_LEN];
  uint8_t password_len;
  bool wpa2;         /* else open */
  bool bad_ssid;     /* a non-NULL SSID was not 1 to 32 bytes: default used */
  bool bad_password; /* a non-NULL password was not 8 to 63 bytes: open */
} wl_ap_params_t;

void wl_ap_params(const char *ssid, const char *password, wl_ap_params_t *out);

/* The saved network to join: among the scan results whose SSID is saved and
 * whose security fits, the strongest (the first on equal RSSI). Returns its
 * index in aps and sets *saved_index, or -1 when there is none. */
int wl_choose(const wl_ap_t *aps, size_t ap_count, const wl_saved_t *saved,
              size_t saved_count, size_t *saved_index);

/* The setup page list: one entry per SSID (its strongest access point, the
 * first on equal RSSI), no hidden networks, strongest first (scan order on
 * equal RSSI), at most out_max entries. Writes indices into aps to out and
 * returns how many. */
size_t wl_shape_scan(const wl_ap_t *aps, size_t ap_count, uint16_t *out,
                     size_t out_max);

/* What to do after the n-th failed attempt in a row (n >= 1; 0 counts as 1).
 * With a client on the access point the full choice waits 60 s, not 30 s. */
typedef enum {
  WL_RETRY_SAME, /* the same access point again */
  WL_RETRY_FULL, /* scan and choose again */
} wl_retry_action_t;

typedef struct {
  wl_retry_action_t action;
  uint32_t delay_ms;
} wl_retry_t;

wl_retry_t wl_retry_after(unsigned failures, bool ap_has_clients);

/* Name of an ESP-IDF disconnect reason code, "UNKNOWN" for others. */
const char *wl_reason_name(unsigned reason);
