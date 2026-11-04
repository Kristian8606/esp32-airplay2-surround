#include "mdns_airplay.h"

#include <stdio.h>
#include <string.h>

#include "airplay_identity.h"
#include "esp_log.h"
#include "ap2_hap.h"
#include "mdns.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "mdns_airplay";

#define AIRPLAY_PORT     7000
#define NAME_CAPACITY    65
#define MAC_TEXT_LEN     18
#define PUBLIC_KEY_BYTES 32

#define COUNT_OF(list) (sizeof(list) / sizeof((list)[0]))

static void log_failure(const char *what, esp_err_t err) {
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
  }
}

void mdns_airplay_init(void) {
  char name[NAME_CAPACITY] = "";
  char mac[MAC_TEXT_LEN];
  char mac_digits[MAC_TEXT_LEN] = "";
  char host[WIFI_HOSTNAME_MAX_LEN + 1];
  char raop_name[MAC_TEXT_LEN + 1 + NAME_CAPACITY];
  char features[32], flags[16], version[8];
  char key_hex[2 * PUBLIC_KEY_BYTES + 1];
  char pairing_id[AIRPLAY_PAIRING_ID_LEN];
  const uint8_t *key = ap2_hap_identity_public_key();
  size_t digits = 0;

  (void)settings_get_device_name(name, sizeof(name));
  wifi_get_mac_str(mac, sizeof(mac));
  for (const char *c = mac; *c; c++) {
    if (*c != ':') {
      mac_digits[digits++] = *c;
    }
  }
  mac_digits[digits] = '\0';
  for (int i = 0; i < PUBLIC_KEY_BYTES; i++) {
    snprintf(&key_hex[2 * i], 3, "%02x", key[i]);
  }
  snprintf(features, sizeof(features), "0x%X,0x%X",
           (unsigned)AIRPLAY_FEATURES_LO, (unsigned)AIRPLAY_FEATURES_HI);
  snprintf(flags, sizeof(flags), "0x%X", (unsigned)AIRPLAY_STATUS_FLAGS);
  snprintf(version, sizeof(version), "%d", AIRPLAY_PROTOCOL_VERSION);
  airplay_get_pairing_id(pairing_id, sizeof(pairing_id));

  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    log_failure("mdns_init", err);
    return;
  }
  wifi_sanitize_hostname(name, host, sizeof(host));
  log_failure("mdns_hostname_set", mdns_hostname_set(host));

  mdns_txt_item_t airplay_txt[] = {
      {"deviceid", mac},
      {"features", features},
      {"flags", flags},
      {"model", AIRPLAY_MODEL},
      {"pk", key_hex},
      {"pi", pairing_id},
      {"srcvers", AIRPLAY_SOURCE_VERSION},
      {"vv", version},
      {"acl", "0"},
  };
  log_failure("_airplay service",
              mdns_service_add(name, "_airplay", "_tcp", AIRPLAY_PORT,
                               airplay_txt, COUNT_OF(airplay_txt)));

  /* The RAOP name is the MAC's digits, then the device name. */
  snprintf(raop_name, sizeof(raop_name), "%s@%s", mac_digits, name);
  mdns_txt_item_t raop_txt[] = {
      {"am", AIRPLAY_MODEL},
      {"cn", "0,1,2,3"},
      {"da", "true"},
      {"et", "0,3,5"},
      {"ft", features},
      {"md", "0,2"},
      {"pk", key_hex},
      {"sf", flags},
      {"tp", "UDP"},
      {"vn", "65537"},
      {"vs", AIRPLAY_SOURCE_VERSION},
      {"vv", version},
  };
  log_failure("_raop service",
              mdns_service_add(raop_name, "_raop", "_tcp", AIRPLAY_PORT,
                               raop_txt, COUNT_OF(raop_txt)));
}
