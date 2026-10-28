#pragma once
#include <stdint.h>
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define SETTINGS_DEFAULT_DEVICE_NAME "ESP32 AirPlay"
#define SETTINGS_MAX_WIFI_NETWORKS 8
#define SETTINGS_WIFI_SSID_LEN 33
#define SETTINGS_WIFI_PASSWORD_LEN 65

typedef struct {
  char ssid[SETTINGS_WIFI_SSID_LEN];
  char password[SETTINGS_WIFI_PASSWORD_LEN];
} settings_wifi_network_t;

esp_err_t settings_init(void);
esp_err_t settings_set_wifi_credentials(const char *ssid, const char *password);
bool settings_has_wifi_credentials(void);
size_t settings_get_wifi_network_count(void);
esp_err_t settings_get_wifi_network(size_t index,
                                    settings_wifi_network_t *network);
esp_err_t settings_get_device_name(char *name, size_t len);
esp_err_t settings_set_device_name(const char *name);
esp_err_t settings_get_volume(float *volume_db);
esp_err_t settings_set_volume(float volume_db);
/* Write the volume to NVS if it changed since the last write. Call only
 * when playback stops: a flash write stalls the caches for a moment. */
esp_err_t settings_persist_volume(void);
/* Output latency after the ESP (us). Returns the Kconfig default
 * CONFIG_AIRPLAY_OUTPUT_LATENCY_US when nothing was saved. */
esp_err_t settings_get_output_latency_us(int32_t *us);
esp_err_t settings_set_output_latency_us(int32_t us);
/* Forget the saved value (back to the Kconfig default). */
esp_err_t settings_clear_output_latency(void);
/* Loudness compensation: on/off and reference offset in dB (off, 0 when
 * nothing was saved). */
esp_err_t settings_get_loudness(bool *enabled, int32_t *reference_db);
esp_err_t settings_set_loudness(bool enabled, int32_t reference_db);
/* Multichannel experiment: which 5.1/7.1 speaker this receiver plays
 * (aac_speaker_t, -1 = mono mix). Returns the Kconfig default
 * CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER when nothing was saved. */
esp_err_t settings_get_mc_speaker(int32_t *speaker);
esp_err_t settings_set_mc_speaker(int32_t speaker);
/* Speaker build: Surround mode (follow the Surround Gateway, no AirPlay
 * advertisement or RTSP) instead of a standalone AirPlay receiver. Off when
 * nothing was saved; takes effect after a restart. */
bool settings_get_surround_mode(void);
esp_err_t settings_set_surround_mode(bool enabled);
/* Surround speaker tuning set from the Gateway web page: level trim in
 * tenths of a dB (<= 0) and extra delay in us. 0 and 0 when nothing saved. */
esp_err_t settings_get_surround_tuning(int32_t *trim_db10, int32_t *delay_us);
esp_err_t settings_set_surround_tuning(int32_t trim_db10, int32_t delay_us);
