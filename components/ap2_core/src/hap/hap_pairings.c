/* SPDX-License-Identifier: Apache-2.0 */
#include "ap2_pairings.h"

#include <stdlib.h>
#include <string.h>

#include "ap2_tlv8.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "hap_pairings";

/* The same namespace, key and record layout as earlier HomeKit firmware of
 * this speaker, so a speaker already in a home stays in it. */
#define NVS_NAMESPACE "airplay"
#define NVS_KEY_PAIRS "hk_pairs"
#define TLV_PERMISSIONS 0x0B
#define TLV_SEPARATOR   0xFF

typedef struct {
  uint8_t used;
  uint8_t perm;
  uint8_t id_len;
  char id[AP2_PAIRING_ID_MAX];
  uint8_t ltpk[32];
} pairing_t;

static pairing_t *s_pairs; /* AP2_PAIRINGS_MAX entries, PSRAM */
static SemaphoreHandle_t s_lock;
static ap2_pairings_changed_cb s_cb;

#define TABLE_BYTES (sizeof(pairing_t) * AP2_PAIRINGS_MAX)

/* RTSP has an external stack. NVS writes run with an internal stack and
 * internal data, so flash cache suspension cannot strand their caller. */
typedef struct {
  pairing_t pairs[AP2_PAIRINGS_MAX];
  SemaphoreHandle_t done;
  esp_err_t err;
} save_job_t;

static void save_task(void *arg) {
  save_job_t *job = arg;
  nvs_handle_t nvs;
  job->err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (job->err == ESP_OK) {
    job->err = nvs_set_blob(nvs, NVS_KEY_PAIRS, job->pairs, TABLE_BYTES);
    if (job->err == ESP_OK) job->err = nvs_commit(nvs);
    nvs_close(nvs);
  }
  xSemaphoreGive(job->done);
  vTaskDelete(NULL);
}

static esp_err_t save_locked(void) {
  save_job_t *job = heap_caps_calloc(1, sizeof(*job),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!job) return ESP_ERR_NO_MEM;
  memcpy(job->pairs, s_pairs, TABLE_BYTES);
  job->done = xSemaphoreCreateBinary();
  esp_err_t err = ESP_ERR_NO_MEM;
  if (job->done && xTaskCreate(save_task, "hk_store", 3072, job,
                               uxTaskPriorityGet(NULL), NULL) == pdPASS) {
    xSemaphoreTake(job->done, portMAX_DELAY);
    err = job->err;
  }
  if (job->done) vSemaphoreDelete(job->done);
  heap_caps_free(job);
  return err;
}

static bool has_admin_locked(void) {
  for (int i = 0; i < AP2_PAIRINGS_MAX; i++) {
    if (s_pairs[i].used && (s_pairs[i].perm & AP2_PERM_ADMIN)) return true;
  }
  return false;
}

static int find_locked(const uint8_t *id, size_t id_len) {
  for (int i = 0; i < AP2_PAIRINGS_MAX; i++) {
    if (s_pairs[i].used && s_pairs[i].id_len == id_len &&
        memcmp(s_pairs[i].id, id, id_len) == 0) {
      return i;
    }
  }
  return -1;
}

static void notify(bool before, bool after) {
  if (before != after && s_cb) s_cb(after);
}

