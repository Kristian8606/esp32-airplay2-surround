#include "settings.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "airplay_log.h"

static const char *TAG = "settings";
static const char *NS = "airplay";
static float s_volume_db = 0.0f;

#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_WIFI_LIST "wifi_list"
#define WIFI_STORE_MAGIC 0x57494649U
#define WIFI_STORE_VERSION 1U

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint16_t latest_index;
  uint16_t next_replace;
  settings_wifi_network_t network[SETTINGS_MAX_WIFI_NETWORKS];
} wifi_store_t;

static void wifi_store_init(wifi_store_t *store) {
  memset(store, 0, sizeof(*store));
  store->magic = WIFI_STORE_MAGIC;
  store->version = WIFI_STORE_VERSION;
}

static bool wifi_store_valid(const wifi_store_t *store) {
  if (!store || store->magic != WIFI_STORE_MAGIC ||
      store->version != WIFI_STORE_VERSION ||
      store->count > SETTINGS_MAX_WIFI_NETWORKS) {
    return false;
  }
  if (store->count == 0) return true;
  return store->latest_index < store->count;
}

static esp_err_t wifi_store_load(nvs_handle_t h, wifi_store_t *store) {
  if (!store) return ESP_ERR_INVALID_ARG;
  size_t size = sizeof(*store);
  esp_err_t e = nvs_get_blob(h, NVS_KEY_WIFI_LIST, store, &size);
  if (e != ESP_OK) return e;
  if (size != sizeof(*store) || !wifi_store_valid(store)) {
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

static esp_err_t wifi_store_save(nvs_handle_t h, const wifi_store_t *store) {
  if (!store || !wifi_store_valid(store)) return ESP_ERR_INVALID_ARG;
  return nvs_set_blob(h, NVS_KEY_WIFI_LIST, store, sizeof(*store));
}

static esp_err_t nvs_get_str_fixed(nvs_handle_t h, const char *key, char *out,
                                   size_t len) {
  if (!out || len == 0) return ESP_ERR_INVALID_ARG;
  size_t need = len;
  esp_err_t e = nvs_get_str(h, key, out, &need);
  if (e != ESP_OK) out[0] = '\0';
  return e;
}

static bool load_legacy_network(nvs_handle_t h,
                                settings_wifi_network_t *network) {
  if (!network) return false;
  memset(network, 0, sizeof(*network));
  if (nvs_get_str_fixed(h, NVS_KEY_WIFI_SSID, network->ssid,
                        sizeof(network->ssid)) != ESP_OK ||
      network->ssid[0] == '\0') {
    return false;
  }
  if (nvs_get_str_fixed(h, NVS_KEY_WIFI_PASS, network->password,
                        sizeof(network->password)) != ESP_OK) {
    network->password[0] = '\0';
  }
  return true;
}

static esp_err_t load_store_or_legacy(nvs_handle_t h, wifi_store_t *store) {
  esp_err_t e = wifi_store_load(h, store);
  if (e == ESP_OK) return ESP_OK;

  wifi_store_init(store);
  settings_wifi_network_t legacy;
  if (load_legacy_network(h, &legacy)) {
    store->network[0] = legacy;
    store->count = 1;
    store->latest_index = 0;
    store->next_replace = 0;
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}

esp_err_t settings_init(void) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;

  wifi_store_t store;
  e = wifi_store_load(h, &store);
  if (e != ESP_OK) {
    wifi_store_init(&store);
    settings_wifi_network_t legacy;
    if (load_legacy_network(h, &legacy)) {
      store.network[0] = legacy;
      store.count = 1;
      store.latest_index = 0;
      e = wifi_store_save(h, &store);
      if (e == ESP_OK) e = nvs_commit(h);
      if (e == ESP_OK) {
        ESP_LOGI(TAG, "Migrated legacy WiFi credentials into saved network list");
      }
    } else {
      e = wifi_store_save(h, &store);
      if (e == ESP_OK) e = nvs_commit(h);
    }
  }

  nvs_close(h);
  return e;
}

static esp_err_t get_str(const char *key, char *out, size_t len) {
  if (!out || !len) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e != ESP_OK) return e;
  size_t need = len;
  e = nvs_get_str(h, key, out, &need);
  nvs_close(h);
  return e;
}

static esp_err_t set_str(const char *key, const char *value) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_str(h, key, value ? value : "");
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}

