/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* HomeKit controller pairings (persistent, NVS).
 *
 * The Home app adds an AirPlay speaker to a home with a full (non-transient)
 * pair-setup, or with transient pairing followed by /pair-add. The
 * controller's pairing ID and long-term Ed25519 public key (LTPK) are stored
 * here. The home's other controllers are added with /pair-add, removed with
 * /pair-remove and listed with /pair-list. pair-verify checks a stored
 * controller's signature against its LTPK. */

#define AP2_PAIRING_ID_MAX 64
#define AP2_PAIRINGS_MAX   16
#define AP2_PERM_ADMIN     0x01

/* Loads the table from NVS. Call once, after NVS is up. */
esp_err_t ap2_pairings_init(void);

/* Number of stored controllers / whether at least one is an admin. */
size_t ap2_pairings_count(void);
bool ap2_pairings_has_admin(void);
/* True only for an existing admin with no other admin remaining. */
bool ap2_pairings_is_last_admin(const uint8_t *id, size_t id_len);

/* Looks a controller up; ltpk (32 bytes) and perm may be NULL. */
bool ap2_pairings_find(const uint8_t *id, size_t id_len, uint8_t *ltpk,
                       uint8_t *perm);

/* Adds or updates a controller. ESP_ERR_NO_MEM when the table is full,
 * ESP_ERR_INVALID_STATE when the id exists with another LTPK. */
esp_err_t ap2_pairings_add(const uint8_t *id, size_t id_len,
                           const uint8_t ltpk[32], uint8_t perm);

/* Removes a controller (ESP_ERR_NOT_FOUND if unknown). Removing the last
 * admin removes every pairing, as HAP requires. */
esp_err_t ap2_pairings_remove(const uint8_t *id, size_t id_len);

/* Removes every controller, preserving the accessory identity and settings.
 * RAM is restored if the persistent write fails. */
esp_err_t ap2_pairings_clear(void);

/* /pair-list reply: State 2 and every pairing (identifier, public key,
 * permissions), separated by TLV separators. Returns the length, 0 when it
 * does not fit. */
size_t ap2_pairings_list_tlv(uint8_t *out, size_t cap);

/* Logs the number of controllers; with detail one line per controller
 * (otherwise those lines at debug level). */
void ap2_pairings_log(const char *why, bool detail);

/* Called (from the caller's task) whenever the admin presence changes. */
typedef void (*ap2_pairings_changed_cb)(bool has_admin);
void ap2_pairings_set_changed_cb(ap2_pairings_changed_cb cb);
