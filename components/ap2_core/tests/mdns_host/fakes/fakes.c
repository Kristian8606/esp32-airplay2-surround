/* SPDX-License-Identifier: Apache-2.0 */
#include "fakes.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_identity.h"
#include "ap2_hap.h"
#include "esp_log.h"
#include "mdns.h"
#include "settings.h"
#include "wifi.h"

fake_config_t fake;
char fake_calls[FAKE_MAX_CALLS][FAKE_LINE_LEN];
int fake_call_count;
char fake_logs[FAKE_MAX_LOGS][FAKE_LINE_LEN];
int fake_log_count;
char fake_sanitize_name[FAKE_LINE_LEN];
size_t fake_sanitize_len;
int fake_sanitize_count;
size_t fake_settings_len;
size_t fake_mac_len;
size_t fake_pi_len;

/* The key is handed out in a heap block of exactly 32 bytes, so that the
 * address sanitizer sees any read past it. */
static uint8_t *public_key_block;

void fake_reset(void)
{
  memset(&fake, 0, sizeof fake);
  fake.settings_name = "Living Room";
  fake.mac = "AA:BB:CC:01:02:03";
  fake.pairing_id = "11111111-2222-4333-8444-555555555555";
  for (int i = 0; i < 32; i++) {
    fake.public_key[i] = (uint8_t)(i * 7 + 1);
  }
  fake.host_mode = FAKE_HOST_PREFIX;
  fake.host_prefix = "h-";

  memset(fake_calls, 0, sizeof fake_calls);
  fake_call_count = 0;
  memset(fake_logs, 0, sizeof fake_logs);
  fake_log_count = 0;
  memset(fake_sanitize_name, 0, sizeof fake_sanitize_name);
  fake_sanitize_len = 0;
  fake_sanitize_count = 0;
  fake_settings_len = 0;
  fake_mac_len = 0;
  fake_pi_len = 0;

  free(public_key_block);
  public_key_block = NULL;
}

int fake_log_count_level(char level)
{
  int n = 0;
  for (int i = 0; i < fake_log_count; i++) {
    if (fake_logs[i][0] == level) {
      n++;
    }
  }
  return n;
}

static char *next_call(void)
{
  if (fake_call_count >= FAKE_MAX_CALLS) {
    fprintf(stderr, "fake: too many calls\n");
    abort();
  }
  return fake_calls[fake_call_count++];
}

/* Appends to a call line; aborts rather than cut it. */
static void append(char *line, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void append(char *line, const char *fmt, ...)
{
  size_t used = strlen(line);
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line + used, FAKE_LINE_LEN - used, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= FAKE_LINE_LEN - used) {
    fprintf(stderr, "fake: call line too long\n");
    abort();
  }
}

/* ---- esp_err / esp_log ---- */

const char *esp_err_to_name(esp_err_t code)
{
  switch (code) {
  case ESP_OK:
    return "ESP_OK";
  case ESP_FAIL:
    return "ESP_FAIL";
  case ESP_ERR_NO_MEM:
    return "ESP_ERR_NO_MEM";
  case ESP_ERR_INVALID_ARG:
    return "ESP_ERR_INVALID_ARG";
  case ESP_ERR_INVALID_STATE:
    return "ESP_ERR_INVALID_STATE";
  case ESP_ERR_INVALID_SIZE:
    return "ESP_ERR_INVALID_SIZE";
  case ESP_ERR_NOT_FOUND:
    return "ESP_ERR_NOT_FOUND";
  default:
    return "UNKNOWN ERROR";
  }
}

void fake_log(char level, const char *tag, const char *fmt, ...)
{
  if (fake_log_count >= FAKE_MAX_LOGS) {
    fprintf(stderr, "fake: too many log lines\n");
    abort();
  }
  char *line = fake_logs[fake_log_count++];
  int n = snprintf(line, FAKE_LINE_LEN, "%c %s: ", level, tag);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, FAKE_LINE_LEN - (size_t)n, fmt, ap);
  va_end(ap);
}

/* ---- mdns ---- */

esp_err_t mdns_init(void)
{
  append(next_call(), "mdns_init()");
  return fake.init_ret;
}

esp_err_t mdns_hostname_set(const char *hostname)
{
  char *line = next_call();
  if (!hostname) {
    append(line, "mdns_hostname_set(NULL)");
  } else {
    append(line, "mdns_hostname_set(\"%s\")", hostname);
  }
  return fake.hostname_ret;
}

esp_err_t mdns_service_add(const char *instance_name, const char *service_type,
                           const char *proto, uint16_t port,
                           mdns_txt_item_t txt[], size_t num_items)
{
  char *line = next_call();
  append(line, "mdns_service_add(\"%s\", \"%s\", \"%s\", %u,",
         instance_name ? instance_name : "(null)",
         service_type ? service_type : "(null)", proto ? proto : "(null)",
         (unsigned)port);
  /* The component copies the TXT records here, as this fake does. */
  for (size_t i = 0; i < num_items; i++) {
    append(line, " %s=%s", txt[i].key ? txt[i].key : "(null)",
           txt[i].value ? txt[i].value : "(null)");
  }
  append(line, ")");
  if (service_type && strcmp(service_type, "_raop") == 0) {
    return fake.raop_ret;
  }
  return fake.airplay_ret;
}

/* ---- settings ---- */

esp_err_t settings_get_device_name(char *name, size_t len)
{
  fake_settings_len = len;
  /* Touch the whole buffer the caller claims to have. */
  memset(name, 0xAA, len);
  if (fake.settings_ret != ESP_OK) {
    return fake.settings_ret;
  }
  if (fake.settings_no_nul) {
    memset(name, 'a', len);
    return ESP_OK;
  }
  size_t n = strlen(fake.settings_name);
  if (n > len - 1) {
    n = len - 1;
  }
  memcpy(name, fake.settings_name, n);
  name[n] = '\0';
  return ESP_OK;
}

/* ---- wifi ---- */

void wifi_get_mac_str(char *mac_str, size_t len)
{
  fake_mac_len = len;
  memset(mac_str, 0xAA, len);
  if (fake.mac_no_nul) {
    memset(mac_str, 'F', len);
    return;
  }
  snprintf(mac_str, len, "%s", fake.mac);
}

void wifi_sanitize_hostname(const char *name, char *out, size_t out_len)
{
  fake_sanitize_count++;
  snprintf(fake_sanitize_name, sizeof fake_sanitize_name, "%s", name);
  fake_sanitize_len = out_len;
  memset(out, 0xAA, out_len);
  if (fake.host_mode == FAKE_HOST_NO_NUL) {
    memset(out, 'z', out_len);
    return;
  }
  size_t used = (size_t)snprintf(out, out_len, "%s", fake.host_prefix);
  for (size_t i = 0; name[i] != '\0' && used + 1 < out_len; i++) {
    out[used++] = name[i] == ' ' ? '-' : name[i];
  }
  out[used < out_len ? used : out_len - 1] = '\0';
}

/* ---- identity ---- */

void airplay_get_pairing_id(char *out, size_t len)
{
  fake_pi_len = len;
  memset(out, 0xAA, len);
  if (fake.pi_no_nul) {
    memset(out, '9', len);
    return;
  }
  snprintf(out, len, "%s", fake.pairing_id);
}

const uint8_t *ap2_hap_identity_public_key(void)
{
  free(public_key_block);
  public_key_block = malloc(32);
  if (!public_key_block) {
    abort();
  }
  memcpy(public_key_block, fake.public_key, 32);
  return public_key_block;
}
