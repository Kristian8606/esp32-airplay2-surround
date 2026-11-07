/* SPDX-License-Identifier: Apache-2.0 */
#include "mdns_airplay.h"

#include <stdbool.h>
#include <stdint.h>

#include "ap2_hap.h"
#include "airplay_identity.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mdns.h"
#include "mdns_airplay_txt.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "mdns_airplay";

_Static_assert(sizeof(mdns_txt_item_t) == sizeof(mdns_airplay_txt_t),
               "mdns_txt_item_t layout changed");

static esp_err_t add_service(const char *instance, const char *type,
                             const mdns_airplay_txt_t *txt, size_t count)
{
  mdns_txt_item_t items[MDNS_AIRPLAY_RAOP_TXT_COUNT];
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

  if (all_ok) {
    ESP_LOGI(TAG, "AirPlay on mDNS as \"%s\" (%s.local, port %d)",
             airplay_instance, host, MDNS_AIRPLAY_PORT);
  }
}
