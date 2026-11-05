#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

/* Wi-Fi of both firmwares: the station on a saved network and the setup
 * access point with its captive DNS. See README.md. */

/* Longest DHCP and mDNS host name, without the terminating NUL. */
#define WIFI_HOSTNAME_MAX_LEN 31

/* Starts Wi-Fi once, early in start-up. NULL for both: the default setup
 * access point. */
void wifi_init_apsta(const char *ap_ssid, const char *ap_password);

/* Waits until the station has an IPv4 address. timeout_ms 0: no limit.
 * Returns whether it has one. */
bool wifi_wait_connected(uint32_t timeout_ms);

/* The station MAC as "AA:BB:CC:DD:EE:FF". */
void wifi_get_mac_str(char *mac_str, size_t len);

/* Whether the station has an IPv4 address now. */
bool wifi_is_connected(void);

/* The station's IPv4 address as text. */
esp_err_t wifi_get_ip_str(char *ip_str, size_t len);

/* Scans for networks. On ESP_OK *ap_list is an array of *ap_count records
 * that the caller frees with free(). */
esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count);

/* A device name made into a host name (rules in README.md). */
void wifi_sanitize_hostname(const char *name, char *out, size_t out_len);

/* Sets the station's DHCP host name from a device name. */
void wifi_set_hostname(const char *device_name);
