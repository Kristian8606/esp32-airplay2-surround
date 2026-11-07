/* SPDX-License-Identifier: Apache-2.0 */

#include "fake_nvs.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NAMESPACES 8
#define MAX_ENTRIES 64
#define MAX_HANDLES 64
#define NAME_MAX_LEN 15

typedef struct {
  bool used;
  int ns;
  char key[NAME_MAX_LEN + 1];
  fake_nvs_type_t type;
  uint8_t *data; /* exactly len bytes, so ASan catches reads past it */
  size_t len;
} entry_t;

typedef struct {
  bool used;
  int ns;
  bool writable;
  bool dirty;
} handle_t;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static char s_namespaces[MAX_NAMESPACES][NAME_MAX_LEN + 1];
static int s_namespace_count;
static entry_t s_entries[MAX_ENTRIES];
static handle_t s_handles[MAX_HANDLES];

static esp_err_t s_open_err;
static bool s_open_any_mode;
static nvs_open_mode_t s_open_mode;
static esp_err_t s_set_err;
static char s_set_key[NAME_MAX_LEN + 1];
static esp_err_t s_commit_err;

static int s_open_count, s_set_count, s_commit_count, s_uncommitted_closes;

static uint8_t s_mac[6];
static esp_err_t s_mac_result;

static char s_log[8192];
static size_t s_log_len;

static void lock(void) { pthread_mutex_lock(&s_mutex); }
static void unlock(void) { pthread_mutex_unlock(&s_mutex); }

static int find_namespace(const char *ns) {
  for (int i = 0; i < s_namespace_count; i++) {
    if (strcmp(s_namespaces[i], ns) == 0) {
      return i;
    }
  }
  return -1;
}

static int add_namespace(const char *ns) {
  int i = find_namespace(ns);
  if (i >= 0) {
    return i;
  }
  if (s_namespace_count == MAX_NAMESPACES) {
    abort();
  }
  strcpy(s_namespaces[s_namespace_count], ns);
  return s_namespace_count++;
}

static entry_t *find_entry(int ns, const char *key) {
  for (int i = 0; i < MAX_ENTRIES; i++) {
    if (s_entries[i].used && s_entries[i].ns == ns &&
        strcmp(s_entries[i].key, key) == 0) {
      return &s_entries[i];
    }
  }
  return NULL;
}

static void store(int ns, const char *key, fake_nvs_type_t type,
                  const void *data, size_t len) {
  entry_t *e = find_entry(ns, key);
  if (e == NULL) {
    for (int i = 0; i < MAX_ENTRIES && e == NULL; i++) {
      if (!s_entries[i].used) {
        e = &s_entries[i];
      }
    }
    if (e == NULL) {
      abort();
    }
  } else {
    free(e->data);
  }
  e->used = true;
  e->ns = ns;
  strcpy(e->key, key);
  e->type = type;
  e->data = malloc(len ? len : 1);
  memcpy(e->data, data, len);
  e->len = len;
}

void fake_nvs_reset(void) {
  lock();
  for (int i = 0; i < MAX_ENTRIES; i++) {
    free(s_entries[i].data);
  }
  memset(s_entries, 0, sizeof(s_entries));
  memset(s_handles, 0, sizeof(s_handles));
  s_namespace_count = 0;
  s_open_err = s_set_err = s_commit_err = ESP_OK;
  s_open_count = s_set_count = s_commit_count = s_uncommitted_closes = 0;
  static const uint8_t mac[6] = {0x14, 0xC1, 0x9F, 0x27, 0xBE, 0xC8};
  memcpy(s_mac, mac, 6);
  s_mac_result = ESP_OK;
  s_log_len = 0;
  s_log[0] = '\0';
  unlock();
}

void fake_nvs_put(const char *ns, const char *key, fake_nvs_type_t type,
                  const void *data, size_t len) {
  lock();
  store(add_namespace(ns), key, type, data, len);
  unlock();
}

void fake_nvs_put_str(const char *ns, const char *key, const char *value) {
  fake_nvs_put(ns, key, FAKE_NVS_STR, value, strlen(value) + 1);
}

void fake_nvs_put_i32(const char *ns, const char *key, int32_t value) {
  fake_nvs_put(ns, key, FAKE_NVS_I32, &value, sizeof(value));
}

void fake_nvs_put_u8(const char *ns, const char *key, uint8_t value) {
  fake_nvs_put(ns, key, FAKE_NVS_U8, &value, sizeof(value));
}

bool fake_nvs_peek(const char *ns, const char *key, fake_nvs_type_t type,
                   void *data, size_t cap, size_t *len) {
  lock();
  int n = find_namespace(ns);
  entry_t *e = n < 0 ? NULL : find_entry(n, key);
  bool found = e != NULL && e->type == type;
  if (found) {
    memcpy(data, e->data, e->len < cap ? e->len : cap);
    if (len != NULL) {
      *len = e->len;
    }
  }
  unlock();
  return found;
}

bool fake_nvs_has_namespace(const char *ns) {
  lock();
  bool found = find_namespace(ns) >= 0;
  unlock();
  return found;
}

