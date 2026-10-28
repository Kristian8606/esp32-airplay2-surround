#include "web_server.h"
#include "audio_receiver.h"
#if !(defined(CONFIG_AIRPLAY_ROLE_GATEWAY) && CONFIG_AIRPLAY_ROLE_GATEWAY)
#include "aac_decoder.h"
#include "audio_eq.h"
#include "audio_loudness.h"
#include "spk_link.h"
#endif
#include "ota.h"
#if defined(CONFIG_AIRPLAY_ROLE_GATEWAY) && CONFIG_AIRPLAY_ROLE_GATEWAY
#include "gw_speakers.h"
#endif
#include "wifi.h"
#include "settings.h"
#include "log_stream.h"
#include "rtsp_server.h"
#include "rtsp_remote.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "airplay_log.h"

/* The Gateway build has no decoder, EQ or I2S output: its pages and APIs for
 * those (EQ, loudness, speaker, output latency, output mute) are not built. */
#if defined(CONFIG_AIRPLAY_ROLE_GATEWAY) && CONFIG_AIRPLAY_ROLE_GATEWAY
#define WEB_AUDIO_PAGES 0
#else
#define WEB_AUDIO_PAGES 1
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
#define WEB_PAGE_CHUNK 1024
#define SPEEDTEST_CHUNK 2048
#define SPEEDTEST_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define HTTP_SERVER_TASK_PRIORITY 3
#define HTTP_BODY_IDLE_TIMEOUT_US (15LL * 1000LL * 1000LL)

static void log_wifi_scan_memory(const char *where) {
  AP_LOGI(TAG,
           "WiFi scan MEM %s internal=%uKiB largest=%uKiB psram=%uKiB largest=%uKiB",
           where,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) /
                      1024U),
           (unsigned)(heap_caps_get_largest_free_block(
                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) /
                      1024U),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) /
                      1024U));
}

/* Before the audio engine is released: a Surround speaker drops its link to
 * the Gateway first, so no forwarded call reaches the released engine. */
static void suspend_surround_link(void) {
#if WEB_AUDIO_PAGES
  if (settings_get_surround_mode()) spk_link_suspend();
#endif
}

static esp_err_t restore_airplay_after_wifi_scan(void) {
  esp_err_t last_err = ESP_FAIL;

  for (int attempt = 1; attempt <= 2; ++attempt) {
    last_err = audio_receiver_init();
#if WEB_AUDIO_PAGES
    /* A speaker in Surround mode runs no RTSP: its link reconnects. */
    if (last_err == ESP_OK && settings_get_surround_mode()) {
      spk_link_resume();
      return ESP_OK;
    }
#endif
    if (last_err == ESP_OK) {
      last_err = rtsp_server_start();
      if (last_err == ESP_OK) {
        if (attempt > 1) {
          AP_LOGI(TAG, "WiFi scan: AirPlay restore succeeded on retry");
        }
        return ESP_OK;
      }
      ESP_LOGE(TAG, "WiFi scan: RTSP restore attempt %d failed: %s",
               attempt, esp_err_to_name(last_err));
      /* A failed server start should not leave a partially created listener
       * around when we retry. audio_receiver_init() itself is idempotent. */
      rtsp_server_stop();
    } else {
      ESP_LOGE(TAG, "WiFi scan: audio restore attempt %d failed: %s",
               attempt, esp_err_to_name(last_err));
    }

    if (attempt == 1) {
      log_wifi_scan_memory("restore-retry");
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  return last_err;
}

static void reboot_after_wifi_scan_restore_failure(void) {
  ESP_LOGE(TAG, "WiFi scan: AirPlay restore failed twice; rebooting to recover");
  vTaskDelay(pdMS_TO_TICKS(250));
  esp_restart();
}

/* Linker-backed read-only HTML in flash, included in the firmware OTA slot. */
extern const uint8_t web_index_start[] asm("_binary_web_index_html_start");
extern const uint8_t web_index_end[] asm("_binary_web_index_html_end");
extern const uint8_t web_logs_start[] asm("_binary_web_logs_html_start");
extern const uint8_t web_logs_end[] asm("_binary_web_logs_html_end");
extern const uint8_t web_speedtest_start[] asm("_binary_web_speedtest_html_start");
extern const uint8_t web_speedtest_end[] asm("_binary_web_speedtest_html_end");
extern const uint8_t web_eq_start[] asm("_binary_web_eq_html_start");
extern const uint8_t web_eq_end[] asm("_binary_web_eq_html_end");
#if !WEB_AUDIO_PAGES
extern const uint8_t web_speakers_start[] asm("_binary_web_speakers_html_start");
extern const uint8_t web_speakers_end[] asm("_binary_web_speakers_html_end");
#endif

static esp_err_t serve_embedded_page(httpd_req_t *req, const uint8_t *start,
                                      const uint8_t *end) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  /* A reload after OTA must load the UI matching the running firmware. */
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  size_t remaining = (size_t)(end - start);
  while (remaining > 0U) {
    const size_t chunk = remaining < WEB_PAGE_CHUNK ? remaining : WEB_PAGE_CHUNK;
    /* No page-sized RAM allocation or filesystem buffer. */
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)start, chunk);
    if (err != ESP_OK) return err;
    start += chunk;
    remaining -= chunk;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}
static esp_err_t root_handler(httpd_req_t *req) {
  return serve_embedded_page(req, web_index_start, web_index_end);
}
static esp_err_t logs_handler(httpd_req_t *req) {
  return serve_embedded_page(req, web_logs_start, web_logs_end);
}
static esp_err_t speedtest_handler(httpd_req_t *req) {
  return serve_embedded_page(req, web_speedtest_start, web_speedtest_end);
}
#if WEB_AUDIO_PAGES
static esp_err_t eq_page_handler(httpd_req_t *req) {
  return serve_embedded_page(req, web_eq_start, web_eq_end);
}
#else
static esp_err_t speakers_page_handler(httpd_req_t *req) {
  return serve_embedded_page(req, web_speakers_start, web_speakers_end);
}
#endif
static esp_err_t favicon_handler(httpd_req_t *req){ httpd_resp_set_status(req,"204 No Content"); return httpd_resp_send(req,NULL,0); }
static esp_err_t captive_redirect(httpd_req_t *req){ httpd_resp_set_status(req,"302 Found"); httpd_resp_set_hdr(req,"Location","http://192.168.4.1/"); return httpd_resp_send(req,NULL,0); }
static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t error){
  (void)error;
  /* Unknown probe URLs should enter the setup page only while STA is not
   * connected. In normal STA mode keep ordinary 404 behavior. */
  if (!wifi_is_connected()) return captive_redirect(req);
  httpd_resp_set_status(req,"404 Not Found");
  httpd_resp_set_type(req,"text/plain");
  return httpd_resp_sendstr(req,"Not Found");
}

