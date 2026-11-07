/* SPDX-License-Identifier: Apache-2.0 */

#include "settings_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SLOT_OFFSET(i)                                                         \
  (SETTINGS_WIFI_LIST_HEADER_SIZE + (size_t)(i) * SETTINGS_WIFI_SLOT_SIZE)

static uint16_t get_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

void settings_wifi_list_clear(settings_wifi_list_t *list) {
  memset(list, 0, sizeof(*list));
}

bool settings_wifi_list_parse(const uint8_t *blob, size_t len,
                              settings_wifi_list_t *list) {
  settings_wifi_list_clear(list);
  if (blob == NULL || len != SETTINGS_WIFI_LIST_SIZE) {
    return false;
  }
  uint16_t count = get_le16(blob + 6);
  uint16_t last_set = get_le16(blob + 8);
  uint16_t next = get_le16(blob + 10);
  if (get_le32(blob) != SETTINGS_WIFI_LIST_MAGIC ||
      get_le16(blob + 4) != SETTINGS_WIFI_LIST_VERSION ||
      count > SETTINGS_MAX_WIFI_NETWORKS ||
      (count > 0 && last_set >= count) || next >= SETTINGS_MAX_WIFI_NETWORKS) {
    return false;
  }
  for (uint16_t i = 0; i < count; i++) {
    const uint8_t *ssid = blob + SLOT_OFFSET(i);
    const uint8_t *password = ssid + SETTINGS_WIFI_SSID_LEN;
    if (memchr(ssid, 0, SETTINGS_WIFI_SSID_LEN) == NULL ||
        memchr(password, 0, SETTINGS_WIFI_PASSWORD_LEN) == NULL) {
      settings_wifi_list_clear(list);
      return false;
    }
    /* Copy up to the NUL only, so that the slot stays NUL-padded. */
    strcpy(list->slot[i].ssid, (const char *)ssid);
    strcpy(list->slot[i].password, (const char *)password);
  }
  list->count = count;
  list->last_set = last_set;
  list->next = next;
  return true;
}

void settings_wifi_list_serialize(const settings_wifi_list_t *list,
                                  uint8_t blob[SETTINGS_WIFI_LIST_SIZE]) {
  memset(blob, 0, SETTINGS_WIFI_LIST_SIZE);
  put_le32(blob, SETTINGS_WIFI_LIST_MAGIC);
  put_le16(blob + 4, SETTINGS_WIFI_LIST_VERSION);
  put_le16(blob + 6, list->count);
  put_le16(blob + 8, list->last_set);
  put_le16(blob + 10, list->next);
  for (uint16_t i = 0; i < list->count && i < SETTINGS_MAX_WIFI_NETWORKS;
       i++) {
    uint8_t *ssid = blob + SLOT_OFFSET(i);
    uint8_t *password = ssid + SETTINGS_WIFI_SSID_LEN;
    /* strnlen keeps the NUL padding even if a slot were not terminated. */
    memcpy(ssid, list->slot[i].ssid,
           strnlen(list->slot[i].ssid, SETTINGS_WIFI_SSID_LEN - 1));
    memcpy(password, list->slot[i].password,
           strnlen(list->slot[i].password, SETTINGS_WIFI_PASSWORD_LEN - 1));
  }
}

bool settings_wifi_credentials_valid(const char *ssid, const char *password) {
  if (ssid == NULL || ssid[0] == '\0' ||
      strnlen(ssid, SETTINGS_WIFI_SSID_LEN) >= SETTINGS_WIFI_SSID_LEN) {
    return false;
  }
  return password == NULL || strnlen(password, SETTINGS_WIFI_PASSWORD_LEN) <
                                 SETTINGS_WIFI_PASSWORD_LEN;
}

static void set_slot(settings_wifi_network_t *slot, const char *ssid,
                     const char *password) {
  memset(slot, 0, sizeof(*slot));
  strcpy(slot->ssid, ssid);
  strcpy(slot->password, password);
}

void settings_wifi_list_put(settings_wifi_list_t *list, const char *ssid,
                            const char *password) {
  if (password == NULL) {
    password = "";
  }
  for (uint16_t i = 0; i < list->count; i++) {
    if (strcmp(list->slot[i].ssid, ssid) == 0) {
      set_slot(&list->slot[i], ssid, password);
      list->last_set = i;
      return;
    }
  }
  if (list->count < SETTINGS_MAX_WIFI_NETWORKS) {
    set_slot(&list->slot[list->count], ssid, password);
    list->last_set = list->count;
    list->count++;
    return;
  }
  set_slot(&list->slot[list->next], ssid, password);
  list->last_set = list->next;
  list->next = (uint16_t)((list->next + 1) % SETTINGS_MAX_WIFI_NETWORKS);
}

void settings_default_device_name(const uint8_t mac[6],
                                  char out[SETTINGS_DEFAULT_NAME_SIZE]) {
  snprintf(out, SETTINGS_DEFAULT_NAME_SIZE, "%s %X%02X",
           SETTINGS_DEFAULT_DEVICE_NAME, (unsigned)(mac[4] & 0x0F),
           (unsigned)mac[5]);
}

static bool is_continuation(char c) {
  return ((unsigned char)c & 0xC0) == 0x80;
}

void settings_copy_name(char *out, size_t len, const char *src) {
  size_t n = strlen(src);
  if (n > len - 1) {
    /* src[n] is the first byte left out. When it continues a character, back
     * up to that character's lead byte (at most 3 bytes in UTF-8). Text that
     * is not UTF-8 there is cut byte-wise. */
    size_t cut = len - 1;
    n = cut;
    for (int i = 0; i < 3 && n > 0 && is_continuation(src[n]); i++) {
      n--;
    }
    if (is_continuation(src[n])) {
      n = cut;
    }
  }
  memcpy(out, src, n);
  out[n] = '\0';
}

int32_t settings_volume_to_mdb(float volume_db) {
  float mdb = volume_db * 1000;
  /* NaN is 0 and values outside int32_t saturate (spec answer 5); the cast
   * itself would be undefined there. */
  if (isnan(mdb)) {
    return 0;
  }
  if (mdb >= 2147483648.0f) {
    return INT32_MAX;
  }
  if (mdb < -2147483648.0f) {
    return INT32_MIN;
  }
  return (int32_t)mdb;
}
