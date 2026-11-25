#include "gw_rew.h"

#if defined(SURROUND_PROTO_MEASURE)

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_log.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw_calib.h"
#include "gw_radio.h"
#include "gw_speakers.h"
#include "sdkconfig.h"
#include "web_server_internal.h"
#if CONFIG_GATEWAY_USB_PLAYER
#include "gw_player.h"
#endif

static const char *TAG = "gw_rew";

/* The mode ends by itself this long after the last action. */
#define REW_TIMEOUT_US (15LL * 60LL * 1000000LL)

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timer;
/* Under s_lock. */
static struct {
  bool active;
  bool selected; /* a speaker plays */
  uint8_t mac[6];
  char name[24];
  uint8_t way; /* SURROUND_MEASURE_WAY_* */
  bool eq_on;
  bool invert;
  int64_t last_us; /* the last action */
} s_st;

static const char *way_name(uint8_t way) {
  return way == SURROUND_MEASURE_WAY_WOOFER    ? "woofer"
         : way == SURROUND_MEASURE_WAY_TWEETER ? "tweeter"
                                               : "full";
}

static bool way_from_name(const char *s, uint8_t *way) {
  if (!s || strcmp(s, "full") == 0) *way = SURROUND_MEASURE_WAY_FULL;
  else if (strcmp(s, "woofer") == 0) *way = SURROUND_MEASURE_WAY_WOOFER;
  else if (strcmp(s, "tweeter") == 0) *way = SURROUND_MEASURE_WAY_TWEETER;
  else return false;
  return true;
}

static bool parse_mac(const char *s, uint8_t mac[6]) {
  unsigned v[6];
  if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3],
                   &v[4], &v[5]) != 6)
    return false;
  for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)v[i];
  return true;
}

/* The connected speaker's name, false when it is not connected. */
static bool speaker_name(const uint8_t mac[6], char *name, size_t size) {
  gw_speaker_info_t *list = malloc(sizeof(gw_speaker_info_t) * SURROUND_MAX_SPEAKERS);
  if (!list) return false;
  const size_t n = gw_speakers_list(list);
  bool found = false;
  for (size_t i = 0; i < n && !found; ++i) {
    if (memcmp(list[i].mac, mac, 6) != 0) continue;
    strlcpy(name, list[i].name[0] ? list[i].name : list[i].ip, size);
    found = true;
  }
  free(list);
  return found;
}

/* Every speaker gets the state; caller holds s_lock. */
static void send_locked(void) {
  const surround_measure_t m = {
      .active = s_st.active,
      .way = s_st.way,
      .eq_on = s_st.eq_on,
      .invert_tweeter = s_st.invert,
  };
  gw_speakers_set_measure(&m, s_st.active && s_st.selected ? s_st.mac : NULL);
  if (!s_st.active) return;
  if (s_st.selected) {
    AP_LOGI(TAG, "REW mode: only %s (%s) plays, EQ %s%s", s_st.name, way_name(s_st.way),
            s_st.eq_on ? "on" : "off", s_st.invert ? ", tweeter inverted" : "");
  } else {
    AP_LOGI(TAG, "REW mode: no speaker plays, EQ %s%s", s_st.eq_on ? "on" : "off",
            s_st.invert ? ", tweeter inverted" : "");
  }
}

/* An action: the 15 minutes start again. Caller holds s_lock. */
static void touch_locked(void) {
  s_st.last_us = esp_timer_get_time();
  (void)esp_timer_stop(s_timer);
  (void)esp_timer_start_once(s_timer, REW_TIMEOUT_US);
}

static void stop_locked(const char *why) {
  if (!s_st.active) return;
  (void)esp_timer_stop(s_timer);
  memset(&s_st, 0, sizeof(s_st));
  send_locked();
  AP_LOGI(TAG, "REW mode off (%s): every speaker plays as before", why);
}

void gw_rew_stop_for(const char *why) {
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  stop_locked(why);
  xSemaphoreGive(s_lock);
}

