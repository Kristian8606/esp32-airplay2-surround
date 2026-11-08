#include "airplay_identity.h"

#include <stdio.h>

#include "ap2_pairings.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "sodium.h"

void airplay_get_pairing_id(char *out, size_t len) {
  if (!out || len == 0) {
    return;
  }
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  /* Same MAC -> same UUID on every boot; different devices -> different
   * UUIDs. Hash so the UUID does not expose the MAC directly. */
  static const char kDomain[] = "airplay-esp32 pairing id";
  uint8_t hash[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state st;
  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, (const uint8_t *)kDomain, sizeof(kDomain) - 1);
  crypto_hash_sha512_update(&st, mac, sizeof(mac));
  crypto_hash_sha512_final(&st, hash);

  uint8_t *u = hash; /* first 16 bytes form the UUID */
  u[6] = (uint8_t)((u[6] & 0x0F) | 0x40); /* version 4 layout */
  u[8] = (uint8_t)((u[8] & 0x3F) | 0x80); /* RFC 4122 variant */

  snprintf(out, len,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
           u[11], u[12], u[13], u[14], u[15]);
}

void airplay_get_serial_number(char *out, size_t len) {
  if (!out || len == 0) return;
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(out, len, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4],
           m[5]);
}

void airplay_get_firmware_revision(char *out, size_t len) {
  if (!out || len == 0) return;
  const char *v = esp_app_get_description()->version;
  if (v[0] == 'v' || v[0] == 'V') v++;
  snprintf(out, len, "%s", v);
}

static volatile bool s_session_active;

bool airplay_set_session_active(bool active) {
  if (s_session_active == active) return false;
  s_session_active = active;
  return true;
}

uint32_t airplay_status_flags(void) {
  uint32_t f = AIRPLAY_STATUS_FLAGS;
#ifdef CONFIG_AIRPLAY_HOMEKIT
  if (s_session_active) f |= AIRPLAY_STATUS_RELAY;
  if (ap2_pairings_has_admin()) f |= AIRPLAY_STATUS_HOMEKIT;
#endif
  return f;
}
