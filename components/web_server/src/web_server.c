/* SPDX-License-Identifier: Apache-2.0 */

/* The shared web server of both firmwares: starts esp_http_server and serves
 * the shared pages and APIs. Each firmware adds its own through
 * web_role_register(). See spec/spec.md. */

#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "firmware_id.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_stream.h"
#include "ota.h"
#include "settings.h"
#include "web_priv.h"
#include "web_rules.h"
#include "web_server_internal.h"
#include "wifi.h"

static const char *TAG = "web_server";

/* web_rules_reset_reason_name() counts on the ESP-IDF order. */
_Static_assert(ESP_RST_UNKNOWN == 0 && ESP_RST_POWERON == 1 &&
                   ESP_RST_EXT == 2 && ESP_RST_SW == 3 && ESP_RST_PANIC == 4 &&
                   ESP_RST_INT_WDT == 5 && ESP_RST_TASK_WDT == 6 &&
                   ESP_RST_WDT == 7 && ESP_RST_DEEPSLEEP == 8 &&
                   ESP_RST_BROWNOUT == 9 && ESP_RST_SDIO == 10,
               "esp_reset_reason_t order");

extern const uint8_t index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_web_index_html_end");
extern const uint8_t logs_html_start[] asm("_binary_web_logs_html_start");
extern const uint8_t logs_html_end[] asm("_binary_web_logs_html_end");
extern const uint8_t speedtest_html_start[] asm(
    "_binary_web_speedtest_html_start");
extern const uint8_t speedtest_html_end[] asm("_binary_web_speedtest_html_end");

#define WIFI_CONFIG_BODY_CAP 512
#define DEVICE_NAME_BODY_CAP 256
#define DEVICE_NAME_CAP 65
#define REBOOT_DELAY_MS 500
#define RESTART_DELAY_MS 200
#define DOWNLOAD_CHUNK 2048
#define UPLOAD_CHUNK 1024
#define SETUP_URL "http://192.168.4.1/"

static httpd_handle_t s_server;

static esp_err_t send_invalid_body(httpd_req_t *req) {
  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
  /* The body may be partly read: close the connection. */
  return ESP_FAIL;
}

/* ---- Pages ---- */

static esp_err_t index_get(httpd_req_t *req) {
  return web_serve_page(req, index_html_start, index_html_end);
}

static esp_err_t logs_get(httpd_req_t *req) {
  return web_serve_page(req, logs_html_start, logs_html_end);
}

static esp_err_t speedtest_page_get(httpd_req_t *req) {
  return web_serve_page(req, speedtest_html_start, speedtest_html_end);
}

static esp_err_t favicon_get(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, NULL, 0);
}

/* ---- System ---- */

static esp_err_t system_info_get(httpd_req_t *req) {
  char ip[16] = "";
  if (wifi_get_ip_str(ip, sizeof(ip)) != ESP_OK) {
    ip[0] = '\0';
  }
  ip[sizeof(ip) - 1] = '\0';

  char mac[18] = "";
  wifi_get_mac_str(mac, sizeof(mac));
  mac[sizeof(mac) - 1] = '\0';

  char name[DEVICE_NAME_CAP] = "";
  if (settings_get_device_name(name, sizeof(name)) != ESP_OK) {
    name[0] = '\0';
  }
  name[sizeof(name) - 1] = '\0';

  char build[64] = "";
  firmware_id(build, sizeof(build));
  build[sizeof(build) - 1] = '\0';

  const esp_app_desc_t *app = esp_app_get_description();
  char version[sizeof(app->version) + 1];
  snprintf(version, sizeof(version), "%.*s",
           (int)strnlen(app->version, sizeof(app->version)), app->version);

  web_rules_system_info_t info = {
      .ip = ip,
      .mac = mac,
      .device_name = name,
      .wifi_connected = wifi_is_connected(),
      .role = web_role_name(),
      .free_heap = esp_get_free_heap_size(),
      .firmware_version = version,
      .firmware_build = build,
      .reset_reason = web_rules_reset_reason_name((int)esp_reset_reason()),
      .uptime_s = esp_timer_get_time() / 1000000,
  };

  wifi_ap_record_t ap;
  memset(&ap, 0, sizeof(ap));
  char ssid[WEB_RULES_SSID_MAX + 1] = "";
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    size_t len = strnlen((const char *)ap.ssid, WEB_RULES_SSID_MAX);
    memcpy(ssid, ap.ssid, len);
    ssid[len] = '\0';
    info.have_ap = true;
    info.wifi_ssid = ssid;
    memcpy(info.wifi_bssid, ap.bssid, sizeof(info.wifi_bssid));
    info.wifi_rssi = ap.rssi;
    info.wifi_channel = ap.primary;
    info.wifi_phy = web_rules_phy_name(ap.phy_11n, ap.phy_11g, ap.phy_11b);
  }

  web_send_json(req, web_rules_system_info_json(&info));
  return ESP_OK;
}

