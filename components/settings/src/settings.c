/* SPDX-License-Identifier: Apache-2.0 */

#include "settings.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "settings_core.h"

#ifdef CONFIG_AIRPLAY_MULTICHANNEL
#define DEFAULT_MC_SPEAKER CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER
#else
#define DEFAULT_MC_SPEAKER (-1)
#endif

#define NAMESPACE "airplay"
#define KEY_DEVICE_NAME "device_name"
#define KEY_WIFI_LIST "wifi_list"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_VOLUME "volume_mdb"
#define KEY_OUTPUT_LATENCY "out_lat_us"
#define KEY_LOUDNESS_ON "loud_en"
#define KEY_LOUDNESS_REF "loud_ref"
#define KEY_MC_SPEAKER "mc_speaker"
#define KEY_SURROUND_TRIM "sur_trim"
#define KEY_SURROUND_DELAY "sur_delay"

#define DEVICE_NAME_MAX 64

static const char *TAG = "settings";

/* Guards the volume in RAM, the read-modify-write of the network list and
 * the buffers below. Created by settings_init(), or by the first call if one
 * comes earlier; settings_init() runs in the main task before any other task
 * that uses settings starts, so two first calls never race. */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

/* Kept out of the callers' stacks; only used with s_lock held. */
static settings_wifi_list_t s_list;
static uint8_t s_blob[SETTINGS_WIFI_LIST_SIZE];

static bool s_volume_set;         /* s_volume_db holds a value */
static float s_volume_db;
static bool s_volume_saved_known; /* s_volume_saved_mdb is what NVS holds */
static int32_t s_volume_saved_mdb;

static void lock(void) {
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_lock); }

static esp_err_t open_read(nvs_handle_t *handle) {
  return nvs_open(NAMESPACE, NVS_READONLY, handle);
}

static esp_err_t open_write(nvs_handle_t *handle, const char *what) {
  esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s not saved: %s", what, esp_err_to_name(err));
  }
  return err;
}

/* Commits when everything before went well, closes, logs a failure. */
static esp_err_t finish_write(nvs_handle_t handle, esp_err_t err,
                              const char *what) {
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s not saved: %s", what, esp_err_to_name(err));
  }
  return err;
}

/* ---- Wi-Fi networks ---- */

static bool read_list(nvs_handle_t handle, settings_wifi_list_t *list) {
  size_t len = 0;
  if (nvs_get_blob(handle, KEY_WIFI_LIST, NULL, &len) != ESP_OK ||
      len != SETTINGS_WIFI_LIST_SIZE) {
    settings_wifi_list_clear(list);
    return false;
  }
  if (nvs_get_blob(handle, KEY_WIFI_LIST, s_blob, &len) != ESP_OK) {
    settings_wifi_list_clear(list);
    return false;
  }
  return settings_wifi_list_parse(s_blob, len, list);
}

/* The network kept in wifi_ssid / wifi_pass before the list. */
static bool read_single_network(nvs_handle_t handle,
                                settings_wifi_network_t *network) {
  memset(network, 0, sizeof(*network));
  size_t len = sizeof(network->ssid);
  if (nvs_get_str(handle, KEY_WIFI_SSID, network->ssid, &len) != ESP_OK ||
      network->ssid[0] == '\0') {
    memset(network, 0, sizeof(*network));
    return false;
  }
  len = sizeof(network->password);
  esp_err_t err =
      nvs_get_str(handle, KEY_WIFI_PASS, network->password, &len);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    network->password[0] = '\0';
  } else if (err != ESP_OK) {
    /* A password that cannot be read: no network (spec answer 1). */
    memset(network, 0, sizeof(*network));
    return false;
  }
  return true;
}

/* The valid list, else the older single network, else an empty list. */
static void load_networks(nvs_handle_t handle, settings_wifi_list_t *list) {
  if (read_list(handle, list)) {
    return;
  }
  if (read_single_network(handle, &list->slot[0])) {
    list->count = 1;
  }
}

/* Under s_lock. */
static void load_networks_read_only(settings_wifi_list_t *list) {
  nvs_handle_t handle = 0;
  if (open_read(&handle) != ESP_OK) {
    settings_wifi_list_clear(list);
    return;
  }
  load_networks(handle, list);
  nvs_close(handle);
}

esp_err_t settings_init(void) {
  lock();
  nvs_handle_t handle = 0;
  esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    unlock();
    return err;
  }
  if (!read_list(handle, &s_list) &&
      read_single_network(handle, &s_list.slot[0])) {
    s_list.count = 1;
    settings_wifi_list_serialize(&s_list, s_blob);
    err = nvs_set_blob(handle, KEY_WIFI_LIST, s_blob, sizeof(s_blob));
    if (err == ESP_OK) {
      err = nvs_commit(handle);
    }
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Migrated the saved Wi-Fi network into the network list");
    } else {
      /* Still ESP_OK: the older network is read from wifi_ssid and the next
       * start tries again (spec answer 2). */
      ESP_LOGE(TAG, "Wi-Fi network list not saved: %s",
               esp_err_to_name(err));
    }
  }
  nvs_close(handle);
  unlock();
  return ESP_OK;
}

