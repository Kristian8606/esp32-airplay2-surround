/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* A fake NVS for host tests: namespaces, typed keys, read-only and
 * read-write handles, ESP_ERR_NVS_NOT_FOUND for a missing namespace or key,
 * injected errors, call counts. Also the fake MAC, log and esp_err_to_name. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_mac.h"
#include "nvs.h"

typedef enum {
  FAKE_NVS_U8,
  FAKE_NVS_I32,
  FAKE_NVS_STR,
  FAKE_NVS_BLOB,
} fake_nvs_type_t;

/* Empties the storage, clears injected errors, counters, the log and sets
 * the MAC to 14:C1:9F:27:BE:C8. */
void fake_nvs_reset(void);

/* Storage seen directly, as other firmware would have left it. */
void fake_nvs_put(const char *ns, const char *key, fake_nvs_type_t type,
                  const void *data, size_t len);
void fake_nvs_put_str(const char *ns, const char *key, const char *value);
void fake_nvs_put_i32(const char *ns, const char *key, int32_t value);
void fake_nvs_put_u8(const char *ns, const char *key, uint8_t value);
/* Copies the value out (up to cap bytes). False when the key is not there
 * with that type. */
bool fake_nvs_peek(const char *ns, const char *key, fake_nvs_type_t type,
                   void *data, size_t cap, size_t *len);
bool fake_nvs_has_namespace(const char *ns);
bool fake_nvs_has_key(const char *ns, const char *key);

/* Errors to inject. open: every nvs_open with that mode (or any mode with
 * any_mode) fails. set: every nvs_set_* of that key (NULL: any key) fails.
 * commit / erase: every call fails. ESP_OK clears it. */
void fake_nvs_fail_open(esp_err_t err, bool any_mode, nvs_open_mode_t mode);
void fake_nvs_fail_set(esp_err_t err, const char *key);
void fake_nvs_fail_commit(esp_err_t err);

/* Counters since the last reset. */
int fake_nvs_open_count(void);
int fake_nvs_handles_open(void);   /* handles not closed */
int fake_nvs_set_count(void);      /* successful nvs_set_* and erases */
int fake_nvs_commit_count(void);   /* successful commits */
int fake_nvs_uncommitted_closes(void); /* closed after writes, no commit */

void fake_mac_set(const uint8_t mac[6], esp_err_t result);

/* The log lines, "E settings: text\n" each. */
const char *fake_log_text(void);
void fake_log_clear(void);