size_t settings_get_wifi_network_count(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
  wifi_store_t store;
  esp_err_t e = load_store_or_legacy(h, &store);
  nvs_close(h);
  return e == ESP_OK ? store.count : 0;
}

esp_err_t settings_get_wifi_network(size_t index,
                                    settings_wifi_network_t *network) {
  if (!network) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e != ESP_OK) return e;

  wifi_store_t store;
  e = load_store_or_legacy(h, &store);
  if (e == ESP_OK) {
    if (index >= store.count) {
      e = ESP_ERR_NOT_FOUND;
    } else {
      *network = store.network[index];
    }
  }
  nvs_close(h);
  return e;
}

esp_err_t settings_set_wifi_credentials(const char *ssid,
                                        const char *password) {
  if (!ssid || ssid[0] == '\0' || strlen(ssid) >= SETTINGS_WIFI_SSID_LEN ||
      (password && strlen(password) >= SETTINGS_WIFI_PASSWORD_LEN)) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;

  wifi_store_t store;
  if (load_store_or_legacy(h, &store) != ESP_OK) wifi_store_init(&store);

  size_t index = SETTINGS_MAX_WIFI_NETWORKS;
  for (size_t i = 0; i < store.count; ++i) {
    if (strcmp(store.network[i].ssid, ssid) == 0) {
      index = i;
      break;
    }
  }

  if (index == SETTINGS_MAX_WIFI_NETWORKS) {
    if (store.count < SETTINGS_MAX_WIFI_NETWORKS) {
      index = store.count++;
    } else {
      index = store.next_replace % SETTINGS_MAX_WIFI_NETWORKS;
      store.next_replace =
          (uint16_t)((store.next_replace + 1U) % SETTINGS_MAX_WIFI_NETWORKS);
      AP_LOGW(TAG, "WiFi list full; replacing saved network slot %u",
               (unsigned)index);
    }
  }

  memset(&store.network[index], 0, sizeof(store.network[index]));
  strlcpy(store.network[index].ssid, ssid,
          sizeof(store.network[index].ssid));
  strlcpy(store.network[index].password, password ? password : "",
          sizeof(store.network[index].password));
  store.latest_index = (uint16_t)index;

  e = wifi_store_save(h, &store);
  if (e == ESP_OK) {
    // Keep the old keys in sync so older firmware can still boot using the
    // most recently added network after a downgrade.
    e = nvs_set_str(h, NVS_KEY_WIFI_SSID, store.network[index].ssid);
  }
  if (e == ESP_OK) {
    e = nvs_set_str(h, NVS_KEY_WIFI_PASS, store.network[index].password);
  }
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);

  if (e == ESP_OK) {
    AP_LOGI(TAG, "Saved WiFi network '%s' (%u/%u slots used)", ssid,
             (unsigned)store.count, (unsigned)SETTINGS_MAX_WIFI_NETWORKS);
  }
  return e;
}

bool settings_has_wifi_credentials(void) {
  return settings_get_wifi_network_count() > 0;
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (!name || !len) return ESP_ERR_INVALID_ARG;
  esp_err_t e = get_str("device_name", name, len);
  /* No name saved (or the plain default saved by an older firmware): add the
   * last 3 hex digits of the Wi-Fi MAC, so two receivers on one network get
   * different mDNS names and host names, e.g. "ESP32 AirPlay A3F". */
  if (e != ESP_OK || name[0] == '\0' ||
      strcmp(name, SETTINGS_DEFAULT_DEVICE_NAME) == 0) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, len, "%s %X%02X", SETTINGS_DEFAULT_DEVICE_NAME,
             (unsigned)(mac[4] & 0x0F), (unsigned)mac[5]);
  }
  return ESP_OK;
}
esp_err_t settings_set_device_name(const char *name) { return set_str("device_name", name); }
/* The live volume is the RAM value; NVS holds the value persisted when
 * playback last stopped (pause, stream teardown, disconnect). Read NVS once,
 * so a later read (a new connection) can never replace the playing session's
 * volume with the stale stored one. */
static bool s_volume_known;
static bool s_volume_stored_valid; /* s_volume_stored_mdb is what NVS holds */
static int32_t s_volume_stored_mdb;

