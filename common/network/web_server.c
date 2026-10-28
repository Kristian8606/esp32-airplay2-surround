#include "web_server.h"
#include "web_server_internal.h"
#include "audio_receiver.h"
#include "ota.h"
#include "wifi.h"
#include "settings.h"
#include "log_stream.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airplay_log.h"

/* The pages and APIs both firmwares share. Each firmware adds its own
 * (web_role_register()) and says how its audio source stops and starts for
 * maintenance (web_server_internal.h). */

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
#define WEB_PAGE_CHUNK 1024
#define SPEEDTEST_CHUNK 2048
#define SPEEDTEST_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define HTTP_SERVER_TASK_PRIORITY 3
#define HTTP_BODY_IDLE_TIMEOUT_US (15LL * 1000LL * 1000LL)

void web_log_memory(const char *where) {
  AP_LOGI(TAG,
           "MEM %s internal=%uKiB largest=%uKiB psram=%uKiB largest=%uKiB",
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

/* After maintenance: the audio engine again, then its source (the Gateway's
 * RTSP server, a speaker's link to the Gateway). One retry. */
static esp_err_t restore_audio(void) {
  esp_err_t last_err = ESP_FAIL;

  for (int attempt = 1; attempt <= 2; ++attempt) {
    last_err = audio_receiver_init();
    if (last_err == ESP_OK) {
      last_err = web_role_source_start();
      if (last_err == ESP_OK) {
        if (attempt > 1) {
          AP_LOGI(TAG, "Audio restore succeeded on retry");
        }
        return ESP_OK;
      }
      ESP_LOGE(TAG, "Audio source restore attempt %d failed: %s",
               attempt, esp_err_to_name(last_err));
      /* A failed start should not leave a partially created listener
       * around when we retry. audio_receiver_init() itself is idempotent. */
      (void)web_role_source_stop();
    } else {
      ESP_LOGE(TAG, "Audio restore attempt %d failed: %s",
               attempt, esp_err_to_name(last_err));
    }

    if (attempt == 1) {
      web_log_memory("restore-retry");
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  return last_err;
}

static void reboot_after_restore_failure(void) {
  ESP_LOGE(TAG, "Audio restore failed twice; rebooting to recover");
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

esp_err_t web_serve_page(httpd_req_t *req, const uint8_t *start,
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
  return web_serve_page(req, web_index_start, web_index_end);
}
static esp_err_t logs_handler(httpd_req_t *req) {
  return web_serve_page(req, web_logs_start, web_logs_end);
}
static esp_err_t speedtest_handler(httpd_req_t *req) {
  return web_serve_page(req, web_speedtest_start, web_speedtest_end);
}
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

static esp_err_t source_still_stopping(httpd_req_t *req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "text/plain");
  (void)httpd_resp_sendstr(req,
                          "AirPlay clients are still stopping; recovering service");
  /* Refusing maintenance must not strand the listener after delayed owners
   * finish. Keep this rare recovery in the HTTP owner: no additional stack or
   * timer callback is needed, and audio memory remains untouched. */
  const int64_t deadline = esp_timer_get_time() + 30000000LL;
  while (!web_role_source_idle() && esp_timer_get_time() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (web_role_source_idle()) {
    const esp_err_t err = web_role_source_start();
    if (err == ESP_OK) {
      AP_LOGI(TAG, "Audio source restored after delayed maintenance stop");
      return ESP_FAIL;
    }
    ESP_LOGE(TAG, "Audio source restart after delayed stop failed: %s",
             esp_err_to_name(err));
  }
  /* A stuck owner cannot safely have its buffers freed or reused. Follow the
   * existing maintenance recovery policy and reboot with resources intact. */
  ESP_LOGE(TAG, "Audio source delayed stop recovery failed; rebooting");
  vTaskDelay(pdMS_TO_TICKS(250));
  esp_restart();
  return ESP_FAIL;
}

esp_err_t web_audio_pause(httpd_req_t *req, const char *what, bool *paused) {
  *paused = audio_receiver_is_initialized();
  if (!*paused) return ESP_OK;
  AP_LOGI(TAG, "%s: pausing audio and releasing its memory", what);
  (void)web_role_source_stop();
  if (!web_role_source_idle()) {
    (void)source_still_stopping(req);
    return ESP_FAIL;
  }
  const esp_err_t rel = audio_receiver_release_for_wifi_scan();
  if (rel != ESP_OK) {
    ESP_LOGE(TAG, "%s: audio memory release failed: %s", what,
             esp_err_to_name(rel));
    /* A refused release means the maintenance must not proceed. Restore the
     * normal service; if even the retry cannot recover it, a clean reboot is
     * safer than leaving a half-stopped audio engine. */
    if (restore_audio() != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Audio recovery failed; rebooting");
      reboot_after_restore_failure();
      return ESP_FAIL;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Audio engine could not pause");
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t web_audio_resume(httpd_req_t *req, const char *what) {
  if (restore_audio() != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Audio could not restart; rebooting");
    reboot_after_restore_failure();
    return ESP_FAIL;
  }
  AP_LOGI(TAG, "%s: audio ready again", what);
  return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;
  /* The 6 MiB compressed store is intentionally borrowed by provisioning.
   * A normal connected device stops audio only for the duration of this
   * scan. On first boot the audio engine was never created, so the scan starts
   * with essentially the full PSRAM headroom already available. */
  bool restore_audio_after = false;
  if (web_audio_pause(req, "WiFi scan", &restore_audio_after) != ESP_OK) {
    return ESP_FAIL;
  }
  web_log_memory(restore_audio_after ? "after-audio-release" : "setup-mode");

  esp_err_t err = wifi_scan(&ap_list, &ap_count);

  /* esp_wifi_scan_get_ap_records() has already released the driver's scan
   * list at this point. Reclaim the large contiguous 6 MiB codec workspace
   * immediately, before cJSON/string allocations have a chance to fragment
   * the freshly available PSRAM. */
  if (restore_audio_after) {
    if (web_audio_resume(req, "WiFi scan") != ESP_OK) {
      free(ap_list);
      return ESP_FAIL;
    }
    web_log_memory("after-audio-restore");
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
  web_log_memory("after-scan-results-free");

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

esp_err_t web_recv_json(httpd_req_t *req, char *buf, size_t cap){
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
bool web_admin_ok(httpd_req_t *req) {
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
  if(!web_admin_ok(req)) return ESP_OK;
  char b[512]; if(web_recv_json(req,b,sizeof(b))!=ESP_OK){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid body");return ESP_FAIL;} cJSON *j=cJSON_Parse(b); cJSON *s=j?cJSON_GetObjectItem(j,"ssid"):NULL; cJSON *p=j?cJSON_GetObjectItem(j,"password"):NULL; cJSON *r=cJSON_CreateObject();
  if(s&&cJSON_IsString(s)){ esp_err_t e=settings_set_wifi_credentials(s->valuestring,(p&&cJSON_IsString(p))?p->valuestring:""); cJSON_AddBoolToObject(r,"success",e==ESP_OK); if(e!=ESP_OK)cJSON_AddStringToObject(r,"error",esp_err_to_name(e)); }
  else { cJSON_AddBoolToObject(r,"success",false); cJSON_AddStringToObject(r,"error","Invalid SSID"); }
  /* Restart ONLY after the new credentials were really saved. */
  cJSON *ok_item=cJSON_GetObjectItem(r,"success"); const bool saved=ok_item&&cJSON_IsTrue(ok_item);
  char *out=cJSON_PrintUnformatted(r); httpd_resp_set_type(req,"application/json"); httpd_resp_sendstr(req,out); free(out); cJSON_Delete(r); if(j)cJSON_Delete(j);
  if(saved){ vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
  return ESP_OK;
}
void web_send_json(httpd_req_t *req, cJSON *r) {
  char *out = cJSON_PrintUnformatted(r);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, out ? out : "{}");
  free(out);
  cJSON_Delete(r);
}

static esp_err_t device_name_handler(httpd_req_t *req){
  if(!web_admin_ok(req)) return ESP_OK;
  char b[256]; if(web_recv_json(req,b,sizeof(b))!=ESP_OK){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid body");return ESP_FAIL;} cJSON *j=cJSON_Parse(b); cJSON *n=j?cJSON_GetObjectItem(j,"name"):NULL; cJSON *r=cJSON_CreateObject();
  if(n&&cJSON_IsString(n)){ esp_err_t e=settings_set_device_name(n->valuestring); if(e==ESP_OK)wifi_set_hostname(n->valuestring); cJSON_AddBoolToObject(r,"success",e==ESP_OK); if(e!=ESP_OK)cJSON_AddStringToObject(r,"error",esp_err_to_name(e)); } else {cJSON_AddBoolToObject(r,"success",false);cJSON_AddStringToObject(r,"error","Invalid name");}
  char *out=cJSON_PrintUnformatted(r); httpd_resp_set_type(req,"application/json"); httpd_resp_sendstr(req,out); free(out); cJSON_Delete(r); if(j)cJSON_Delete(j); return ESP_OK;
}




static esp_err_t ota_handler(httpd_req_t *req){ if(!web_admin_ok(req)) return ESP_OK; if(req->content_len==0){httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"No firmware uploaded");return ESP_FAIL;} AP_LOGI(TAG,"Stopping the audio source for OTA"); (void)web_role_source_stop(); if(!web_role_source_idle()) return source_still_stopping(req); esp_err_t e=ota_start_from_http(req); if(e!=ESP_OK){ ESP_LOGE(TAG,"OTA failed (%s)",esp_err_to_name(e)); esp_err_t re=web_role_source_start(); if(re!=ESP_OK){ESP_LOGE(TAG,"Audio source restart after OTA failure failed: %s",esp_err_to_name(re));} httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,esp_err_to_name(e));return e;} httpd_resp_sendstr(req,"Firmware update complete, rebooting now!\n"); vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); return ESP_OK; }
static const char *reset_reason_str(esp_reset_reason_t r){switch(r){case ESP_RST_POWERON:return"poweron";case ESP_RST_EXT:return"external";case ESP_RST_SW:return"software";case ESP_RST_PANIC:return"panic";case ESP_RST_INT_WDT:return"int_wdt";case ESP_RST_TASK_WDT:return"task_wdt";case ESP_RST_WDT:return"other_wdt";case ESP_RST_DEEPSLEEP:return"deepsleep";case ESP_RST_BROWNOUT:return"brownout";case ESP_RST_SDIO:return"sdio";default:return"unknown";}}
static esp_err_t system_info_handler(httpd_req_t *req){
  cJSON *root=cJSON_CreateObject(),*i=cJSON_CreateObject(); char ip[16]={0},mac[18]={0},name[65]={0}; bool connected=wifi_is_connected(); wifi_get_ip_str(ip,sizeof(ip)); wifi_get_mac_str(mac,sizeof(mac)); settings_get_device_name(name,sizeof(name)); cJSON_AddStringToObject(i,"ip",ip);cJSON_AddStringToObject(i,"mac",mac);cJSON_AddStringToObject(i,"device_name",name);cJSON_AddBoolToObject(i,"wifi_connected",connected);cJSON_AddStringToObject(i,"role",web_role_name());cJSON_AddNumberToObject(i,"free_heap",esp_get_free_heap_size());
  if(connected){wifi_ap_record_t ap;if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){char ssid[33]={0},bssid[18];memcpy(ssid,ap.ssid,32);snprintf(bssid,sizeof(bssid),"%02x:%02x:%02x:%02x:%02x:%02x",ap.bssid[0],ap.bssid[1],ap.bssid[2],ap.bssid[3],ap.bssid[4],ap.bssid[5]);const char *phy=ap.phy_11n?"11n":ap.phy_11g?"11g":ap.phy_11b?"11b":ap.phy_lr?"LR":"?";cJSON_AddStringToObject(i,"wifi_ssid",ssid);cJSON_AddStringToObject(i,"wifi_bssid",bssid);cJSON_AddNumberToObject(i,"wifi_rssi",ap.rssi);cJSON_AddNumberToObject(i,"wifi_channel",ap.primary);cJSON_AddStringToObject(i,"wifi_phy",phy);}}
  const esp_app_desc_t *d=esp_app_get_description();cJSON_AddStringToObject(i,"firmware_version",d->version);cJSON_AddStringToObject(i,"reset_reason",reset_reason_str(esp_reset_reason()));cJSON_AddNumberToObject(i,"uptime_s",(double)(esp_timer_get_time()/1000000));cJSON_AddItemToObject(root,"info",i);cJSON_AddBoolToObject(root,"success",true);char *out=cJSON_PrintUnformatted(root);httpd_resp_set_type(req,"application/json");httpd_resp_sendstr(req,out);free(out);cJSON_Delete(root);return ESP_OK;
}
static esp_err_t restart_handler(httpd_req_t *req){if(!web_admin_ok(req)) return ESP_OK;httpd_resp_sendstr(req,"Restarting\n");vTaskDelay(pdMS_TO_TICKS(200));esp_restart();return ESP_OK;}
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
  REG("/",HTTP_GET,root_handler);REG("/favicon.ico",HTTP_GET,favicon_handler);REG("/logs",HTTP_GET,logs_handler);REG("/speedtest",HTTP_GET,speedtest_handler);REG("/api/wifi/scan",HTTP_GET,wifi_scan_handler);REG("/api/wifi/config",HTTP_POST,wifi_config_handler);REG("/api/device/name",HTTP_POST,device_name_handler);REG("/api/ota/update",HTTP_POST,ota_handler);REG("/api/system/info",HTTP_GET,system_info_handler);REG("/api/system/restart",HTTP_POST,restart_handler);REG("/api/speedtest/ping",HTTP_GET,speed_ping);REG("/api/speedtest/download",HTTP_GET,speed_download);REG("/api/speedtest/upload",HTTP_POST,speed_upload);REG("/hotspot-detect.html",HTTP_GET,captive_redirect);REG("/library/test/success.html",HTTP_GET,captive_redirect);REG("/generate_204",HTTP_GET,captive_redirect);REG("/connecttest.txt",HTTP_GET,captive_redirect);
  web_role_register(s_server);
  ESP_ERROR_CHECK(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404_handler));
#undef REG
  e=log_stream_register(s_server);if(e!=ESP_OK)ESP_LOGW(TAG,"log stream register failed: %s",esp_err_to_name(e));ESP_LOGI(TAG,"Web UI started on port %u",port);return ESP_OK; }