esp_err_t ap2_pairings_init(void) {
  if (s_pairs) return ESP_OK;
  s_pairs = heap_caps_calloc(AP2_PAIRINGS_MAX, sizeof(pairing_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_pairs) s_pairs = calloc(AP2_PAIRINGS_MAX, sizeof(pairing_t));
  s_lock = xSemaphoreCreateMutex();
  if (!s_pairs || !s_lock) {
    free(s_pairs);
    s_pairs = NULL;
    if (s_lock) vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return ESP_ERR_NO_MEM;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t len = TABLE_BYTES;
    err = nvs_get_blob(nvs, NVS_KEY_PAIRS, s_pairs, &len);
    nvs_close(nvs);
    if (err == ESP_OK && len != TABLE_BYTES) err = ESP_ERR_INVALID_STATE;
  }
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  for (int i = 0; i < AP2_PAIRINGS_MAX && err == ESP_OK; i++) {
    const pairing_t *p = &s_pairs[i];
    if (!p->used) continue;
    if (p->used != 1 || p->id_len == 0 || p->id_len > AP2_PAIRING_ID_MAX ||
        p->perm > AP2_PERM_ADMIN) { err = ESP_ERR_INVALID_STATE; break; }
    for (int j = 0; j < i; j++) {
      if (s_pairs[j].used && s_pairs[j].id_len == p->id_len &&
          !memcmp(s_pairs[j].id, p->id, p->id_len)) err = ESP_ERR_INVALID_STATE;
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Loading HomeKit pairings failed: %s", esp_err_to_name(err));
    free(s_pairs);
    s_pairs = NULL;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return err;
  }
  ap2_pairings_log("loaded", true);
  return ESP_OK;
}

size_t ap2_pairings_count(void) {
  if (!s_pairs) return 0;
  size_t n = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < AP2_PAIRINGS_MAX; i++) n += s_pairs[i].used ? 1U : 0U;
  xSemaphoreGive(s_lock);
  return n;
}

bool ap2_pairings_has_admin(void) {
  if (!s_pairs) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool admin = has_admin_locked();
  xSemaphoreGive(s_lock);
  return admin;
}

bool ap2_pairings_is_last_admin(const uint8_t *id, size_t id_len) {
  if (!s_pairs || !id || !id_len || id_len > AP2_PAIRING_ID_MAX) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const int target = find_locked(id, id_len);
  bool last = target >= 0 && (s_pairs[target].perm & AP2_PERM_ADMIN);
  for (int i = 0; last && i < AP2_PAIRINGS_MAX; ++i) {
    if (i != target && s_pairs[i].used && (s_pairs[i].perm & AP2_PERM_ADMIN))
      last = false;
  }
  xSemaphoreGive(s_lock);
  return last;
}

bool ap2_pairings_find(const uint8_t *id, size_t id_len, uint8_t *ltpk,
                       uint8_t *perm) {
  if (!s_pairs || !id || id_len == 0 || id_len > AP2_PAIRING_ID_MAX) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int i = find_locked(id, id_len);
  if (i >= 0) {
    if (ltpk) memcpy(ltpk, s_pairs[i].ltpk, 32);
    if (perm) *perm = s_pairs[i].perm;
  }
  xSemaphoreGive(s_lock);
  return i >= 0;
}

esp_err_t ap2_pairings_add(const uint8_t *id, size_t id_len,
                           const uint8_t ltpk[32], uint8_t perm) {
  if (!s_pairs || !id || id_len == 0 || id_len > AP2_PAIRING_ID_MAX || !ltpk ||
      perm > AP2_PERM_ADMIN) {
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  pairing_t before_pairs[AP2_PAIRINGS_MAX];
  memcpy(before_pairs, s_pairs, TABLE_BYTES);
  const bool before = has_admin_locked();
  esp_err_t err = ESP_OK;
  int i = find_locked(id, id_len);
  const bool existed = i >= 0;
  bool unchanged = false;
  if (existed) {
    if (memcmp(s_pairs[i].ltpk, ltpk, 32) != 0) {
      err = ESP_ERR_INVALID_STATE; /* HAP: same id, other key */
    } else if (s_pairs[i].perm == perm) {
      /* The Home hub adds the same controllers again in every management
       * session: nothing to store, the flash is spared. */
      unchanged = true;
    } else {
      s_pairs[i].perm = perm;
    }
  } else {
    for (i = 0; i < AP2_PAIRINGS_MAX && s_pairs[i].used; i++) {
    }
    if (i == AP2_PAIRINGS_MAX) {
      err = ESP_ERR_NO_MEM;
    } else {
      memset(&s_pairs[i], 0, sizeof(s_pairs[i]));
      s_pairs[i].used = 1;
      s_pairs[i].perm = perm;
      s_pairs[i].id_len = (uint8_t)id_len;
      memcpy(s_pairs[i].id, id, id_len);
      memcpy(s_pairs[i].ltpk, ltpk, 32);
    }
  }
  if (err == ESP_OK && !unchanged) {
    if (before && !has_admin_locked()) memset(s_pairs, 0, TABLE_BYTES);
    err = save_locked();
  }
  if (err != ESP_OK) memcpy(s_pairs, before_pairs, TABLE_BYTES);
  const bool after = has_admin_locked();
  xSemaphoreGive(s_lock);
  if (err == ESP_OK && unchanged) {
    ESP_LOGD(TAG, "controller %.*s already paired (%s)", (int)id_len,
             (const char *)id, (perm & AP2_PERM_ADMIN) ? "admin" : "user");
  } else if (err == ESP_OK) {
    ESP_LOGI(TAG, "controller %.*s %s (%s)", (int)id_len, (const char *)id,
             existed ? "permission changed" : "stored",
             (perm & AP2_PERM_ADMIN) ? "admin" : "user");
  }
  notify(before, after);
  return err;
}

esp_err_t ap2_pairings_remove(const uint8_t *id, size_t id_len) {
  if (!s_pairs || !id || id_len == 0 || id_len > AP2_PAIRING_ID_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  pairing_t before_pairs[AP2_PAIRINGS_MAX];
  memcpy(before_pairs, s_pairs, TABLE_BYTES);
  const bool before = has_admin_locked();
  int i = find_locked(id, id_len);
  esp_err_t err = ESP_ERR_NOT_FOUND;
  if (i >= 0) {
    memset(&s_pairs[i], 0, sizeof(s_pairs[i]));
    /* HAP: removing the last admin removes all pairings. */
    if (!has_admin_locked()) memset(s_pairs, 0, TABLE_BYTES);
    err = save_locked();
  }
  if (err != ESP_OK) memcpy(s_pairs, before_pairs, TABLE_BYTES);
  const bool after = has_admin_locked();
  xSemaphoreGive(s_lock);
  if (i >= 0) {
    ESP_LOGI(TAG, "controller %.*s removed%s", (int)id_len, (const char *)id,
             after ? "" : " (no admin left: all pairings cleared)");
  }
  notify(before, after);
  return err;
}

esp_err_t ap2_pairings_clear(void) {
  if (!s_pairs) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  pairing_t previous[AP2_PAIRINGS_MAX];
  memcpy(previous, s_pairs, TABLE_BYTES);
  const bool before = has_admin_locked();
  memset(s_pairs, 0, TABLE_BYTES);
  const esp_err_t err = save_locked();
  if (err != ESP_OK) memcpy(s_pairs, previous, TABLE_BYTES);
  const bool after = has_admin_locked();
  xSemaphoreGive(s_lock);
  if (err == ESP_OK) ESP_LOGI(TAG, "HomeKit controllers cleared");
  notify(before, after);
  return err;
}

size_t ap2_pairings_list_tlv(uint8_t *out, size_t cap) {
  ap2_tlv8_writer_t w;
  ap2_tlv8_writer_init(&w, out, cap);
  bool ok = ap2_tlv8_put_byte(&w, AP2_TLV_STATE, 2);
  if (!s_pairs) return ok ? ap2_tlv8_writer_size(&w) : 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool first = true;
  for (int i = 0; ok && i < AP2_PAIRINGS_MAX; i++) {
    if (!s_pairs[i].used) continue;
    if (!first) ok = ap2_tlv8_put(&w, TLV_SEPARATOR, NULL, 0);
    first = false;
    ok = ok &&
         ap2_tlv8_put(&w, AP2_TLV_IDENTIFIER, (const uint8_t *)s_pairs[i].id,
                      s_pairs[i].id_len) &&
         ap2_tlv8_put(&w, AP2_TLV_PUBLIC_KEY, s_pairs[i].ltpk, 32) &&
         ap2_tlv8_put_byte(&w, TLV_PERMISSIONS, s_pairs[i].perm);
  }
  xSemaphoreGive(s_lock);
  return ok ? ap2_tlv8_writer_size(&w) : 0;
}

void ap2_pairings_log(const char *why, bool detail) {
  if (!s_pairs) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int n = 0;
  for (int i = 0; i < AP2_PAIRINGS_MAX; i++) n += s_pairs[i].used ? 1 : 0;
  ESP_LOGI(TAG, "HomeKit pairings (%s): %d controller%s", why ? why : "-", n,
           n == 1 ? "" : "s");
  for (int i = 0; i < AP2_PAIRINGS_MAX; i++) {
    const pairing_t *p = &s_pairs[i];
    if (!p->used) continue;
    if (detail) {
      ESP_LOGI(TAG, "    %.*s  %s  ltpk %02x%02x%02x%02x...", (int)p->id_len,
               p->id, (p->perm & AP2_PERM_ADMIN) ? "admin" : "user ",
               p->ltpk[0], p->ltpk[1], p->ltpk[2], p->ltpk[3]);
    } else {
      ESP_LOGD(TAG, "    %.*s  %s  ltpk %02x%02x%02x%02x...", (int)p->id_len,
               p->id, (p->perm & AP2_PERM_ADMIN) ? "admin" : "user ",
               p->ltpk[0], p->ltpk[1], p->ltpk[2], p->ltpk[3]);
    }
  }
  xSemaphoreGive(s_lock);
}

void ap2_pairings_set_changed_cb(ap2_pairings_changed_cb cb) { s_cb = cb; }