bool gw_rew_active(void) {
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool on = s_st.active;
  xSemaphoreGive(s_lock);
  return on;
}

static void timeout_cb(void *arg) {
  (void)arg;
  gw_rew_stop_for("15 minutes without an action");
}

/* Why the mode cannot start now, NULL when it can. */
static const char *start_blocked(void) {
  if (gw_calib_running()) return "a calibration or Room EQ measurement is running";
  if (gw_radio_active()) return "the web radio plays: stop it first";
#if CONFIG_GATEWAY_USB_PLAYER
  if (gw_player_active()) return "the USB player plays: stop it first";
#endif
  return NULL;
}

static void state_json(cJSON *r) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool active = s_st.active, selected = s_st.selected;
  uint8_t mac[6];
  memcpy(mac, s_st.mac, sizeof(mac));
  char name[24];
  strlcpy(name, s_st.name, sizeof(name));
  const uint8_t way = s_st.way;
  const bool eq_on = s_st.eq_on, invert = s_st.invert;
  const int64_t last_us = s_st.last_us;
  xSemaphoreGive(s_lock);

  cJSON_AddBoolToObject(r, "active", active);
  if (!active) return;
  char m[18] = "";
  if (selected)
    snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  cJSON_AddStringToObject(r, "mac", m);
  cJSON_AddStringToObject(r, "name", selected ? name : "");
  cJSON_AddStringToObject(r, "way", way_name(way));
  cJSON_AddBoolToObject(r, "eq_on", eq_on);
  cJSON_AddBoolToObject(r, "invert_tweeter", invert);
  const int64_t left = REW_TIMEOUT_US - (esp_timer_get_time() - last_us);
  cJSON_AddNumberToObject(r, "seconds_left", left > 0 ? (double)(left / 1000000LL) : 0);
  /* What the speakers say: a 5.1/7.1 stream (the marked speaker plays its
   * channel, not L+R) and the marked speaker's clipping. */
  gw_speaker_info_t *list = malloc(sizeof(gw_speaker_info_t) * SURROUND_MAX_SPEAKERS);
  bool multichannel = false;
  uint32_t clips = 0;
  float peak_db = 0.0f;
  if (list) {
    const size_t n = gw_speakers_list(list);
    for (size_t i = 0; i < n; ++i) {
      if (!list[i].status_valid) continue;
      const surround_status_t *st = &list[i].status;
      if ((st->flags & SURROUND_STATUS_ACTIVE) && st->stream_channels > 2) multichannel = true;
      if (selected && memcmp(list[i].mac, mac, 6) == 0) {
        clips = st->measure_clips;
        peak_db = (float)st->measure_peak_db10 / 10.0f;
      }
    }
    free(list);
  }
  cJSON_AddBoolToObject(r, "multichannel", multichannel);
  cJSON_AddNumberToObject(r, "clips", clips);
  if (clips) cJSON_AddNumberToObject(r, "clip_peak_db", peak_db);
}

