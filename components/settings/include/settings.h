/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The saved settings of both firmwares, in NVS: device name, Wi-Fi
 * networks, volume and the speaker's audio settings. See spec/spec.md. */

#define SETTINGS_DEFAULT_DEVICE_NAME "ESP32 AirPlay"
#define SETTINGS_MAX_WIFI_NETWORKS 8
#define SETTINGS_WIFI_SSID_LEN 33     /* 32 bytes and the NUL */
#define SETTINGS_WIFI_PASSWORD_LEN 65 /* 64 bytes and the NUL */

typedef struct {
  char ssid[SETTINGS_WIFI_SSID_LEN];
  char password[SETTINGS_WIFI_PASSWORD_LEN];
} settings_wifi_network_t;

/* Once at start-up, after nvs_flash_init(). */
esp_err_t settings_init(void);

/* Wi-Fi networks, at most SETTINGS_MAX_WIFI_NETWORKS. */
esp_err_t settings_set_wifi_credentials(const char *ssid, const char *password);
bool settings_has_wifi_credentials(void);
size_t settings_get_wifi_network_count(void);
esp_err_t settings_get_wifi_network(size_t index,
                                    settings_wifi_network_t *network);

/* Device name (UTF-8); a device without one has a default name. */
esp_err_t settings_get_device_name(char *name, size_t len);
esp_err_t settings_set_device_name(const char *name);

/* AirPlay volume in dB. Set keeps it in RAM; persist writes it to NVS if it
 * changed since the last write. Call persist only when playback stops: a
 * flash write stalls the caches for a moment. */
esp_err_t settings_get_volume(float *volume_db);
esp_err_t settings_set_volume(float volume_db);
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

/* Which 5.1/7.1 speaker this receiver plays (-1 = mono mix). Returns the
 * Kconfig default when nothing was saved. */
esp_err_t settings_get_mc_speaker(int32_t *speaker);
esp_err_t settings_set_mc_speaker(int32_t speaker);

/* Surround speaker tuning set from the Gateway web page: level trim in
 * tenths of a dB and extra delay in us. 0 and 0 when nothing saved. */
esp_err_t settings_get_surround_tuning(int32_t *trim_db10, int32_t *delay_us);
esp_err_t settings_set_surround_tuning(int32_t trim_db10, int32_t delay_us);

/* Subwoofer crossover set from the Gateway web page, in Hz (0 = off).
 * fallback when nothing was saved. */
esp_err_t settings_get_lfe_lowpass(int32_t *hz, int32_t fallback);
esp_err_t settings_set_lfe_lowpass(int32_t hz);

/* Subwoofer LFE gain for 5.1/7.1 in dB, set from the Gateway web page.
 * fallback when nothing was saved. */
esp_err_t settings_get_lfe_gain(int32_t *db, int32_t fallback);
esp_err_t settings_set_lfe_gain(int32_t db);
