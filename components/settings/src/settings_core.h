/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* The rules of the settings that need no ESP-IDF: the layout of the network
 * list blob and its changes, the default device name, UTF-8 cutting and the
 * volume rounding. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"

#define SETTINGS_WIFI_LIST_MAGIC 0x57494649u
#define SETTINGS_WIFI_LIST_VERSION 1u
#define SETTINGS_WIFI_LIST_HEADER_SIZE 12u
#define SETTINGS_WIFI_SLOT_SIZE (SETTINGS_WIFI_SSID_LEN + SETTINGS_WIFI_PASSWORD_LEN)
#define SETTINGS_WIFI_LIST_SIZE                                                \
  (SETTINGS_WIFI_LIST_HEADER_SIZE +                                            \
   SETTINGS_MAX_WIFI_NETWORKS * SETTINGS_WIFI_SLOT_SIZE) /* 796 */

/* "ESP32 AirPlay " and three hex digits, with the NUL. */
#define SETTINGS_DEFAULT_NAME_SIZE 18

typedef struct {
  uint16_t count;    /* 0..8 */
  uint16_t last_set; /* index of the network set last */
  uint16_t next;     /* slot replaced when the list is full */
  settings_wifi_network_t slot[SETTINGS_MAX_WIFI_NETWORKS];
} settings_wifi_list_t;

/* An empty list: count 0, last-set 0, next 0, all slots zero. */
void settings_wifi_list_clear(settings_wifi_list_t *list);

/* Reads a stored blob of len bytes. False (and an empty list) when it is not
 * a valid list. */
bool settings_wifi_list_parse(const uint8_t *blob, size_t len,
                              settings_wifi_list_t *list);

/* Writes the list as the SETTINGS_WIFI_LIST_SIZE-byte blob. */
void settings_wifi_list_serialize(const settings_wifi_list_t *list,
                                  uint8_t blob[SETTINGS_WIFI_LIST_SIZE]);

/* True when ssid (non-NULL, 1..32 bytes) and password (NULL or 0..64 bytes)
 * can be saved. */
bool settings_wifi_credentials_valid(const char *ssid, const char *password);

/* Adds or updates a network whose arguments passed
 * settings_wifi_credentials_valid(); password NULL is "". */
void settings_wifi_list_put(settings_wifi_list_t *list, const char *ssid,
                            const char *password);

/* The default device name from the Wi-Fi station MAC. */
void settings_default_device_name(const uint8_t mac[6],
                                  char out[SETTINGS_DEFAULT_NAME_SIZE]);

/* Copies src into out (len > 0 bytes), cut to len - 1 bytes at a whole
 * UTF-8 character. */
void settings_copy_name(char *out, size_t len, const char *src);

/* The volume in thousandths of a dB as stored in NVS. */
int32_t settings_volume_to_mdb(float volume_db);
