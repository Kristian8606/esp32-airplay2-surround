/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* The part of mdns_airplay that needs no ESP-IDF: instance names and TXT
 * values. */

#include <stddef.h>
#include <stdint.h>

#include "airplay_identity.h"

#define MDNS_AIRPLAY_PORT 7000
/* A DNS label, so one instance name [RFC 1035 2.3.4, RFC 6763 4.1.1]. */
#define MDNS_AIRPLAY_LABEL_MAX 63
/* The device name as read from the settings: 64 bytes and the NUL. */
#define MDNS_AIRPLAY_DEVICE_NAME_LEN 65
#define MDNS_AIRPLAY_FALLBACK_NAME "AirPlay Surround Gateway"
/* "AA:BB:CC:DD:EE:FF" and the NUL. */
#define MDNS_AIRPLAY_MAC_STR_LEN 18
#define MDNS_AIRPLAY_PUBLIC_KEY_LEN 32

#define MDNS_AIRPLAY_AIRPLAY_TXT_COUNT 9
#define MDNS_AIRPLAY_RAOP_TXT_COUNT 12

/* One TXT record; the same layout as mdns_txt_item_t. */
typedef struct {
  const char *key;
  const char *value;
} mdns_airplay_txt_t;

/* The TXT records of both services. The items point into the struct, so it
 * must not be copied or moved after mdns_airplay_build_txt(). */
typedef struct {
  char deviceid[MDNS_AIRPLAY_MAC_STR_LEN];
  char features[24]; /* "0xFFFFFFFF,0xFFFFFFFF" */
  char flags[12];    /* "0xFFFFFFFF" */
  char pk[2 * MDNS_AIRPLAY_PUBLIC_KEY_LEN + 1];
  char pi[AIRPLAY_PAIRING_ID_LEN];
  char vv[12];
  mdns_airplay_txt_t airplay[MDNS_AIRPLAY_AIRPLAY_TXT_COUNT];
  mdns_airplay_txt_t raop[MDNS_AIRPLAY_RAOP_TXT_COUNT];
} mdns_airplay_txt_records_t;

/* Length of the longest prefix of s with at most max_len bytes that does
 * not split a UTF-8 sequence: the byte after the prefix is never 10xxxxxx. */
size_t mdns_airplay_utf8_prefix_len(const char *s, size_t max_len);

/* The _airplay._tcp instance name: the device name cut to 63 bytes. If the
 * cut leaves nothing (only possible with invalid UTF-8), the fallback name
 * is used instead. */
void mdns_airplay_airplay_instance(const char *device_name,
                                   char out[MDNS_AIRPLAY_LABEL_MAX + 1]);

/* The _raop._tcp instance name: the MAC without colons, "@", then the
 * device name cut so that the whole name has at most 63 bytes, or the
 * fallback name when the cut leaves nothing. */
void mdns_airplay_raop_instance(const char *mac_str, const char *device_name,
                                char out[MDNS_AIRPLAY_LABEL_MAX + 1]);

/* Fills both TXT record lists. mac_str and pairing_id are copied (cut to
 * their buffers); public_key is 32 bytes and never NULL. */
void mdns_airplay_build_txt(mdns_airplay_txt_records_t *rec,
                            const char *mac_str, const uint8_t *public_key,
                            const char *pairing_id);