static esp_err_t airplay_still_stopping(httpd_req_t *req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "text/plain");
  (void)httpd_resp_sendstr(req,
                          "AirPlay clients are still stopping; recovering service");
  /* Refusing maintenance must not strand the listener after delayed owners
   * finish. Keep this rare recovery in the HTTP owner: no additional stack or
   * timer callback is needed, and audio memory remains untouched. */
  const int64_t deadline = esp_timer_get_time() + 30000000LL;
  while (!rtsp_server_is_idle() && esp_timer_get_time() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (rtsp_server_is_idle()) {
    const esp_err_t err = rtsp_server_start();
    if (err == ESP_OK) {
      AP_LOGI(TAG, "AirPlay restored after delayed maintenance stop");
      return ESP_FAIL;
    }
    ESP_LOGE(TAG, "AirPlay restart after delayed stop failed: %s",
             esp_err_to_name(err));
  }
  /* A stuck owner cannot safely have its buffers freed or reused. Follow the
   * existing maintenance recovery policy and reboot with resources intact. */
  ESP_LOGE(TAG, "AirPlay delayed stop recovery failed; rebooting");
  vTaskDelay(pdMS_TO_TICKS(250));
  esp_restart();
  return ESP_FAIL;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;
  const bool restore_airplay = audio_receiver_is_initialized();

  /* The 6 MiB compressed store is intentionally borrowed by provisioning.
   * A normal connected device stops AirPlay only for the duration of this
   * scan. On first boot the audio engine was never created, so the scan starts
   * with essentially the full PSRAM headroom already available. */
  if (restore_airplay) {
    AP_LOGI(TAG, "WiFi scan: pausing AirPlay and releasing audio memory");
    rtsp_server_stop();
    if (!rtsp_server_is_idle()) {
      return airplay_still_stopping(req);
    }
    suspend_surround_link();
    esp_err_t release_err = audio_receiver_release_for_wifi_scan();
    if (release_err != ESP_OK) {
      ESP_LOGE(TAG, "WiFi scan: audio memory release failed: %s",
               esp_err_to_name(release_err));
      /* A refused release means the scan must not proceed. Restore the normal
       * service before returning; if even the retry cannot recover it, a clean
       * reboot is safer than leaving a half-stopped audio engine advertised. */
      esp_err_t recover_err = restore_airplay_after_wifi_scan();
      if (recover_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "AirPlay recovery failed; rebooting");
        reboot_after_wifi_scan_restore_failure();
        return ESP_FAIL;
      }
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Audio engine could not pause for WiFi scan");
      return ESP_FAIL;
    }
    log_wifi_scan_memory("after-audio-release");
  } else {
    log_wifi_scan_memory("setup-mode");
  }

  esp_err_t err = wifi_scan(&ap_list, &ap_count);

  /* esp_wifi_scan_get_ap_records() has already released the driver's scan
   * list at this point. Reclaim the large contiguous 6 MiB codec workspace
   * immediately, before cJSON/string allocations have a chance to fragment
   * the freshly available PSRAM. */
  if (restore_airplay) {
    const esp_err_t restore_err = restore_airplay_after_wifi_scan();
    if (restore_err != ESP_OK) {
      free(ap_list);
      ap_list = NULL;
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "AirPlay could not restart after WiFi scan; rebooting");
      reboot_after_wifi_scan_restore_failure();
      return ESP_FAIL;
    }
    AP_LOGI(TAG, "WiFi scan: AirPlay ready again");
    log_wifi_scan_memory("after-audio-restore");
  }

  cJSON *json = cJSON_CreateObject();
  if (!json) {
    free(ap_list);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to encode WiFi scan results");
    return ESP_FAIL;
  }

  if (err == ESP_OK) {
    cJSON *networks = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_count; i++) {
      cJSON *net = cJSON_CreateObject();
      cJSON_AddStringToObject(net, "ssid", (char *)ap_list[i].ssid);
      cJSON_AddNumberToObject(net, "rssi", ap_list[i].rssi);
      cJSON_AddNumberToObject(net, "channel", ap_list[i].primary);
      cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddItemToObject(json, "networks", networks);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
  }

  char *json_str = cJSON_PrintUnformatted(json);
  free(ap_list);
  ap_list = NULL;
  cJSON_Delete(json);
  log_wifi_scan_memory("after-scan-results-free");

  if (!json_str) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to encode WiFi scan results");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  esp_err_t send_err = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);

  return send_err;
}

static esp_err_t recv_json(httpd_req_t *req, char *buf, size_t cap){
  if (req->content_len <= 0 || req->content_len >= cap) {
    return ESP_ERR_INVALID_SIZE;
  }

  size_t got = 0;
  int64_t idle_deadline = esp_timer_get_time() + HTTP_BODY_IDLE_TIMEOUT_US;
  while (got < (size_t)req->content_len) {
    int n = httpd_req_recv(req, buf + got, req->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      if (esp_timer_get_time() >= idle_deadline) return ESP_ERR_TIMEOUT;
      continue;
    }
    if (n <= 0) return ESP_FAIL;
    got += (size_t)n;
    idle_deadline = esp_timer_get_time() + HTTP_BODY_IDLE_TIMEOUT_US;
  }
  buf[got] = 0;
  return ESP_OK;
}
/* Optional admin password (menuconfig AIRPLAY_WEB_ADMIN_PASSWORD).
 * Empty = no check (default, unchanged behaviour). When set, firmware update,
 * restart, Wi-Fi/name changes and the latency test require HTTP Basic auth
 * (user "admin"); the browser shows its own login prompt once. */
#ifndef CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD
#define CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD ""
#endif
static bool admin_ok(httpd_req_t *req) {
  const char *pw = CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD;
  if (!pw[0]) return true;
  char hdr[160];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
      strncmp(hdr, "Basic ", 6) == 0) {
    unsigned char dec[112]; size_t dl = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dl, (const unsigned char *)hdr + 6,
                              strlen(hdr + 6)) == 0) {
      dec[dl] = 0;
      const char *colon = strchr((const char *)dec, ':');
      if (colon && strncmp((const char *)dec, "admin", (size_t)(colon - (const char *)dec)) == 0 &&
          (colon - (const char *)dec) == 5 && strcmp(colon + 1, pw) == 0)
        return true;
    }
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AirPlay ESP32\"");
  httpd_resp_sendstr(req, "Authentication required\n");
  return false;
}

