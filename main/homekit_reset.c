/* SPDX-License-Identifier: Apache-2.0 */
#include "homekit_reset.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "web_server_internal.h"
#include "sound_engine.h"
#include "rtsp_server.h"

#define RESET_BODY_CAP 128
static const char *TAG = "homekit_reset";
static bool s_ready;

void homekit_reset_mark_ready(void) {
  __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE);
}

static esp_err_t reset_homekit(void) {
  /* These are the persistent keys owned by hap.c and hap_pairings.c.
   * RTSP is idle before this runs; reboot discards their cached RAM tables. */
  static const char *const keys[] = {"hk_pairs", "ed25519_pub", "ed25519_sec"};
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("airplay", NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  for (size_t i = 0; err == ESP_OK && i < sizeof(keys) / sizeof(keys[0]); ++i) {
    err = nvs_erase_key(nvs, keys[i]);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  }
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}

static esp_err_t stop_for_reset(void) {
  esp_err_t err = web_role_source_stop();
  for (unsigned waited = 0; !web_role_source_idle() && waited < 5000; waited += 20)
    vTaskDelay(pdMS_TO_TICKS(20));
  if (!web_role_source_idle()) err = ESP_ERR_TIMEOUT;
  if (err == ESP_OK && sound_engine_is_initialized())
    err = sound_engine_release_for_wifi_scan();
  return err;
}

#ifdef CONFIG_AIRPLAY_HOMEKIT
#define HOMEKIT_RESET_STACK_BYTES 3072
static StaticTask_t s_homekit_reset_tcb;
static StackType_t s_homekit_reset_stack[HOMEKIT_RESET_STACK_BYTES / sizeof(StackType_t)];
static TaskHandle_t s_homekit_reset_task;
static httpd_handle_t s_homekit_reset_server;

static void homekit_reset_work(void *arg) {
  (void)arg;
  /* Run on the same HTTP task as Wi-Fi scans, latency measurement, OTA and
   * the web reset. No other web handler may release or restore audio until
   * this reset reboots. Its existing internal stack is safe for NVS. */
  vTaskPrioritySet(NULL, 1);
  rtsp_server_begin_reset();
  /* The response is already sent; let its connection finish before stopping
   * the server. */
  vTaskDelay(pdMS_TO_TICKS(500));
  ESP_LOGW(TAG, "Apple Home removal: stopping AirPlay before HomeKit reset");
  web_log_memory("homekit-reset-before");
  esp_err_t err = stop_for_reset();
  if (err == ESP_OK) {
    web_log_memory("homekit-reset-audio-released");
    err = reset_homekit();
  }
  if (err == ESP_OK) ESP_LOGW(TAG, "HomeKit keys erased; restarting for new pairing");
  else ESP_LOGE(TAG, "HomeKit removal reset failed: %s; restarting", esp_err_to_name(err));
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
  for (;;) vTaskDelay(portMAX_DELAY);
}

static void homekit_reset_task(void *arg) {
  (void)arg;
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  while (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) vTaskDelay(1);
  /* Only dispatch here: audio lifecycle changes are serialized on HTTP.
   * A transiently full control socket must not lose an accepted reset. */
  for (unsigned attempt = 0; attempt < 50; ++attempt) {
    if (httpd_queue_work(s_homekit_reset_server, homekit_reset_work, NULL) == ESP_OK)
      for (;;) vTaskDelay(portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  ESP_LOGE(TAG, "HomeKit reset could not reach web worker; restarting without erasing keys");
  esp_restart();
  for (;;) vTaskDelay(portMAX_DELAY);
}

void homekit_reset_schedule(void) {
  /* Created before the AirPlay server starts. No heap allocation here. */
  xTaskNotifyGive(s_homekit_reset_task);
}
#endif

static esp_err_t reset_post(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "Still starting. Try again shortly.");
  }
  char type[64], site[32];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
      strncmp(type, "application/json", 16) != 0 ||
      (type[16] != '\0' && type[16] != ';')) {
    httpd_resp_set_status(req, "415 Unsupported Media Type");
    return httpd_resp_sendstr(req, "Use application/json.");
  }
  if (httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", site, sizeof(site)) == ESP_OK &&
      strcmp(site, "cross-site") == 0) {
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, "Cross-site reset rejected.");
  }
  char body[RESET_BODY_CAP];
  if (web_recv_json(req, body, sizeof(body)) != ESP_OK ||
      memchr(body, '\0', req->content_len) != NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid reset request");
    return ESP_FAIL; /* close a partially read/oversized request */
  }
  cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
  const cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "mode");
  const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(json, "confirm");
  const bool homekit = cJSON_IsString(mode) && strcmp(mode->valuestring, "homekit") == 0;
  const bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 2 &&
      homekit && cJSON_IsString(confirm) &&
      strcmp(confirm->valuestring, "RESET HOMEKIT") == 0;
  cJSON_Delete(json);
  if (!valid)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Confirm the selected reset");

  /* The one HTTP task serializes web setters. Keep it inside this handler
   * until reboot, so no queued request can write cached settings after erase.
   * Its existing internal stack is safe while flash caches are suspended. */
  rtsp_server_begin_reset();
  vTaskPrioritySet(NULL, 1);
  esp_err_t err = stop_for_reset();
  if (err == ESP_OK) {
    ESP_LOGW(TAG, "HomeKit reset: AirPlay stopped, erasing HomeKit keys");
    err = reset_homekit();
  }
  httpd_resp_set_type(req, HTTPD_TYPE_JSON);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (err == ESP_OK) {
    httpd_resp_sendstr(req,
        "{\"success\":true,\"mode\":\"homekit\",\"restarting\":true}");
  } else {
    ESP_LOGE(TAG, "Reset incomplete: %s; restarting", esp_err_to_name(err));
    httpd_resp_set_status(req, "500 Internal Server Error");
    char answer[160];
    snprintf(answer, sizeof(answer),
             "{\"success\":false,\"restarting\":true,\"error\":\"Reset incomplete (%s). Restarting; retry after reboot.\"}",
             esp_err_to_name(err));
    httpd_resp_sendstr(req, answer);
  }
  /* Even partial erasure must not return to live services with stale keys.
   * The response reports erase outcome, not just acceptance of the request. */
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

esp_err_t homekit_reset_register(httpd_handle_t server) {
#ifdef CONFIG_AIRPLAY_HOMEKIT
  if (!s_homekit_reset_task) {
    s_homekit_reset_server = server;
    s_homekit_reset_task = xTaskCreateStatic(homekit_reset_task, "hk_reset",
        sizeof(s_homekit_reset_stack), NULL, 1, s_homekit_reset_stack,
        &s_homekit_reset_tcb);
    if (!s_homekit_reset_task) return ESP_ERR_NO_MEM;
  }
#endif
  const httpd_uri_t uri = {
      .uri = "/api/system/reset", .method = HTTP_POST, .handler = reset_post};
  return httpd_register_uri_handler(server, &uri);
}