esp_err_t settings_get_volume(float *volume_db) {
  if (!volume_db) return ESP_ERR_INVALID_ARG;
  if (s_volume_known) {
    *volume_db = s_volume_db;
    return ESP_OK;
  }
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e != ESP_OK) { *volume_db = s_volume_db; return e; }
  int32_t mv = 0;
  e = nvs_get_i32(h, "volume_mdb", &mv);
  nvs_close(h);
  if (e == ESP_OK) {
    s_volume_db = (float)mv / 1000.0f;
    s_volume_known = true;
    s_volume_stored_mdb = mv;
    s_volume_stored_valid = true;
  }
  *volume_db = s_volume_db;
  return e;
}
esp_err_t settings_set_volume(float volume_db) {
  s_volume_db = volume_db;
  s_volume_known = true;
  return ESP_OK;
}

#ifndef CONFIG_AIRPLAY_OUTPUT_LATENCY_US
#define CONFIG_AIRPLAY_OUTPUT_LATENCY_US 0
#endif
esp_err_t settings_get_output_latency_us(int32_t *us) {
  if (!us) return ESP_ERR_INVALID_ARG;
  *us = CONFIG_AIRPLAY_OUTPUT_LATENCY_US;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
  int32_t v = 0;
  if (nvs_get_i32(h, "out_lat_us", &v) == ESP_OK) *us = v;
  nvs_close(h);
  return ESP_OK;
}
esp_err_t settings_set_output_latency_us(int32_t us) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_i32(h, "out_lat_us", us);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t settings_clear_output_latency(void) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_erase_key(h, "out_lat_us");
  if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t settings_get_loudness(bool *enabled, int32_t *reference_db) {
  if (!enabled || !reference_db) return ESP_ERR_INVALID_ARG;
  *enabled = false;
  *reference_db = 0;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
  uint8_t en = 0;
  int32_t ref = 0;
  if (nvs_get_u8(h, "loud_en", &en) == ESP_OK) *enabled = en != 0;
  if (nvs_get_i32(h, "loud_ref", &ref) == ESP_OK) *reference_db = ref;
  nvs_close(h);
  return ESP_OK;
}
esp_err_t settings_set_loudness(bool enabled, int32_t reference_db) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_u8(h, "loud_en", enabled ? 1U : 0U);
  if (e == ESP_OK) e = nvs_set_i32(h, "loud_ref", reference_db);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t settings_get_mc_speaker(int32_t *speaker) {
  if (!speaker) return ESP_ERR_INVALID_ARG;
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
  *speaker = CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER;
#else
  *speaker = -1;
#endif
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
  int32_t v = 0;
  if (nvs_get_i32(h, "mc_speaker", &v) == ESP_OK) *speaker = v;
  nvs_close(h);
  return ESP_OK;
}
esp_err_t settings_set_mc_speaker(int32_t speaker) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_i32(h, "mc_speaker", speaker);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
bool settings_get_surround_mode(void) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t v = 0;
  if (nvs_get_u8(h, "surround", &v) != ESP_OK) v = 0;
  nvs_close(h);
  return v != 0U;
}
esp_err_t settings_set_surround_mode(bool enabled) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_u8(h, "surround", enabled ? 1U : 0U);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t settings_get_surround_tuning(int32_t *trim_db10, int32_t *delay_us) {
  if (!trim_db10 || !delay_us) return ESP_ERR_INVALID_ARG;
  *trim_db10 = 0;
  *delay_us = 0;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
  (void)nvs_get_i32(h, "sur_trim", trim_db10);
  (void)nvs_get_i32(h, "sur_delay", delay_us);
  nvs_close(h);
  return ESP_OK;
}
esp_err_t settings_set_surround_tuning(int32_t trim_db10, int32_t delay_us) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_i32(h, "sur_trim", trim_db10);
  if (e == ESP_OK) e = nvs_set_i32(h, "sur_delay", delay_us);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t settings_persist_volume(void) {
  /* Called whenever playback stops: write only a changed volume, so pauses
   * do not wear the flash. */
  if (!s_volume_known) return ESP_OK;
  const int32_t mdb = (int32_t)(s_volume_db * 1000.0f);
  if (s_volume_stored_valid && s_volume_stored_mdb == mdb) return ESP_OK;
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_set_i32(h, "volume_mdb", mdb);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  if (e == ESP_OK) {
    s_volume_stored_mdb = mdb;
    s_volume_stored_valid = true;
  }
  return e;
}