bool fake_nvs_has_key(const char *ns, const char *key) {
  lock();
  int n = find_namespace(ns);
  bool found = n >= 0 && find_entry(n, key) != NULL;
  unlock();
  return found;
}

void fake_nvs_fail_open(esp_err_t err, bool any_mode, nvs_open_mode_t mode) {
  lock();
  s_open_err = err;
  s_open_any_mode = any_mode;
  s_open_mode = mode;
  unlock();
}

void fake_nvs_fail_set(esp_err_t err, const char *key) {
  lock();
  s_set_err = err;
  s_set_key[0] = '\0';
  if (key != NULL) {
    snprintf(s_set_key, sizeof(s_set_key), "%s", key);
  }
  unlock();
}

void fake_nvs_fail_commit(esp_err_t err) {
  lock();
  s_commit_err = err;
  unlock();
}

#define COUNTER(name, var)                                                     \
  int name(void) {                                                             \
    lock();                                                                    \
    int v = var;                                                               \
    unlock();                                                                  \
    return v;                                                                  \
  }

COUNTER(fake_nvs_open_count, s_open_count)
COUNTER(fake_nvs_set_count, s_set_count)
COUNTER(fake_nvs_commit_count, s_commit_count)
COUNTER(fake_nvs_uncommitted_closes, s_uncommitted_closes)

int fake_nvs_handles_open(void) {
  lock();
  int n = 0;
  for (int i = 0; i < MAX_HANDLES; i++) {
    n += s_handles[i].used;
  }
  unlock();
  return n;
}

/* ---- nvs.h ---- */

static bool valid_name(const char *name) {
  return name != NULL && name[0] != '\0' && strlen(name) <= NAME_MAX_LEN;
}

/* Handles are index + 1, so 0 is never valid. */
static handle_t *get_handle(nvs_handle_t h) {
  if (h == 0 || h > MAX_HANDLES || !s_handles[h - 1].used) {
    return NULL;
  }
  return &s_handles[h - 1];
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle) {
  if (out_handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!valid_name(namespace_name)) {
    return ESP_ERR_NVS_KEY_TOO_LONG;
  }
  lock();
  s_open_count++;
  if (s_open_err != ESP_OK && (s_open_any_mode || s_open_mode == open_mode)) {
    esp_err_t err = s_open_err;
    unlock();
    return err;
  }
  int ns = find_namespace(namespace_name);
  if (ns < 0) {
    if (open_mode == NVS_READONLY) {
      unlock();
      return ESP_ERR_NVS_NOT_FOUND;
    }
    ns = add_namespace(namespace_name);
  }
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (!s_handles[i].used) {
      s_handles[i] = (handle_t){true, ns, open_mode == NVS_READWRITE, false};
      *out_handle = (nvs_handle_t)(i + 1);
      unlock();
      return ESP_OK;
    }
  }
  unlock();
  abort(); /* a handle leak */
}

void nvs_close(nvs_handle_t handle) {
  lock();
  handle_t *h = get_handle(handle);
  if (h == NULL) {
    unlock();
    abort(); /* closed twice or never opened */
  }
  if (h->dirty) {
    s_uncommitted_closes++;
  }
  h->used = false;
  unlock();
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  lock();
  handle_t *h = get_handle(handle);
  esp_err_t err = h == NULL ? ESP_ERR_NVS_INVALID_HANDLE : s_commit_err;
  if (err == ESP_OK) {
    if (!h->writable) {
      err = ESP_ERR_NVS_READ_ONLY;
    } else {
      h->dirty = false;
      s_commit_count++;
    }
  }
  unlock();
  return err;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
  if (!valid_name(key)) {
    return ESP_ERR_NVS_KEY_TOO_LONG;
  }
  lock();
  handle_t *h = get_handle(handle);
  esp_err_t err = ESP_OK;
  entry_t *e = NULL;
  if (h == NULL) {
    err = ESP_ERR_NVS_INVALID_HANDLE;
  } else if (!h->writable) {
    err = ESP_ERR_NVS_READ_ONLY;
  } else if ((e = find_entry(h->ns, key)) == NULL) {
    err = ESP_ERR_NVS_NOT_FOUND;
  } else {
    free(e->data);
    memset(e, 0, sizeof(*e));
    h->dirty = true;
    s_set_count++;
  }
  unlock();
  return err;
}

/* Finds key with that type. */
static esp_err_t lookup(nvs_handle_t handle, const char *key,
                        fake_nvs_type_t type, entry_t **out) {
  if (!valid_name(key)) {
    return ESP_ERR_NVS_KEY_TOO_LONG;
  }
  handle_t *h = get_handle(handle);
  if (h == NULL) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  entry_t *e = find_entry(h->ns, key);
  if (e == NULL) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (e->type != type) {
    return ESP_ERR_NVS_TYPE_MISMATCH;
  }
  *out = e;
  return ESP_OK;
}