static esp_err_t system_restart_post(httpd_req_t *req) {
  if (!web_admin_ok(req)) {
    return ESP_OK;
  }
  httpd_resp_sendstr(req, "Restarting\n");
  vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
  esp_restart();
  return ESP_OK;
}

static esp_err_t ota_update_post(httpd_req_t *req) {
  if (!web_admin_ok(req)) {
    return ESP_OK;
  }
  esp_err_t err = web_role_source_stop();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "OTA: stopping the audio source: %s", esp_err_to_name(err));
  }
  /* The image is written only once nothing plays any more. */
  if (!web_source_wait_idle()) {
    ESP_LOGE(TAG, "OTA: the audio source did not stop; update refused");
    esp_err_t start_err = web_role_source_start();
    if (start_err != ESP_OK) {
      ESP_LOGE(TAG, "OTA: restarting the audio source: %s",
               esp_err_to_name(start_err));
    }
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "The audio source did not stop; try again\n");
    return ESP_OK;
  }
  err = ota_start_from_http(req);
  if (err == ESP_OK) {
    httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
    return ESP_OK;
  }
  ESP_LOGE(TAG, "OTA failed (%s)", esp_err_to_name(err));
  esp_err_t start_err = web_role_source_start();
  if (start_err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: restarting the audio source: %s",
             esp_err_to_name(start_err));
  }
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                      esp_err_to_name(err));
  return ESP_FAIL;
}

/* ---- Wi-Fi and device name ---- */

static cJSON *scan_json(const wifi_ap_record_t *list, uint16_t count) {
  cJSON *root = web_rules_scan_begin();
  bool ok = root != NULL;
  for (uint16_t i = 0; ok && list != NULL && i < count; i++) {
    ok = web_rules_scan_add(root, list[i].ssid, sizeof(list[i].ssid),
                            list[i].rssi, list[i].primary);
  }
  ok = ok && web_rules_scan_end(root);
  if (!ok) {
    cJSON_Delete(root);
    return web_rules_error_json(esp_err_to_name(ESP_ERR_NO_MEM));
  }
  return root;
}

static esp_err_t wifi_scan_get(httpd_req_t *req) {
  bool paused = false;
  if (web_audio_pause(req, "Wi-Fi scan", &paused) != ESP_OK) {
    return ESP_OK; /* answered */
  }

  wifi_ap_record_t *list = NULL;
  uint16_t count = 0;
  cJSON *answer;
  esp_err_t err = wifi_scan(&list, &count);
  if (err == ESP_OK) {
    answer = scan_json(list, count);
    free(list);
  } else {
    answer = web_rules_error_json(esp_err_to_name(err));
  }

  if (paused && web_audio_resume(req, "Wi-Fi scan") != ESP_OK) {
    cJSON_Delete(answer);
    return ESP_OK; /* answered */
  }
  web_send_json(req, answer);
  return ESP_OK;
}

/* A string member of a JSON object, else NULL. */
static const char *json_string(const cJSON *obj, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t wifi_config_post(httpd_req_t *req) {
  if (!web_admin_ok(req)) {
    return ESP_OK;
  }
  char body[WIFI_CONFIG_BODY_CAP];
  if (web_recv_json(req, body, sizeof(body)) != ESP_OK) {
    return send_invalid_body(req);
  }
  /* Data after the first complete JSON value is ignored (decided). */
  cJSON *in = cJSON_Parse(body);
  const char *ssid = cJSON_IsObject(in) ? json_string(in, "ssid") : NULL;
  if (ssid == NULL) {
    cJSON_Delete(in);
    web_send_json(req, web_rules_error_json("Invalid SSID"));
    return ESP_OK;
  }
  const char *password = json_string(in, "password");
  esp_err_t err =
      settings_set_wifi_credentials(ssid, password != NULL ? password : "");
  cJSON_Delete(in);
  if (err != ESP_OK) {
    web_send_json(req, web_rules_error_json(esp_err_to_name(err)));
    return ESP_OK;
  }
  web_send_json(req, web_rules_ok_json());
  vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
  esp_restart();
  return ESP_OK;
}

static esp_err_t device_name_post(httpd_req_t *req) {
  if (!web_admin_ok(req)) {
    return ESP_OK;
  }
  char body[DEVICE_NAME_BODY_CAP];
  if (web_recv_json(req, body, sizeof(body)) != ESP_OK) {
    return send_invalid_body(req);
  }
  cJSON *in = cJSON_Parse(body);
  const char *name = cJSON_IsObject(in) ? json_string(in, "name") : NULL;
  if (name == NULL) {
    cJSON_Delete(in);
    web_send_json(req, web_rules_error_json("Invalid name"));
    return ESP_OK;
  }
  esp_err_t err = settings_set_device_name(name);
  if (err == ESP_OK) {
    wifi_set_hostname(name);
  }
  cJSON_Delete(in);
  web_send_json(req, err == ESP_OK ? web_rules_ok_json()
                                   : web_rules_error_json(esp_err_to_name(err)));
  return ESP_OK;
}

/* ---- Speed test ---- */

static esp_err_t speedtest_ping_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "ok");
}

