/* SPDX-License-Identifier: Apache-2.0 */
#include "mdns_airplay.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ap2_hap.h"
#include "airplay_identity.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mdns.h"
#include "mdns_airplay_txt.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "mdns_airplay";
static bool s_ready;
static char s_flags[12];
static char s_gid[48];
#ifdef CONFIG_AIRPLAY_HOMEKIT
static bool s_contains_leader;
static bool s_flags_valid = true;
static bool s_group_valid = true;
#endif

_Static_assert(sizeof(mdns_txt_item_t) == sizeof(mdns_airplay_txt_t),
               "mdns_txt_item_t layout changed");

static esp_err_t add_service(const char *instance, const char *type,
                             const mdns_airplay_txt_t *txt, size_t count)
{
  mdns_txt_item_t items[MDNS_AIRPLAY_AIRPLAY_TXT_COUNT > MDNS_AIRPLAY_RAOP_TXT_COUNT
                            ? MDNS_AIRPLAY_AIRPLAY_TXT_COUNT : MDNS_AIRPLAY_RAOP_TXT_COUNT];
  if (count > sizeof(items) / sizeof(items[0])) return ESP_ERR_INVALID_SIZE;
  for (size_t i = 0; i < count; i++) {
    items[i].key = txt[i].key;
    items[i].value = txt[i].value;
  }
  return mdns_service_add(instance, type, "_tcp", MDNS_AIRPLAY_PORT, items,
                          count);
}

void mdns_airplay_init(void)
{
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
    return;
  }

  /* On an error the settings leave the buffer undefined: use "" for the
   * host name and the fallback for the instance names. */
  char name[MDNS_AIRPLAY_DEVICE_NAME_LEN] = {0};
  const char *instance_name = name;
  err = settings_get_device_name(name, sizeof name);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "settings_get_device_name failed: %s",
             esp_err_to_name(err));
    name[0] = '\0';
  }
  name[sizeof name - 1] = '\0';
  if (name[0] == '\0') {
    instance_name = MDNS_AIRPLAY_FALLBACK_NAME;
  }

  bool all_ok = true;

  char host[WIFI_HOSTNAME_MAX_LEN + 1] = {0};
  wifi_sanitize_hostname(name, host, sizeof host);
  host[sizeof host - 1] = '\0';
  err = mdns_hostname_set(host);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_hostname_set(\"%s\") failed: %s", host,
             esp_err_to_name(err));
    all_ok = false;
  }

  char mac[MDNS_AIRPLAY_MAC_STR_LEN] = {0};
  wifi_get_mac_str(mac, sizeof mac);
  mac[sizeof mac - 1] = '\0';

  char pairing_id[AIRPLAY_PAIRING_ID_LEN] = {0};
  airplay_get_pairing_id(pairing_id, sizeof pairing_id);
  pairing_id[sizeof pairing_id - 1] = '\0';

  mdns_airplay_txt_records_t txt;
  mdns_airplay_build_txt(&txt, mac, ap2_hap_identity_public_key(), pairing_id);

  char airplay_instance[MDNS_AIRPLAY_LABEL_MAX + 1];
  mdns_airplay_airplay_instance(instance_name, airplay_instance);
  err = add_service(airplay_instance, "_airplay", txt.airplay,
                    MDNS_AIRPLAY_AIRPLAY_TXT_COUNT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_service_add(_airplay._tcp) failed: %s",
             esp_err_to_name(err));
    all_ok = false;
  }

  char raop_instance[MDNS_AIRPLAY_LABEL_MAX + 1];
  mdns_airplay_raop_instance(mac, instance_name, raop_instance);
  err = add_service(raop_instance, "_raop", txt.raop,
                    MDNS_AIRPLAY_RAOP_TXT_COUNT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_service_add(_raop._tcp) failed: %s",
             esp_err_to_name(err));
    all_ok = false;
  }

  s_ready = all_ok;
  snprintf(s_flags, sizeof(s_flags), "%s", txt.flags);
  snprintf(s_gid, sizeof(s_gid), "%s", pairing_id);
  if (all_ok) {
    ESP_LOGI(TAG, "AirPlay on mDNS as \"%s\" (%s.local, port %d)",
             airplay_instance, host, MDNS_AIRPLAY_PORT);
  }
}

esp_err_t mdns_airplay_update_name(const char *name) {
  if (!name) return ESP_ERR_INVALID_ARG;
  if (!s_ready) return ESP_ERR_INVALID_STATE;
  char host[WIFI_HOSTNAME_MAX_LEN + 1], mac[MDNS_AIRPLAY_MAC_STR_LEN];
  char airplay[MDNS_AIRPLAY_LABEL_MAX + 1], raop[MDNS_AIRPLAY_LABEL_MAX + 1];
  wifi_sanitize_hostname(name, host, sizeof(host));
  wifi_get_mac_str(mac, sizeof(mac));
  mdns_airplay_airplay_instance(name, airplay);
  mdns_airplay_raop_instance(mac, name, raop);
  const esp_err_t a = mdns_hostname_set(host);
  const esp_err_t b = mdns_service_instance_name_set("_airplay", "_tcp", airplay);
  const esp_err_t c = mdns_service_instance_name_set("_raop", "_tcp", raop);
  if (a != ESP_OK || b != ESP_OK || c != ESP_OK) {
    ESP_LOGW(TAG, "Updating speaker name: %s / %s / %s",
             esp_err_to_name(a), esp_err_to_name(b), esp_err_to_name(c));
    return a != ESP_OK ? a : b != ESP_OK ? b : c;
  }
  return ESP_OK;
}

void mdns_airplay_update_flags(void) {
#ifdef CONFIG_AIRPLAY_HOMEKIT
  char flags[12];
  if (!s_ready) return;
  snprintf(flags, sizeof(flags), "0x%X", (unsigned)airplay_status_flags());
  if (s_flags_valid && !strcmp(flags, s_flags)) return;
  esp_err_t a = mdns_service_txt_item_set("_airplay", "_tcp", "flags", flags);
  esp_err_t b = mdns_service_txt_item_set("_raop", "_tcp", "sf", flags);
  s_flags_valid = a == ESP_OK && b == ESP_OK;
  if (s_flags_valid) snprintf(s_flags, sizeof(s_flags), "%s", flags);
  else ESP_LOGW(TAG, "Updating HomeKit status: %s / %s",
                esp_err_to_name(a), esp_err_to_name(b));
#endif
}

void mdns_airplay_set_group(const char *gid, bool contains_leader) {
#ifdef CONFIG_AIRPLAY_HOMEKIT
  char want[sizeof(s_gid)];
  if (!s_ready) return;
  if (gid && *gid) snprintf(want, sizeof(want), "%s", gid);
  else airplay_get_pairing_id(want, sizeof(want));
  if (s_group_valid && !strcmp(want, s_gid) && contains_leader == s_contains_leader) return;
  esp_err_t a = mdns_service_txt_item_set("_airplay", "_tcp", "gid", want);
  esp_err_t b = mdns_service_txt_item_set("_airplay", "_tcp", "gcgl",
                                         contains_leader ? "1" : "0");
  s_group_valid = a == ESP_OK && b == ESP_OK;
  if (s_group_valid) {
    snprintf(s_gid, sizeof(s_gid), "%s", want);
    s_contains_leader = contains_leader;
  } else ESP_LOGW(TAG, "Updating AirPlay group: %s / %s",
                  esp_err_to_name(a), esp_err_to_name(b));
#else
  (void)gid;
  (void)contains_leader;
#endif
}