static esp_err_t wifi_config_handler(httpd_req_t *req){
  if(!admin_ok(req)) return ESP_OK;
  char b[512]; if(recv_json(req,b,sizeof(b))!=ESP_OK){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid body");return ESP_FAIL;} cJSON *j=cJSON_Parse(b); cJSON *s=j?cJSON_GetObjectItem(j,"ssid"):NULL; cJSON *p=j?cJSON_GetObjectItem(j,"password"):NULL; cJSON *r=cJSON_CreateObject();
  if(s&&cJSON_IsString(s)){ esp_err_t e=settings_set_wifi_credentials(s->valuestring,(p&&cJSON_IsString(p))?p->valuestring:""); cJSON_AddBoolToObject(r,"success",e==ESP_OK); if(e!=ESP_OK)cJSON_AddStringToObject(r,"error",esp_err_to_name(e)); }
  else { cJSON_AddBoolToObject(r,"success",false); cJSON_AddStringToObject(r,"error","Invalid SSID"); }
  /* Restart ONLY after the new credentials were really saved. */
  cJSON *ok_item=cJSON_GetObjectItem(r,"success"); const bool saved=ok_item&&cJSON_IsTrue(ok_item);
  char *out=cJSON_PrintUnformatted(r); httpd_resp_set_type(req,"application/json"); httpd_resp_sendstr(req,out); free(out); cJSON_Delete(r); if(j)cJSON_Delete(j);
  if(saved){ vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
  return ESP_OK;
}
/* ---- Output latency (manual value + optional wired measurement) ---- */
#ifndef CONFIG_AIRPLAY_OUTPUT_LATENCY_US
#define CONFIG_AIRPLAY_OUTPUT_LATENCY_US 0
#endif
static void send_json_obj(httpd_req_t *req, cJSON *r) {
  char *out = cJSON_PrintUnformatted(r);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, out ? out : "{}");
  free(out);
  cJSON_Delete(r);
}
#if !WEB_AUDIO_PAGES
/* ---- Gateway: the Surround speakers and their own settings ---- */
static esp_err_t speakers_get_handler(httpd_req_t *req) {
  static gw_speaker_info_t list[SURROUND_MAX_SPEAKERS]; /* httpd task only */
  const size_t n = gw_speakers_list(list);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", true);
  cJSON *arr = cJSON_AddArrayToObject(r, "speakers");
  for (size_t i = 0; arr && i < n; ++i) {
    const gw_speaker_info_t *sp = &list[i];
    cJSON *o = cJSON_CreateObject();
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", sp->mac[0],
             sp->mac[1], sp->mac[2], sp->mac[3], sp->mac[4], sp->mac[5]);
    cJSON_AddStringToObject(o, "name", sp->name);
    cJSON_AddStringToObject(o, "ip", sp->ip);
    cJSON_AddStringToObject(o, "mac", mac);
    cJSON_AddStringToObject(o, "firmware", sp->firmware);
    cJSON_AddStringToObject(o, "state", sp->state);
    cJSON_AddBoolToObject(o, "audio", sp->audio);
    if (sp->config_valid) {
      cJSON_AddNumberToObject(o, "channel", sp->config.channel);
      cJSON_AddNumberToObject(o, "trim_db", sp->config.trim_db10 / 10.0);
      cJSON_AddNumberToObject(o, "delay_ms", sp->config.delay_us / 1000.0);
      cJSON_AddBoolToObject(o, "multichannel", sp->config.multichannel != 0U);
    }
    if (sp->status_valid) {
      const surround_status_t *st = &sp->status;
      cJSON *pb = cJSON_AddObjectToObject(o, "playback");
      cJSON_AddBoolToObject(pb, "active", (st->flags & SURROUND_STATUS_ACTIVE) != 0U);
      cJSON_AddBoolToObject(pb, "playing", (st->flags & SURROUND_STATUS_PLAYING) != 0U);
      cJSON_AddBoolToObject(pb, "time", (st->flags & SURROUND_STATUS_TIME) != 0U);
      cJSON_AddStringToObject(pb, "stream",
                              st->stream_type == SURROUND_STREAM_REALTIME ? "realtime"
                              : st->stream_type == SURROUND_STREAM_BUFFERED ? "buffered"
                                                                            : "none");
      cJSON_AddStringToObject(pb, "codec", st->codec);
      cJSON_AddNumberToObject(pb, "sample_rate", st->sample_rate);
      cJSON_AddNumberToObject(pb, "bits", st->bits);
      cJSON_AddNumberToObject(pb, "channels", st->stream_channels);
      if (st->flags & SURROUND_STATUS_MC) {
        cJSON_AddNumberToObject(pb, "output_channel", st->output_channel);
        cJSON_AddStringToObject(pb, "label", st->label);
      }
      if (st->flags & SURROUND_STATUS_SYNC) {
        cJSON_AddNumberToObject(pb, "sync_ms", st->sync_us / 1000.0);
        cJSON_AddNumberToObject(pb, "i2s_ppm", st->servo_ppm);
      }
      if (st->fifo_capacity_kib) {
        cJSON_AddNumberToObject(pb, "fifo_kib", st->fifo_used_kib);
        cJSON_AddNumberToObject(pb, "fifo_capacity_kib", st->fifo_capacity_kib);
      }
      cJSON_AddNumberToObject(pb, "tsf_rms_us", st->tsf_rms_ns10 / 100.0);
    }
    cJSON_AddItemToArray(arr, o);
  }
  send_json_obj(req, r);
  return ESP_OK;
}
static bool parse_mac(const char *s, uint8_t mac[6]) {
  unsigned v[6];
  if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3],
                   &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)v[i];
  return true;
}
static esp_err_t speakers_post_handler(httpd_req_t *req) {
  if (!admin_ok(req)) return ESP_OK;
  char b[192];
  if (recv_json(req, b, sizeof(b)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *j = cJSON_Parse(b);
  cJSON *mac_j = j ? cJSON_GetObjectItem(j, "mac") : NULL;
  cJSON *ch = j ? cJSON_GetObjectItem(j, "channel") : NULL;
  cJSON *trim = j ? cJSON_GetObjectItem(j, "trim_db") : NULL;
  cJSON *delay = j ? cJSON_GetObjectItem(j, "delay_ms") : NULL;
  surround_set_config_t cfg = {0};
  uint8_t mac[6];
  const char *error = NULL;
  if (!cJSON_IsString(mac_j) || !parse_mac(mac_j->valuestring, mac)) {
    error = "expected \"mac\": \"aa:bb:cc:dd:ee:ff\"";
  }
  if (!error && ch) {
    if (cJSON_IsNumber(ch) && ch->valueint >= -1 && ch->valueint <= 7) {
      cfg.fields |= SURROUND_SET_CHANNEL;
      cfg.channel = (int8_t)ch->valueint;
    } else {
      error = "channel: -1 (mono mix) .. 7";
    }
  }
  if (!error && trim) {
    const long v = cJSON_IsNumber(trim) ? lround(trim->valuedouble * 10.0) : 1;
    if (v >= SURROUND_TRIM_MIN_DB10 && v <= SURROUND_TRIM_MAX_DB10) {
      cfg.fields |= SURROUND_SET_TRIM;
      cfg.trim_db10 = (int16_t)v;
    } else {
      error = "trim_db: -30 .. 0";
    }
  }
  if (!error && delay) {
    const long v = cJSON_IsNumber(delay) ? lround(delay->valuedouble * 1000.0) : -1;
    if (v >= SURROUND_DELAY_MIN_US && v <= SURROUND_DELAY_MAX_US) {
      cfg.fields |= SURROUND_SET_DELAY;
      cfg.delay_us = (int32_t)v;
    } else {
      error = "delay_ms: 0 .. 50";
    }
  }
  if (!error && cfg.fields == 0U) error = "nothing to set";
  esp_err_t e = ESP_ERR_INVALID_ARG;
  if (!error) {
    e = gw_speakers_set_config(mac, &cfg);
    if (e == ESP_ERR_NOT_FOUND) error = "speaker not connected";
    else if (e != ESP_OK) error = esp_err_to_name(e);
  }
  if (j) cJSON_Delete(j);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (error) cJSON_AddStringToObject(r, "error", error);
  send_json_obj(req, r);
  return ESP_OK;
}
#endif
#if WEB_AUDIO_PAGES
static void add_latency_state(cJSON *r) {
  cJSON_AddNumberToObject(r, "latency_us", audio_receiver_get_output_latency_us());
  cJSON_AddNumberToObject(r, "default_us", CONFIG_AIRPLAY_OUTPUT_LATENCY_US);
#if CONFIG_AIRPLAY_LATENCY_CAL
  cJSON_AddBoolToObject(r, "measure_available", true);
  cJSON_AddNumberToObject(r, "measure_gpio", CONFIG_AIRPLAY_LATENCY_CAL_GPIO);
#else
  cJSON_AddBoolToObject(r, "measure_available", false);
#endif
}
static esp_err_t latency_get_handler(httpd_req_t *req) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", true);
  add_latency_state(r);
  send_json_obj(req, r);
  return ESP_OK;
}
static esp_err_t latency_post_handler(httpd_req_t *req) {
  if (!admin_ok(req)) return ESP_OK;
  char b[128];
  if (recv_json(req, b, sizeof(b)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *j = cJSON_Parse(b);
  cJSON *r = cJSON_CreateObject();
  esp_err_t e = ESP_ERR_INVALID_ARG;
  cJSON *reset = j ? cJSON_GetObjectItem(j, "reset") : NULL;
  cJSON *v = j ? cJSON_GetObjectItem(j, "latency_us") : NULL;
  if (reset && cJSON_IsTrue(reset)) {
    e = settings_clear_output_latency();
    if (e == ESP_OK)
      e = audio_receiver_set_output_latency_us(CONFIG_AIRPLAY_OUTPUT_LATENCY_US, false);
  } else if (v && cJSON_IsNumber(v)) {
    e = audio_receiver_set_output_latency_us((int32_t)v->valuedouble, true);
  }
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (e != ESP_OK)
    cJSON_AddStringToObject(r, "error", e == ESP_ERR_INVALID_ARG
                                            ? "value out of range (-100000..150000 us)"
                                            : esp_err_to_name(e));
  add_latency_state(r);
  if (j) cJSON_Delete(j);
  send_json_obj(req, r);
  return ESP_OK;
}
/* Loudness compensation: settings plus the live state (the page polls it
 * while open and draws the curve from the filter parameters given here). */
static void add_loudness_state(cJSON *r) {
  audio_loudness_status_t st;
  audio_loudness_get_status(&st);
  cJSON_AddBoolToObject(r, "enabled", st.enabled);
  cJSON_AddNumberToObject(r, "reference_db", st.reference_db);
  cJSON_AddNumberToObject(r, "reference_max_db", AUDIO_LOUDNESS_REF_MAX_DB);
  if (st.output_silent) cJSON_AddNullToObject(r, "output_db");
  else cJSON_AddNumberToObject(r, "output_db", roundf(st.output_db * 10.0f) / 10.0f);
  cJSON_AddNumberToObject(r, "correction_db", roundf(st.correction_db * 100.0f) / 100.0f);
  cJSON_AddNumberToObject(r, "sample_rate", AUDIO_LOUDNESS_SAMPLE_RATE);
  cJSON *lo = cJSON_AddObjectToObject(r, "low");
  if (lo) {
    cJSON_AddNumberToObject(lo, "hz", AUDIO_LOUDNESS_LOW_HZ);
    cJSON_AddNumberToObject(lo, "slope", AUDIO_LOUDNESS_LOW_SLOPE);
    cJSON_AddNumberToObject(lo, "db_per_db", AUDIO_LOUDNESS_LOW_DB_PER_DB);
  }
  cJSON *hi = cJSON_AddObjectToObject(r, "high");
  if (hi) {
    cJSON_AddNumberToObject(hi, "hz", AUDIO_LOUDNESS_HIGH_HZ);
    cJSON_AddNumberToObject(hi, "slope", AUDIO_LOUDNESS_HIGH_SLOPE);
    cJSON_AddNumberToObject(hi, "db_per_db", AUDIO_LOUDNESS_HIGH_DB_PER_DB);
  }
}
static esp_err_t loudness_get_handler(httpd_req_t *req) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", true);
  add_loudness_state(r);
  send_json_obj(req, r);
  return ESP_OK;
}
static esp_err_t loudness_post_handler(httpd_req_t *req) {
  char b[96];
  if (recv_json(req, b, sizeof(b)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *j = cJSON_Parse(b);
  cJSON *en = j ? cJSON_GetObjectItem(j, "enabled") : NULL;
  cJSON *ref = j ? cJSON_GetObjectItem(j, "reference_db") : NULL;
  esp_err_t e = ESP_ERR_INVALID_ARG;
  if (cJSON_IsBool(en) && cJSON_IsNumber(ref)) {
    e = audio_loudness_set(cJSON_IsTrue(en), (int32_t)lround(ref->valuedouble), true);
  }
  if (j) cJSON_Delete(j);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (e != ESP_OK)
    cJSON_AddStringToObject(r, "error", e == ESP_ERR_INVALID_ARG
                                            ? "reference out of range (0..20 dB)"
                                            : esp_err_to_name(e));
  add_loudness_state(r);
  send_json_obj(req, r);
  return ESP_OK;
}
/* Multichannel experiment: which 5.1/7.1 speaker this receiver plays.
 * {"available", "speaker", "speakers":[{"id","name"}], "active",
 *  "channel", "channels", "label"} */
static void add_speaker_state(cJSON *r) {
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
  cJSON_AddBoolToObject(r, "available", true);
#else
  cJSON_AddBoolToObject(r, "available", false);
#endif
  cJSON_AddNumberToObject(r, "speaker", aac_decoder_get_speaker());
  cJSON *list = cJSON_AddArrayToObject(r, "speakers");
  for (int s = AAC_SPEAKER_MIX; s < AAC_SPEAKER_COUNT; ++s) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", s);
    cJSON_AddStringToObject(o, "name", aac_speaker_name(s));
    cJSON_AddItemToArray(list, o);
  }
  int channel = -1, channels = 0;
  char label[16];
  const bool active =
      aac_decoder_get_mc_status(&channel, &channels, label, sizeof(label));
  cJSON_AddBoolToObject(r, "active", active);
  if (active) {
    cJSON_AddNumberToObject(r, "channel", channel);
    cJSON_AddNumberToObject(r, "channels", channels);
    cJSON_AddStringToObject(r, "label", label);
  }
}
static esp_err_t speaker_get_handler(httpd_req_t *req) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", true);
  add_speaker_state(r);
  send_json_obj(req, r);
  return ESP_OK;
}
static esp_err_t speaker_post_handler(httpd_req_t *req) {
  char b[64];
  if (recv_json(req, b, sizeof(b)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *j = cJSON_Parse(b);
  cJSON *sp = j ? cJSON_GetObjectItem(j, "speaker") : NULL;
  esp_err_t e = ESP_ERR_INVALID_ARG;
  if (cJSON_IsNumber(sp) && sp->valueint >= AAC_SPEAKER_MIX &&
      sp->valueint < AAC_SPEAKER_COUNT) {
    aac_decoder_set_speaker(sp->valueint);
    e = settings_set_mc_speaker(sp->valueint);
    if (settings_get_surround_mode()) spk_link_config_changed();
  }
  if (j) cJSON_Delete(j);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (e != ESP_OK)
    cJSON_AddStringToObject(r, "error", e == ESP_ERR_INVALID_ARG
                                            ? "speaker out of range"
                                            : esp_err_to_name(e));
  add_speaker_state(r);
  send_json_obj(req, r);
  return ESP_OK;
}
/* Speaker: Surround mode on/off. {"enabled", "gateway_host"}; a change is
 * saved and the speaker restarts into the new mode. */
static esp_err_t surround_get_handler(httpd_req_t *req) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", true);
  cJSON_AddBoolToObject(r, "enabled", settings_get_surround_mode());
  cJSON_AddStringToObject(r, "gateway_host", CONFIG_AIRPLAY_SURROUND_GATEWAY_HOST);
  send_json_obj(req, r);
  return ESP_OK;
}
static esp_err_t surround_post_handler(httpd_req_t *req) {
  if (!admin_ok(req)) return ESP_OK;
  char b[64];
  if (recv_json(req, b, sizeof(b)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *j = cJSON_Parse(b);
  cJSON *en = j ? cJSON_GetObjectItem(j, "enabled") : NULL;
  esp_err_t e = ESP_ERR_INVALID_ARG;
  bool restart = false;
  if (cJSON_IsBool(en)) {
    const bool want = cJSON_IsTrue(en);
    restart = want != settings_get_surround_mode();
    e = restart ? settings_set_surround_mode(want) : ESP_OK;
    restart = restart && e == ESP_OK;
  }
  if (j) cJSON_Delete(j);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (e != ESP_OK)
    cJSON_AddStringToObject(r, "error", e == ESP_ERR_INVALID_ARG
                                            ? "expected {\"enabled\": true|false}"
                                            : esp_err_to_name(e));
  cJSON_AddBoolToObject(r, "enabled", settings_get_surround_mode());
  cJSON_AddBoolToObject(r, "restarting", restart);
  send_json_obj(req, r);
  if (restart) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  }
  return ESP_OK;
}
static esp_err_t latency_measure_handler(httpd_req_t *req) {
  if (!admin_ok(req)) return ESP_OK;
  /* optional body {"audible":false}; default: switch the amplifier on */
  bool audible = true;
  if (req->content_len > 0 && req->content_len < 64) {
    char b[64];
    if (recv_json(req, b, sizeof(b)) == ESP_OK) {
      cJSON *j = cJSON_Parse(b);
      cJSON *a = j ? cJSON_GetObjectItem(j, "audible") : NULL;
      if (a && cJSON_IsBool(a)) audible = cJSON_IsTrue(a);
      if (j) cJSON_Delete(j);
    }
  }
  /* Same lifecycle as the Wi-Fi scan: stop AirPlay, release its
   * memory, measure, restore AirPlay. */
  const bool restore_airplay = audio_receiver_is_initialized();
  if (restore_airplay) {
    AP_LOGI(TAG, "Latency test: pausing AirPlay and releasing audio memory");
    rtsp_server_stop();
    if (!rtsp_server_is_idle()) {
      return airplay_still_stopping(req);
    }
    suspend_surround_link();
    const esp_err_t rel = audio_receiver_release_for_wifi_scan();
    if (rel != ESP_OK) {
      ESP_LOGE(TAG, "Latency test: audio release failed: %s", esp_err_to_name(rel));
      if (restore_airplay_after_wifi_scan() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "AirPlay recovery failed; rebooting");
        reboot_after_wifi_scan_restore_failure();
        return ESP_FAIL;
      }
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Audio engine could not pause for the latency test");
      return ESP_FAIL;
    }
    log_wifi_scan_memory("latency-test-released");
  }
  latency_cal_result_t res;
  esp_err_t e = audio_receiver_measure_output_latency(&res, audible);
  if (restore_airplay) {
    if (restore_airplay_after_wifi_scan() != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "AirPlay could not restart after the latency test; rebooting");
      reboot_after_wifi_scan_restore_failure();
      return ESP_FAIL;
    }
    AP_LOGI(TAG, "Latency test: AirPlay ready again");
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK && res.ok);
  if (e == ESP_OK && res.ok) {
    /* A successful measurement becomes the saved value. */
    esp_err_t se = audio_receiver_set_output_latency_us(res.latency_us, true);
    cJSON_AddBoolToObject(r, "saved", se == ESP_OK);
  }
  cJSON_AddNumberToObject(r, "measured_us", res.latency_us);
  cJSON_AddNumberToObject(r, "spread_us", res.spread_us);
  cJSON_AddNumberToObject(r, "valid_bursts", res.valid_bursts);
  cJSON_AddNumberToObject(r, "bursts_total", LATENCY_CAL_BURSTS);
  cJSON_AddNumberToObject(r, "correlation", res.corr);
  cJSON_AddNumberToObject(r, "amplitude", res.amplitude);
  cJSON_AddBoolToObject(r, "inverted", res.inverted);
  if (!(e == ESP_OK && res.ok))
    cJSON_AddStringToObject(r, "error", res.error ? res.error : esp_err_to_name(e));
  add_latency_state(r);
  send_json_obj(req, r);
  return ESP_OK;
}
#endif

static esp_err_t device_name_handler(httpd_req_t *req){
  if(!admin_ok(req)) return ESP_OK;
  char b[256]; if(recv_json(req,b,sizeof(b))!=ESP_OK){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid body");return ESP_FAIL;} cJSON *j=cJSON_Parse(b); cJSON *n=j?cJSON_GetObjectItem(j,"name"):NULL; cJSON *r=cJSON_CreateObject();
  if(n&&cJSON_IsString(n)){ esp_err_t e=settings_set_device_name(n->valuestring); if(e==ESP_OK)wifi_set_hostname(n->valuestring); cJSON_AddBoolToObject(r,"success",e==ESP_OK); if(e!=ESP_OK)cJSON_AddStringToObject(r,"error",esp_err_to_name(e)); } else {cJSON_AddBoolToObject(r,"success",false);cJSON_AddStringToObject(r,"error","Invalid name");}
  char *out=cJSON_PrintUnformatted(r); httpd_resp_set_type(req,"application/json"); httpd_resp_sendstr(req,out); free(out); cJSON_Delete(r); if(j)cJSON_Delete(j); return ESP_OK;
}

#if WEB_AUDIO_PAGES
static void eq_output_to_json(const audio_eq_output_config_t *output,
                              cJSON *array) {
  for (uint8_t i = 0; i < output->filter_count; ++i) {
    const audio_eq_filter_config_t *f = &output->filters[i];
    cJSON *item = cJSON_CreateObject();
    cJSON_AddBoolToObject(item, "enabled", f->enabled != 0);
    cJSON_AddStringToObject(
        item, "type",
        audio_eq_filter_type_name((audio_eq_filter_type_t)f->type));
    cJSON_AddNumberToObject(item, "frequency_hz", f->frequency_hz);
    cJSON_AddNumberToObject(item, "gain_db", f->gain_db);
    cJSON_AddNumberToObject(item, "q", f->q);
    cJSON_AddItemToArray(array, item);
  }
}

static void eq_config_to_json(const audio_eq_config_t *cfg, cJSON *root) {
  cJSON_AddBoolToObject(root, "enabled", cfg->enabled != 0);
  cJSON_AddStringToObject(
      root, "channel_mode",
      audio_eq_channel_mode_name((audio_eq_channel_mode_t)cfg->channel_mode));
  cJSON_AddNumberToObject(root, "left_preamp_db", cfg->left_preamp_db);
  cJSON_AddNumberToObject(root, "right_preamp_db", cfg->right_preamp_db);
  cJSON *left = cJSON_AddArrayToObject(root, "left_filters");
  cJSON *right = cJSON_AddArrayToObject(root, "right_filters");
  eq_output_to_json(&cfg->left, left);
  eq_output_to_json(&cfg->right, right);
}
#endif

static void remote_to_json(cJSON *root, const rtsp_remote_status_t *status) {
  cJSON_AddBoolToObject(root, "connected", status->connected);
  cJSON_AddNumberToObject(root, "id", status->id);
  cJSON_AddNumberToObject(root, "command", status->command);
  cJSON_AddStringToObject(root, "result", rtsp_remote_result_name(status->result));
  cJSON_AddNumberToObject(root, "rtsp_status", status->response_code);
}

static esp_err_t remote_get_handler(httpd_req_t *req) {
  rtsp_remote_status_t status;
  rtsp_remote_get_status(&status);
  cJSON *root = cJSON_CreateObject();
  if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  remote_to_json(root, &status);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  send_json_obj(req, root);
  return ESP_OK;
}

static esp_err_t remote_post_handler(httpd_req_t *req) {
  char body[96];
  if (recv_json(req, body, sizeof(body)) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command");
  cJSON *root = cJSON_Parse(body);
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "command");
  static const char *const names[] = {"play", "pause", "toggle", "stop", "next", "previous", "volume_down", "volume_up"};
  int command = -1;
  if (cJSON_IsObject(root) && cJSON_IsString(value)) {
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      if (strcmp(value->valuestring, names[i]) == 0) command = (int)i;
  }
  cJSON_Delete(root);
  if (command < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
  root = cJSON_CreateObject();
  if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  uint32_t id;
  esp_err_t err = rtsp_remote_enqueue((uint8_t)command, &id);
  if (err != ESP_OK) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, err == ESP_ERR_INVALID_STATE ?
        "A command is already pending" : "No AirPlay event connection. Start AirPlay from your phone.");
  }
  cJSON_AddNumberToObject(root, "id", id);
  cJSON_AddStringToObject(root, "result", "queued");
  httpd_resp_set_status(req, "202 Accepted");
  send_json_obj(req, root);
  return ESP_OK;
}

#if WEB_AUDIO_PAGES
static void output_mute_to_json(cJSON *root) {
  const uint32_t mask = audio_receiver_get_output_mute_mask();
  cJSON_AddBoolToObject(root, "left_muted", (mask & 1U) != 0U);
  cJSON_AddBoolToObject(root, "right_muted", (mask & 2U) != 0U);
}

static esp_err_t output_mute_post_handler(httpd_req_t *req) {
  char body[128];
  if (recv_json(req, body, sizeof(body)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mute body");
    return ESP_ERR_INVALID_ARG;
  }
  cJSON *root = cJSON_Parse(body);
  const cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
  const cJSON *muted = cJSON_GetObjectItemCaseSensitive(root, "muted");
  const bool valid = cJSON_IsObject(root) && cJSON_IsString(channel) &&
      cJSON_IsBool(muted) &&
      (strcmp(channel->valuestring, "left") == 0 ||
       strcmp(channel->valuestring, "right") == 0);
  if (!valid) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mute settings");
    return ESP_ERR_INVALID_ARG;
  }
  const uint32_t ch = strcmp(channel->valuestring, "left") == 0 ? 0U : 1U;
  const bool mute = cJSON_IsTrue(muted);
  cJSON_Delete(root);
  root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_ERR_NO_MEM;
  }
  audio_receiver_set_output_muted(ch, mute);
  cJSON_AddBoolToObject(root, "success", true);
  output_mute_to_json(root);
  send_json_obj(req, root);
  return ESP_OK;
}

static esp_err_t eq_get_handler(httpd_req_t *req) {
  audio_eq_config_t cfg;
  esp_err_t err = audio_eq_get_active_config(&cfg);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(err));
    return err;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "success", true);
  eq_config_to_json(&cfg, root);
  output_mute_to_json(root);
  char *out = cJSON_PrintUnformatted(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t send_err = httpd_resp_sendstr(req, out ? out : "{}");
  free(out);
  cJSON_Delete(root);
  return send_err;
}