static esp_err_t get_fixed(nvs_handle_t handle, const char *key,
                           fake_nvs_type_t type, void *out, size_t size) {
  if (out == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  lock();
  entry_t *e = NULL;
  esp_err_t err = lookup(handle, key, type, &e);
  if (err == ESP_OK) {
    memcpy(out, e->data, size);
  }
  unlock();
  return err;
}

/* The length rules of nvs_get_str / nvs_get_blob. */
static esp_err_t get_var(nvs_handle_t handle, const char *key,
                         fake_nvs_type_t type, void *out, size_t *length) {
  lock();
  entry_t *e = NULL;
  esp_err_t err = lookup(handle, key, type, &e);
  if (err == ESP_OK) {
    if (length == NULL) {
      err = ESP_ERR_NVS_INVALID_LENGTH;
    } else if (out == NULL) {
      *length = e->len;
    } else if (*length < e->len) {
      *length = e->len;
      err = ESP_ERR_NVS_INVALID_LENGTH;
    } else {
      memcpy(out, e->data, e->len);
      *length = e->len;
    }
  }
  unlock();
  return err;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out) {
  return get_fixed(handle, key, FAKE_NVS_U8, out, sizeof(*out));
}

esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out) {
  return get_fixed(handle, key, FAKE_NVS_I32, out, sizeof(*out));
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out,
                      size_t *length) {
  return get_var(handle, key, FAKE_NVS_STR, out, length);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *length) {
  return get_var(handle, key, FAKE_NVS_BLOB, out, length);
}

static esp_err_t set(nvs_handle_t handle, const char *key,
                     fake_nvs_type_t type, const void *data, size_t len) {
  if (!valid_name(key)) {
    return ESP_ERR_NVS_KEY_TOO_LONG;
  }
  if (data == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  lock();
  handle_t *h = get_handle(handle);
  esp_err_t err = ESP_OK;
  if (h == NULL) {
    err = ESP_ERR_NVS_INVALID_HANDLE;
  } else if (!h->writable) {
    err = ESP_ERR_NVS_READ_ONLY;
  } else if (s_set_err != ESP_OK &&
             (s_set_key[0] == '\0' || strcmp(s_set_key, key) == 0)) {
    err = s_set_err;
  } else {
    store(h->ns, key, type, data, len);
    h->dirty = true;
    s_set_count++;
  }
  unlock();
  return err;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
  return set(handle, key, FAKE_NVS_U8, &value, sizeof(value));
}

esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value) {
  return set(handle, key, FAKE_NVS_I32, &value, sizeof(value));
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key,
                      const char *value) {
  if (value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (strlen(value) + 1 > 4000) {
    return ESP_ERR_NVS_VALUE_TOO_LONG;
  }
  return set(handle, key, FAKE_NVS_STR, value, strlen(value) + 1);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length) {
  return set(handle, key, FAKE_NVS_BLOB, value, length);
}

/* ---- esp_mac.h, esp_log.h, esp_err_to_name ---- */

void fake_mac_set(const uint8_t mac[6], esp_err_t result) {
  lock();
  memcpy(s_mac, mac, 6);
  s_mac_result = result;
  unlock();
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
  if (mac == NULL || type != ESP_MAC_WIFI_STA) {
    return ESP_ERR_INVALID_ARG;
  }
  lock();
  esp_err_t err = s_mac_result;
  if (err == ESP_OK) {
    memcpy(mac, s_mac, 6);
  }
  unlock();
  return err;
}

void fake_log(char level, const char *tag, const char *format, ...) {
  lock();
  int n = snprintf(s_log + s_log_len, sizeof(s_log) - s_log_len, "%c %s: ",
                   level, tag);
  if (n > 0) {
    s_log_len += (size_t)n;
  }
  if (s_log_len < sizeof(s_log)) {
    va_list ap;
    va_start(ap, format);
    n = vsnprintf(s_log + s_log_len, sizeof(s_log) - s_log_len, format, ap);
    va_end(ap);
    if (n > 0) {
      s_log_len += (size_t)n;
    }
  }
  if (s_log_len + 1 < sizeof(s_log)) {
    s_log[s_log_len++] = '\n';
    s_log[s_log_len] = '\0';
  } else {
    s_log_len = sizeof(s_log) - 1;
  }
  unlock();
}

const char *fake_log_text(void) { return s_log; }

void fake_log_clear(void) {
  lock();
  s_log_len = 0;
  s_log[0] = '\0';
  unlock();
}

const char *esp_err_to_name(esp_err_t code) {
  switch (code) {
  case ESP_OK: return "ESP_OK";
  case ESP_FAIL: return "ESP_FAIL";
  case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
  case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
  case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
  case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
  case ESP_ERR_NVS_TYPE_MISMATCH: return "ESP_ERR_NVS_TYPE_MISMATCH";
  case ESP_ERR_NVS_READ_ONLY: return "ESP_ERR_NVS_READ_ONLY";
  case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
  case ESP_ERR_NVS_INVALID_HANDLE: return "ESP_ERR_NVS_INVALID_HANDLE";
  case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
  case ESP_ERR_NVS_VALUE_TOO_LONG: return "ESP_ERR_NVS_VALUE_TOO_LONG";
  default: return "UNKNOWN";
  }
}