static uint32_t download_size(httpd_req_t *req) {
  size_t len = httpd_req_get_url_query_len(req);
  if (len == 0) {
    return web_rules_download_size(NULL);
  }
  /* A value is never longer than the query: nothing is cut off. */
  char *query = malloc(len + 1);
  char *value = malloc(len + 1);
  const char *found = NULL;
  if (query != NULL && value != NULL &&
      httpd_req_get_url_query_str(req, query, len + 1) == ESP_OK &&
      httpd_query_key_value(query, "bytes", value, len + 1) == ESP_OK) {
    found = value;
  }
  uint32_t size = web_rules_download_size(found);
  free(query);
  free(value);
  return size;
}

static esp_err_t speedtest_download_get(httpd_req_t *req) {
  static const char zeros[DOWNLOAD_CHUNK];
  uint32_t left = download_size(req);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  while (left > 0) {
    uint32_t n = left < DOWNLOAD_CHUNK ? left : DOWNLOAD_CHUNK;
    if (httpd_resp_send_chunk(req, zeros, (ssize_t)n) != ESP_OK) {
      return ESP_FAIL;
    }
    left -= n;
  }
  return httpd_resp_send_chunk(req, NULL, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t speedtest_upload_post(httpd_req_t *req) {
  char buf[UPLOAD_CHUNK];
  size_t len = req->content_len;
  if (web_recv_body(req, buf, sizeof(buf), len, false) != ESP_OK) {
    return ESP_FAIL;
  }
  char answer[32];
  snprintf(answer, sizeof(answer), "received=%lu", (unsigned long)len);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, answer);
}

/* ---- Captive portal ---- */

static esp_err_t send_setup_redirect(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", SETUP_URL);
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t captive_get(httpd_req_t *req) {
  return send_setup_redirect(req);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t error) {
  (void)error;
  if (!wifi_is_connected()) {
    return send_setup_redirect(req); /* the set-up access point */
  }
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "Not Found");
}

/* ---- Start ---- */

static const httpd_uri_t SHARED_HANDLERS[] = {
    {.uri = "/", .method = HTTP_GET, .handler = index_get},
    {.uri = "/logs", .method = HTTP_GET, .handler = logs_get},
    {.uri = "/speedtest", .method = HTTP_GET, .handler = speedtest_page_get},
    {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get},
    {.uri = "/api/system/info", .method = HTTP_GET, .handler = system_info_get},
    {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get},
    {.uri = "/api/wifi/config", .method = HTTP_POST, .handler = wifi_config_post},
    {.uri = "/api/device/name", .method = HTTP_POST, .handler = device_name_post},
    {.uri = "/api/system/restart",
     .method = HTTP_POST,
     .handler = system_restart_post},
    {.uri = "/api/ota/update", .method = HTTP_POST, .handler = ota_update_post},
    {.uri = "/api/speedtest/ping",
     .method = HTTP_GET,
     .handler = speedtest_ping_get},
    {.uri = "/api/speedtest/download",
     .method = HTTP_GET,
     .handler = speedtest_download_get},
    {.uri = "/api/speedtest/upload",
     .method = HTTP_POST,
     .handler = speedtest_upload_post},
    {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_get},
    {.uri = "/library/test/success.html",
     .method = HTTP_GET,
     .handler = captive_get},
    {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_get},
    {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_get},
};

esp_err_t web_server_start(uint16_t port) {
  if (s_server != NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.task_priority = 3;
  config.stack_size = 8192; /* internal RAM (default): handlers write flash */
  config.max_uri_handlers = 40;
  config.lru_purge_enable = true;

  httpd_handle_t server = NULL;
  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed (%s)", esp_err_to_name(err));
    return err;
  }
  s_server = server;

  web_role_register(server);
  for (size_t i = 0; i < sizeof(SHARED_HANDLERS) / sizeof(SHARED_HANDLERS[0]);
       i++) {
    err = httpd_register_uri_handler(server, &SHARED_HANDLERS[i]);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Registering %s failed (%s)", SHARED_HANDLERS[i].uri,
               esp_err_to_name(err));
    }
  }
  err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Registering the 404 handler failed (%s)",
             esp_err_to_name(err));
  }
  err = log_stream_register(server);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "log_stream_register failed (%s)", esp_err_to_name(err));
  }
  ESP_LOGI(TAG, "Web UI started on port %u", (unsigned)port);
  return ESP_OK;
}