static bool json_number(const cJSON *obj, const char *name, float *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, name);
  if (!cJSON_IsNumber(v) || !out) return false;
  *out = (float)v->valuedouble;
  return true;
}

static bool parse_eq_output(cJSON *array, audio_eq_output_config_t *output) {
  if (!cJSON_IsArray(array) || !output) return false;
  const int count = cJSON_GetArraySize(array);
  if (count < 0 || count > (int)AUDIO_EQ_MAX_FILTERS_PER_CHANNEL) return false;
  output->filter_count = (uint8_t)count;

  for (int i = 0; i < count; ++i) {
    cJSON *item = cJSON_GetArrayItem(array, i);
    if (!cJSON_IsObject(item)) return false;
    cJSON *f_enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    audio_eq_filter_type_t parsed_type;
    audio_eq_filter_config_t *f = &output->filters[i];
    if (!cJSON_IsBool(f_enabled) || !cJSON_IsString(type) ||
        !audio_eq_filter_type_from_name(type->valuestring, &parsed_type) ||
        !json_number(item, "frequency_hz", &f->frequency_hz) ||
        !json_number(item, "gain_db", &f->gain_db) ||
        !json_number(item, "q", &f->q)) {
      return false;
    }
    f->enabled = cJSON_IsTrue(f_enabled) ? 1U : 0U;
    f->type = (uint8_t)parsed_type;
  }
  return true;
}