static esp_err_t rew_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    cJSON *r = cJSON_CreateObject();
    if (!r) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    cJSON_AddBoolToObject(r, "success", true);
    state_json(r);
    web_send_json(req, r);
    return ESP_OK;
  }
  if (req->method != HTTP_POST)
    return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "GET or POST");
  if (!web_admin_ok(req)) return ESP_OK;
  char b[192];
  if (web_recv_json(req, b, sizeof(b)) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
  cJSON *j = cJSON_Parse(b);
  const cJSON *a = j ? cJSON_GetObjectItem(j, "action") : NULL;
  const char *action = cJSON_IsString(a) ? a->valuestring : "";
  const cJSON *eq = j ? cJSON_GetObjectItem(j, "eq_on") : NULL;
  const cJSON *inv = j ? cJSON_GetObjectItem(j, "invert_tweeter") : NULL;
  const char *error = NULL;

  if (strcmp(action, "start") == 0) {
    error = start_blocked();
    if (!error) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      if (!s_st.active) {
        memset(&s_st, 0, sizeof(s_st));
        s_st.active = true;
        s_st.eq_on = true; /* the speaker as it plays; off from the window */
      }
      if (cJSON_IsBool(eq)) s_st.eq_on = cJSON_IsTrue(eq);
      if (cJSON_IsBool(inv)) s_st.invert = cJSON_IsTrue(inv);
      touch_locked();
      send_locked();
      xSemaphoreGive(s_lock);
    }
  } else if (strcmp(action, "select") == 0) {
    const cJSON *m = cJSON_GetObjectItem(j, "mac");
    const cJSON *w = cJSON_GetObjectItem(j, "way");
    uint8_t mac[6] = {0}, way = SURROUND_MEASURE_WAY_FULL;
    char name[24] = "";
    const bool none = cJSON_IsNull(m) || (cJSON_IsString(m) && m->valuestring[0] == '\0');
    if (!none && (!cJSON_IsString(m) || !parse_mac(m->valuestring, mac))) {
      error = "mac: the speaker's MAC, or \"\" for none";
    } else if (!way_from_name(cJSON_IsString(w) ? w->valuestring : NULL, &way)) {
      error = "way: full, woofer or tweeter";
    } else if (!none && !speaker_name(mac, name, sizeof(name))) {
      error = "the speaker is not connected";
    } else {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      if (!s_st.active) {
        error = "the REW mode is off: start it first";
      } else {
        s_st.selected = !none;
        memcpy(s_st.mac, mac, sizeof(mac));
        strlcpy(s_st.name, name, sizeof(s_st.name));
        s_st.way = way;
        touch_locked();
        send_locked();
      }
      xSemaphoreGive(s_lock);
    }
  } else if (strcmp(action, "set") == 0) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_st.active) {
      error = "the REW mode is off: start it first";
    } else {
      if (cJSON_IsBool(eq)) s_st.eq_on = cJSON_IsTrue(eq);
      if (cJSON_IsBool(inv)) s_st.invert = cJSON_IsTrue(inv);
      touch_locked();
      send_locked();
    }
    xSemaphoreGive(s_lock);
  } else if (strcmp(action, "stop") == 0) {
    gw_rew_stop_for("stopped from the web page");
  } else {
    error = "action: start, select, set or stop";
  }
  if (j) cJSON_Delete(j);

  cJSON *r = cJSON_CreateObject();
  if (!r) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  cJSON_AddBoolToObject(r, "success", error == NULL);
  if (error) cJSON_AddStringToObject(r, "error", error);
  state_json(r);
  web_send_json(req, r);
  return ESP_OK;
}

extern const uint8_t web_rew_js_start[] asm("_binary_web_rew_js_start");
extern const uint8_t web_rew_js_end[] asm("_binary_web_rew_js_end");

/* /rew.js: the bar on the pages and the window on /speakers. */
static esp_err_t rew_js_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, (const char *)web_rew_js_start,
                         web_rew_js_end - web_rew_js_start);
}

void gw_rew_register_web(httpd_handle_t server) {
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t t = {.callback = timeout_cb, .name = "gw_rew"};
    if (!s_lock || esp_timer_create(&t, &s_timer) != ESP_OK) {
      ESP_LOGE(TAG, "REW mode unavailable: no memory");
      if (s_lock) vSemaphoreDelete(s_lock);
      s_lock = NULL;
      return;
    }
  }
  /* One route for GET and POST: the Gateway is near max_uri_handlers. */
  const httpd_uri_t uris[] = {
      {.uri = "/api/rew", .method = HTTP_ANY, .handler = rew_handler},
      {.uri = "/rew.js", .method = HTTP_GET, .handler = rew_js_handler},
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
    const esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
    if (err != ESP_OK)
      ESP_LOGE(TAG, "web handler %s not registered: %s", uris[i].uri, esp_err_to_name(err));
  }
}

#endif /* SURROUND_PROTO_MEASURE */
