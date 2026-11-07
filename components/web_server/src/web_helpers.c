/* SPDX-License-Identifier: Apache-2.0 */

/* The helpers of web_server_internal.h, used by the shared handlers and by
 * each firmware's own pages and APIs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_receiver.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "web_priv.h"
#include "web_rules.h"
#include "web_server_internal.h"

#ifndef CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD
#error "CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD comes from the airplay_options component"
#endif

static const char *TAG = "web_server";

#define PAGE_CHUNK 1024
#define AUTH_HEADER_MAX 1024
#define SOURCE_POLL_MS 50
#define SOURCE_WAIT_MS 30000
#define FAIL_RESTART_DELAY_MS 250

esp_err_t web_recv_body(httpd_req_t *req, char *buf, size_t buf_len,
                        size_t len, bool keep) {
  if (len == 0) {
    return ESP_OK;
  }
  if (buf == NULL || buf_len == 0 || (keep && buf_len < len)) {
    return ESP_ERR_INVALID_ARG;
  }
  size_t done = 0;
  int timeouts = 0;
  while (done < len) {
    size_t want = len - done;
    char *dst = buf + done;
    if (!keep) {
      dst = buf;
      if (want > buf_len) {
        want = buf_len;
      }
    }
    int r = httpd_req_recv(req, dst, want);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts < WEB_RECV_MAX_TIMEOUTS) {
        continue;
      }
      ESP_LOGW(TAG, "Body: %d receive timeouts", timeouts);
      return ESP_FAIL;
    }
    if (r <= 0 || (size_t)r > want) {
      return ESP_FAIL; /* error, or the peer closed early */
    }
    done += (size_t)r;
  }
  return ESP_OK;
}

esp_err_t web_serve_page(httpd_req_t *req, const uint8_t *start,
                         const uint8_t *end) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  size_t len = (start != NULL && end != NULL && end > start)
                   ? (size_t)(end - start)
                   : 0;
  for (size_t off = 0; off < len;) {
    size_t n = len - off < PAGE_CHUNK ? len - off : PAGE_CHUNK;
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)start + off,
                                          (ssize_t)n);
    if (err != ESP_OK) {
      return err;
    }
    off += n;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t web_recv_json(httpd_req_t *req, char *buf, size_t cap) {
  if (buf == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (req->content_len == 0 || req->content_len >= cap) {
    return ESP_ERR_INVALID_SIZE;
  }
  esp_err_t err = web_recv_body(req, buf, cap, req->content_len, true);
  if (err != ESP_OK) {
    return ESP_FAIL;
  }
  buf[req->content_len] = '\0';
  return ESP_OK;
}

void web_send_json(httpd_req_t *req, cJSON *r) {
  httpd_resp_set_type(req, HTTPD_TYPE_JSON);
  if (r == NULL) {
    httpd_resp_sendstr(req, "{}");
    return;
  }
  char *text = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  if (text == NULL) {
    ESP_LOGE(TAG, "JSON answer: out of memory");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return;
  }
  httpd_resp_sendstr(req, text);
  cJSON_free(text);
}

bool web_admin_ok(httpd_req_t *req) {
  static const char password[] = CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD;
  if (password[0] == '\0') {
    return true;
  }
  bool ok = false;
  size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (len > 0 && len <= AUTH_HEADER_MAX) {
    char *header = malloc(len + 1);
    if (header != NULL &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, len + 1) ==
            ESP_OK) {
      ok = web_rules_basic_auth_ok(header, password);
    }
    free(header);
  }
  if (ok) {
    return true;
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"AirPlay ESP32\"");
  httpd_resp_sendstr(req, "Authentication required\n");
  return false;
}

void web_log_memory(const char *where) {
  const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  ESP_LOGI(TAG,
           "MEM %s internalFree=%u internalLargest=%u psramFree=%u "
           "psramLargest=%u",
           where != NULL ? where : "",
           (unsigned)(heap_caps_get_free_size(internal) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(internal) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) /
                      1024));
}

esp_err_t web_audio_pause(httpd_req_t *req, const char *what, bool *paused) {
  bool unused;
  if (paused == NULL) {
    paused = &unused;
  }
  if (what == NULL) {
    what = "";
  }
  *paused = false;
  if (!audio_receiver_is_initialized()) {
    return ESP_OK;
  }

  esp_err_t err = web_role_source_stop();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "%s: stopping the audio source: %s", what,
             esp_err_to_name(err));
  }
  for (uint32_t waited = 0; !web_role_source_idle(); waited += SOURCE_POLL_MS) {
    if (waited >= SOURCE_WAIT_MS) {
      ESP_LOGE(TAG, "%s: audio source did not stop; restarting", what);
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_sendstr(req,
                         "AirPlay clients are still stopping; recovering "
                         "service");
      vTaskDelay(pdMS_TO_TICKS(FAIL_RESTART_DELAY_MS));
      esp_restart();
      return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(SOURCE_POLL_MS));
  }

  err = audio_receiver_release_for_wifi_scan();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: audio memory not released (%s)", what,
             esp_err_to_name(err));
    esp_err_t start_err = web_role_source_start();
    if (start_err != ESP_OK) {
      ESP_LOGE(TAG, "%s: restarting the audio source: %s", what,
               esp_err_to_name(start_err));
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: audio memory not released", what);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_FAIL;
  }
  *paused = true;
  return ESP_OK;
}

esp_err_t web_audio_resume(httpd_req_t *req, const char *what) {
  if (what == NULL) {
    what = "";
  }
  esp_err_t err = audio_receiver_init();
  if (err == ESP_OK) {
    err = web_role_source_start();
  }
  if (err == ESP_OK) {
    return ESP_OK;
  }
  ESP_LOGE(TAG, "%s: audio restore failed (%s); restarting", what,
           esp_err_to_name(err));
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                      "Audio restore failed; restarting");
  vTaskDelay(pdMS_TO_TICKS(FAIL_RESTART_DELAY_MS));
  esp_restart();
  return ESP_FAIL;
}