static esp_err_t eq_post_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 16384) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid EQ body");
    return ESP_FAIL;
  }

  char *body = malloc((size_t)req->content_len + 1U);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_ERR_NO_MEM;
  }
  esp_err_t recv_err = recv_json(req, body, (size_t)req->content_len + 1U);
  if (recv_err != ESP_OK) {
    free(body);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid EQ body");
    return recv_err;
  }

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  audio_eq_config_t cfg;
  audio_eq_default_config(&cfg);
  cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
  cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "channel_mode");
  cJSON *left = cJSON_GetObjectItemCaseSensitive(root, "left_filters");
  cJSON *right = cJSON_GetObjectItemCaseSensitive(root, "right_filters");
  audio_eq_channel_mode_t parsed_mode;

  bool valid = cJSON_IsBool(enabled) && cJSON_IsString(mode) &&
               audio_eq_channel_mode_from_name(mode->valuestring, &parsed_mode) &&
               json_number(root, "left_preamp_db", &cfg.left_preamp_db) &&
               json_number(root, "right_preamp_db", &cfg.right_preamp_db) &&
               parse_eq_output(left, &cfg.left) &&
               parse_eq_output(right, &cfg.right);

  if (valid) {
    cfg.enabled = cJSON_IsTrue(enabled) ? 1U : 0U;
    cfg.channel_mode = (uint8_t)parsed_mode;
    valid = audio_eq_validate_config(&cfg);
  }
  cJSON_Delete(root);

  if (!valid) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid EQ settings");
    return ESP_ERR_INVALID_ARG;
  }

  char action[12] = "both";
  char query[48];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    (void)httpd_query_key_value(query, "action", action, sizeof(action));
  }

  const bool do_apply = strcmp(action, "save") != 0;
  const bool do_save = strcmp(action, "apply") != 0;
  esp_err_t err = ESP_OK;
  if (do_apply) err = audio_eq_apply_config(&cfg);
  if (err == ESP_OK && do_save) err = audio_eq_save_config(&cfg);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(err));
    return err;
  }

  AP_LOGI(TAG, "EQ %s%s", do_apply ? "applied" : "",
           do_save ? (do_apply ? " + saved" : "saved") : "");
  httpd_resp_set_type(req, "application/json");
  char response[96];
  snprintf(response, sizeof(response),
           "{\"success\":true,\"applied\":%s,\"saved\":%s,\"restarting\":false}",
           do_apply ? "true" : "false", do_save ? "true" : "false");
  return httpd_resp_sendstr(req, response);
}
#endif