esp_err_t settings_set_wifi_credentials(const char *ssid,
                                        const char *password) {
  if (!settings_wifi_credentials_valid(ssid, password)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (password == NULL) {
    password = "";
  }
  const char *what = "Wi-Fi network";
  lock();
  nvs_handle_t handle = 0;
  esp_err_t err = open_write(&handle, what);
  if (err != ESP_OK) {
    unlock();
    return err;
  }
  load_networks(handle, &s_list);
  settings_wifi_list_put(&s_list, ssid, password);
  settings_wifi_list_serialize(&s_list, s_blob);
  err = nvs_set_blob(handle, KEY_WIFI_LIST, s_blob, sizeof(s_blob));
  if (err == ESP_OK) {
    err = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(handle, KEY_WIFI_PASS, password);
  }
  err = finish_write(handle, err, what);
  unlock();
  return err;
}

size_t settings_get_wifi_network_count(void) {
  lock();
  load_networks_read_only(&s_list);
  size_t count = s_list.count;
  unlock();
  return count;
}

bool settings_has_wifi_credentials(void) {
  return settings_get_wifi_network_count() > 0;
}

esp_err_t settings_get_wifi_network(size_t index,
                                    settings_wifi_network_t *network) {
  if (network == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  lock();
  load_networks_read_only(&s_list);
  esp_err_t err = ESP_ERR_NOT_FOUND;
  if (index < s_list.count) {
    *network = s_list.slot[index];
    err = ESP_OK;
  }
  unlock();
  return err;
}

/* ---- Device name ---- */

/* The saved name when there is a non-empty one, else NULL. Free it. */
static char *read_device_name(void) {
  nvs_handle_t handle = 0;
  if (open_read(&handle) != ESP_OK) {
    return NULL;
  }
  char *name = NULL;
  size_t len = 0;
  if (nvs_get_str(handle, KEY_DEVICE_NAME, NULL, &len) == ESP_OK && len > 1) {
    name = malloc(len);
    if (name != NULL &&
        nvs_get_str(handle, KEY_DEVICE_NAME, name, &len) != ESP_OK) {
      free(name);
      name = NULL;
    }
  }
  nvs_close(handle);
  if (name != NULL && name[0] == '\0') {
    free(name);
    name = NULL;
  }
  return name;
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (name == NULL || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  lock();
  char *saved = read_device_name();
  unlock();
  if (saved != NULL) {
    settings_copy_name(name, len, saved);
    free(saved);
    return ESP_OK;
  }
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    memset(mac, 0, sizeof(mac)); /* spec answer 3 */
  }
  char fallback[SETTINGS_DEFAULT_NAME_SIZE];
  settings_default_device_name(mac, fallback);
  settings_copy_name(name, len, fallback);
  return ESP_OK;
}

esp_err_t settings_set_device_name(const char *name) {
  if (name == NULL) {
    name = "";
  }
  if (strnlen(name, DEVICE_NAME_MAX + 1) > DEVICE_NAME_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  const char *what = "Device name";
  lock();
  nvs_handle_t handle = 0;
  esp_err_t err = open_write(&handle, what);
  if (err == ESP_OK) {
    err = finish_write(handle, nvs_set_str(handle, KEY_DEVICE_NAME, name),
                       what);
  }
  unlock();
  return err;
}

/* ---- Volume ---- */

esp_err_t settings_get_volume(float *volume_db) {
  if (volume_db == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  lock();
  if (s_volume_set) {
    *volume_db = s_volume_db;
    unlock();
    return ESP_OK;
  }
  nvs_handle_t handle = 0;
  int32_t mdb = 0;
  esp_err_t err = open_read(&handle);
  if (err == ESP_OK) {
    err = nvs_get_i32(handle, KEY_VOLUME, &mdb);
    nvs_close(handle);
  }
  if (err == ESP_OK) {
    s_volume_db = (float)(mdb / 1000.0);
    s_volume_set = true;
    s_volume_saved_mdb = mdb;
    s_volume_saved_known = true;
    *volume_db = s_volume_db;
  }
  unlock();
  return err;
}

esp_err_t settings_set_volume(float volume_db) {
  lock();
  s_volume_db = volume_db;
  s_volume_set = true;
  unlock();
  return ESP_OK;
}

esp_err_t settings_persist_volume(void) {
  lock();
  if (!s_volume_set) {
    unlock();
    return ESP_OK; /* nothing to save (spec answer 4) */
  }
  int32_t mdb = settings_volume_to_mdb(s_volume_db);
  if (s_volume_saved_known && mdb == s_volume_saved_mdb) {
    unlock();
    return ESP_OK;
  }
  const char *what = "Volume";
  nvs_handle_t handle = 0;
  esp_err_t err = open_write(&handle, what);
  if (err == ESP_OK) {
    err = finish_write(handle, nvs_set_i32(handle, KEY_VOLUME, mdb), what);
  }
  if (err == ESP_OK) {
    s_volume_saved_mdb = mdb;
    s_volume_saved_known = true;
  }
  unlock();
  return err;
}

/* ---- Speaker audio settings ---- */

/* Nothing saved or NVS that cannot be read: the default. */
static int32_t read_i32(nvs_handle_t handle, bool opened, const char *key,
                        int32_t fallback) {
  int32_t value;
  if (opened && nvs_get_i32(handle, key, &value) == ESP_OK) {
    return value;
  }
  return fallback;
}

static esp_err_t write_i32(const char *what, size_t n, const char *const *keys,
                           const int32_t *values) {
  nvs_handle_t handle = 0;
  esp_err_t err = open_write(&handle, what);
  if (err != ESP_OK) {
    return err;
  }
  for (size_t i = 0; i < n && err == ESP_OK; i++) {
    err = nvs_set_i32(handle, keys[i], values[i]);
  }
  return finish_write(handle, err, what);
}

esp_err_t settings_get_output_latency_us(int32_t *us) {
  if (us == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t handle = 0;
  bool opened = open_read(&handle) == ESP_OK;
  *us = read_i32(handle, opened, KEY_OUTPUT_LATENCY,
                 CONFIG_AIRPLAY_OUTPUT_LATENCY_US);
  if (opened) {
    nvs_close(handle);
  }
  return ESP_OK;
}

esp_err_t settings_set_output_latency_us(int32_t us) {
  static const char *const keys[] = {KEY_OUTPUT_LATENCY};
  return write_i32("Output latency", 1, keys, &us);
}

esp_err_t settings_clear_output_latency(void) {
  const char *what = "Output latency";
  nvs_handle_t handle = 0;
  esp_err_t err = open_write(&handle, what);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_erase_key(handle, KEY_OUTPUT_LATENCY);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK;
  }
  return finish_write(handle, err, what);
}

esp_err_t settings_get_loudness(bool *enabled, int32_t *reference_db) {
  if (enabled == NULL || reference_db == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t handle = 0;
  bool opened = open_read(&handle) == ESP_OK;
  uint8_t on = 0;
  if (!opened || nvs_get_u8(handle, KEY_LOUDNESS_ON, &on) != ESP_OK) {
    on = 0;
  }
  *enabled = on != 0;
  *reference_db = read_i32(handle, opened, KEY_LOUDNESS_REF, 0);
  if (opened) {
    nvs_close(handle);
  }
  return ESP_OK;
}

esp_err_t settings_set_loudness(bool enabled, int32_t reference_db) {
  const char *what = "Loudness";
  nvs_handle_t handle = 0;
  esp_err_t err = open_write(&handle, what);
  if (err != ESP_OK) {
    return err;
  }
  err = nvs_set_u8(handle, KEY_LOUDNESS_ON, enabled ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_i32(handle, KEY_LOUDNESS_REF, reference_db);
  }
  return finish_write(handle, err, what);
}

esp_err_t settings_get_mc_speaker(int32_t *speaker) {
  if (speaker == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t handle = 0;
  bool opened = open_read(&handle) == ESP_OK;
  *speaker = read_i32(handle, opened, KEY_MC_SPEAKER, DEFAULT_MC_SPEAKER);
  if (opened) {
    nvs_close(handle);
  }
  return ESP_OK;
}

esp_err_t settings_set_mc_speaker(int32_t speaker) {
  static const char *const keys[] = {KEY_MC_SPEAKER};
  return write_i32("Speaker", 1, keys, &speaker);
}

esp_err_t settings_get_surround_tuning(int32_t *trim_db10, int32_t *delay_us) {
  if (trim_db10 == NULL || delay_us == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t handle = 0;
  bool opened = open_read(&handle) == ESP_OK;
  *trim_db10 = read_i32(handle, opened, KEY_SURROUND_TRIM, 0);
  *delay_us = read_i32(handle, opened, KEY_SURROUND_DELAY, 0);
  if (opened) {
    nvs_close(handle);
  }
  return ESP_OK;
}

esp_err_t settings_set_surround_tuning(int32_t trim_db10, int32_t delay_us) {
  static const char *const keys[] = {KEY_SURROUND_TRIM, KEY_SURROUND_DELAY};
  const int32_t values[] = {trim_db10, delay_us};
  return write_i32("Surround tuning", 2, keys, values);
}
