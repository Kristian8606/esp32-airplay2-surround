/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Fakes of mdns_*, the settings, Wi-Fi and identity functions. Each records
 * what it was called with; fake_reset() sets the defaults of the example in
 * the spec. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FAKE_MAX_CALLS 16
#define FAKE_MAX_LOGS 16
#define FAKE_LINE_LEN 1024

typedef enum {
  FAKE_HOST_PREFIX,  /* host_prefix, then the name with ' ' made '-' */
  FAKE_HOST_NO_NUL,  /* fills the whole buffer with 'z', no NUL */
} fake_host_mode_t;

typedef struct {
  /* Return values of the mdns_* functions. */
  esp_err_t init_ret;
  esp_err_t hostname_ret;
  esp_err_t airplay_ret;
  esp_err_t raop_ret;

  /* settings_get_device_name(): on ESP_OK copies at most len - 1 bytes of
   * settings_name (cutting like strlcpy) and the NUL; on an error leaves
   * garbage (0xAA, no NUL). settings_no_nul fills the buffer with 'a'. */
  esp_err_t settings_ret;
  const char *settings_name;
  bool settings_no_nul;

  /* wifi_get_mac_str(); mac_no_nul fills the buffer with 'F'. */
  const char *mac;
  bool mac_no_nul;

  /* airplay_get_pairing_id(); pi_no_nul fills the buffer with '9'. */
  const char *pairing_id;
  bool pi_no_nul;

  /* ap2_hap_identity_public_key(). */
  uint8_t public_key[32];

  fake_host_mode_t host_mode;
  const char *host_prefix;
} fake_config_t;

extern fake_config_t fake;

/* Calls in order, one line each, e.g.
 * mdns_service_add("x", "_raop", "_tcp", 7000, am=... vv=2) */
extern char fake_calls[FAKE_MAX_CALLS][FAKE_LINE_LEN];
extern int fake_call_count;

/* Log lines: "<level> <tag>: <text>". */
extern char fake_logs[FAKE_MAX_LOGS][FAKE_LINE_LEN];
extern int fake_log_count;

/* What wifi_sanitize_hostname() got. */
extern char fake_sanitize_name[FAKE_LINE_LEN];
extern size_t fake_sanitize_len;
extern int fake_sanitize_count;

/* What the settings and Wi-Fi functions were given as buffer size. */
extern size_t fake_settings_len;
extern size_t fake_mac_len;
extern size_t fake_pi_len;

void fake_reset(void);
int fake_log_count_level(char level);