static esp_err_t ota_handler(httpd_req_t *req){ if(!admin_ok(req)) return ESP_OK; if(req->content_len==0){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"No firmware uploaded");return ESP_FAIL;} AP_LOGI(TAG,"Stopping RTSP for OTA"); rtsp_server_stop(); if(!rtsp_server_is_idle()) return airplay_still_stopping(req); esp_err_t e=ota_start_from_http(req); if(e!=ESP_OK){ ESP_LOGE(TAG,"OTA failed (%s); restarting RTSP",esp_err_to_name(e)); esp_err_t re=rtsp_server_start(); if(re!=ESP_OK){ESP_LOGE(TAG,"RTSP restart after OTA failure failed: %s",esp_err_to_name(re));} httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,esp_err_to_name(e));return e;} httpd_resp_sendstr(req,"Firmware update complete, rebooting now!\n"); vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); return ESP_OK; }
static const char *reset_reason_str(esp_reset_reason_t r){switch(r){case ESP_RST_POWERON:return"poweron";case ESP_RST_EXT:return"external";case ESP_RST_SW:return"software";case ESP_RST_PANIC:return"panic";case ESP_RST_INT_WDT:return"int_wdt";case ESP_RST_TASK_WDT:return"task_wdt";case ESP_RST_WDT:return"other_wdt";case ESP_RST_DEEPSLEEP:return"deepsleep";case ESP_RST_BROWNOUT:return"brownout";case ESP_RST_SDIO:return"sdio";default:return"unknown";}}
static esp_err_t system_info_handler(httpd_req_t *req){
  cJSON *root=cJSON_CreateObject(),*i=cJSON_CreateObject(); char ip[16]={0},mac[18]={0},name[65]={0}; bool connected=wifi_is_connected(); wifi_get_ip_str(ip,sizeof(ip)); wifi_get_mac_str(mac,sizeof(mac)); settings_get_device_name(name,sizeof(name)); cJSON_AddStringToObject(i,"ip",ip);cJSON_AddStringToObject(i,"mac",mac);cJSON_AddStringToObject(i,"device_name",name);cJSON_AddBoolToObject(i,"wifi_connected",connected);cJSON_AddStringToObject(i,"role",!WEB_AUDIO_PAGES?"gateway":settings_get_surround_mode()?"surround-speaker":"speaker");cJSON_AddNumberToObject(i,"free_heap",esp_get_free_heap_size());
  if(connected){wifi_ap_record_t ap;if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){char ssid[33]={0},bssid[18];memcpy(ssid,ap.ssid,32);snprintf(bssid,sizeof(bssid),"%02x:%02x:%02x:%02x:%02x:%02x",ap.bssid[0],ap.bssid[1],ap.bssid[2],ap.bssid[3],ap.bssid[4],ap.bssid[5]);const char *phy=ap.phy_11n?"11n":ap.phy_11g?"11g":ap.phy_11b?"11b":ap.phy_lr?"LR":"?";cJSON_AddStringToObject(i,"wifi_ssid",ssid);cJSON_AddStringToObject(i,"wifi_bssid",bssid);cJSON_AddNumberToObject(i,"wifi_rssi",ap.rssi);cJSON_AddNumberToObject(i,"wifi_channel",ap.primary);cJSON_AddStringToObject(i,"wifi_phy",phy);}}
  const esp_app_desc_t *d=esp_app_get_description();cJSON_AddStringToObject(i,"firmware_version",d->version);cJSON_AddStringToObject(i,"reset_reason",reset_reason_str(esp_reset_reason()));cJSON_AddNumberToObject(i,"uptime_s",(double)(esp_timer_get_time()/1000000));cJSON_AddItemToObject(root,"info",i);cJSON_AddBoolToObject(root,"success",true);char *out=cJSON_PrintUnformatted(root);httpd_resp_set_type(req,"application/json");httpd_resp_sendstr(req,out);free(out);cJSON_Delete(root);return ESP_OK;
}
static esp_err_t restart_handler(httpd_req_t *req){if(!admin_ok(req)) return ESP_OK;httpd_resp_sendstr(req,"Restarting\n");vTaskDelay(pdMS_TO_TICKS(200));esp_restart();return ESP_OK;}
static esp_err_t speed_ping(httpd_req_t *req){httpd_resp_set_type(req,"text/plain");httpd_resp_set_hdr(req,"Cache-Control","no-store");return httpd_resp_send(req,"ok",2);}
static esp_err_t speed_download(httpd_req_t *req){size_t bytes=1024*1024;char q[64],v[16];if(httpd_req_get_url_query_str(req,q,sizeof(q))==ESP_OK&&httpd_query_key_value(q,"bytes",v,sizeof(v))==ESP_OK){long x=strtol(v,NULL,10);if(x>0)bytes=x;}if(bytes>SPEEDTEST_MAX_BYTES)bytes=SPEEDTEST_MAX_BYTES;/* Test pattern lives only for the duration of the request. */ uint8_t *filler=malloc(SPEEDTEST_CHUNK);if(!filler){httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");return ESP_FAIL;}for(size_t i=0;i<SPEEDTEST_CHUNK;i++)filler[i]=(uint8_t)(i*37);httpd_resp_set_type(req,"application/octet-stream");httpd_resp_set_hdr(req,"Cache-Control","no-store");esp_err_t res=ESP_OK;while(bytes){size_t n=bytes<SPEEDTEST_CHUNK?bytes:SPEEDTEST_CHUNK;if(httpd_resp_send_chunk(req,(char*)filler,n)!=ESP_OK){res=ESP_FAIL;break;}bytes-=n;}free(filler);return res==ESP_OK?httpd_resp_send_chunk(req,NULL,0):ESP_FAIL;}
static esp_err_t speed_upload(httpd_req_t *req){
  size_t got=0,total=req->content_len;
  uint8_t b[SPEEDTEST_CHUNK];
  int64_t idle_deadline=esp_timer_get_time()+HTTP_BODY_IDLE_TIMEOUT_US;
  while(got<total){
    size_t want=total-got;if(want>sizeof(b))want=sizeof(b);
    int n=httpd_req_recv(req,(char*)b,want);
    if(n==HTTPD_SOCK_ERR_TIMEOUT){
      if(esp_timer_get_time()>=idle_deadline)return ESP_ERR_TIMEOUT;
      continue;
    }
    if(n<=0)return ESP_FAIL;
    got+=(size_t)n;
    idle_deadline=esp_timer_get_time()+HTTP_BODY_IDLE_TIMEOUT_US;
  }
  char out[64];snprintf(out,sizeof(out),"received=%u",(unsigned)got);
  httpd_resp_set_type(req,"text/plain");return httpd_resp_sendstr(req,out);
}

esp_err_t web_server_start(uint16_t port){ if(s_server)return ESP_OK; httpd_config_t c=HTTPD_DEFAULT_CONFIG();c.server_port=port;c.max_uri_handlers=40;c.stack_size=8192;c.lru_purge_enable=true;c.task_priority=HTTP_SERVER_TASK_PRIORITY;esp_err_t e=httpd_start(&s_server,&c);if(e!=ESP_OK)return e;
#define REG(U,M,H) do{httpd_uri_t x={.uri=U,.method=M,.handler=H};ESP_ERROR_CHECK(httpd_register_uri_handler(s_server,&x));}while(0)
  REG("/",HTTP_GET,root_handler);REG("/favicon.ico",HTTP_GET,favicon_handler);REG("/logs",HTTP_GET,logs_handler);REG("/speedtest",HTTP_GET,speedtest_handler);REG("/api/audio/remote",HTTP_GET,remote_get_handler);REG("/api/audio/remote",HTTP_POST,remote_post_handler);REG("/api/wifi/scan",HTTP_GET,wifi_scan_handler);REG("/api/wifi/config",HTTP_POST,wifi_config_handler);REG("/api/device/name",HTTP_POST,device_name_handler);REG("/api/ota/update",HTTP_POST,ota_handler);REG("/api/system/info",HTTP_GET,system_info_handler);REG("/api/system/restart",HTTP_POST,restart_handler);REG("/api/speedtest/ping",HTTP_GET,speed_ping);REG("/api/speedtest/download",HTTP_GET,speed_download);REG("/api/speedtest/upload",HTTP_POST,speed_upload);REG("/hotspot-detect.html",HTTP_GET,captive_redirect);REG("/library/test/success.html",HTTP_GET,captive_redirect);REG("/generate_204",HTTP_GET,captive_redirect);REG("/connecttest.txt",HTTP_GET,captive_redirect);
#if WEB_AUDIO_PAGES
  REG("/eq",HTTP_GET,eq_page_handler);REG("/api/eq",HTTP_GET,eq_get_handler);REG("/api/eq",HTTP_POST,eq_post_handler);REG("/api/audio/mute",HTTP_POST,output_mute_post_handler);REG("/api/audio/latency",HTTP_GET,latency_get_handler);REG("/api/audio/latency",HTTP_POST,latency_post_handler);REG("/api/audio/latency/measure",HTTP_POST,latency_measure_handler);REG("/api/audio/loudness",HTTP_GET,loudness_get_handler);REG("/api/audio/loudness",HTTP_POST,loudness_post_handler);REG("/api/audio/speaker",HTTP_GET,speaker_get_handler);REG("/api/audio/speaker",HTTP_POST,speaker_post_handler);REG("/api/surround",HTTP_GET,surround_get_handler);REG("/api/surround",HTTP_POST,surround_post_handler);
#else
  REG("/speakers",HTTP_GET,speakers_page_handler);REG("/api/speakers",HTTP_GET,speakers_get_handler);REG("/api/speakers",HTTP_POST,speakers_post_handler);
#endif
  ESP_ERROR_CHECK(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404_handler));
#undef REG
  e=log_stream_register(s_server);if(e!=ESP_OK)ESP_LOGW(TAG,"log stream register failed: %s",esp_err_to_name(e));ESP_LOGI(TAG,"Web UI started on port %u",port);return ESP_OK; }
