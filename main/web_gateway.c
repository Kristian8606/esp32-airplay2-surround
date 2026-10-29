/* The Gateway's own pages and APIs: the speakers page, and play/pause and
 * volume of the current AirPlay sender. */
#include "web_server_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "gw_calib.h"
#include "gw_speakers.h"
#include "audio_receiver.h"
#include "rtsp_remote.h"
#include "rtsp_server.h"

extern const uint8_t web_speakers_start[] asm("_binary_web_speakers_html_start");
extern const uint8_t web_speakers_end[] asm("_binary_web_speakers_html_end");

static esp_err_t speakers_page_handler(httpd_req_t *req) {
  return web_serve_page(req, web_speakers_start, web_speakers_end);
}
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
  web_send_json(req, r);
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
  if (!web_admin_ok(req)) return ESP_OK;
  char b[192];
  if (web_recv_json(req, b, sizeof(b)) != ESP_OK) {
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
    /* Range-checked as a double first: lround() of a huge value is
     * undefined. */
    const double d = cJSON_IsNumber(trim) ? trim->valuedouble : 1.0;
    const long v = d >= -100.0 && d <= 100.0 ? lround(d * 10.0) : 1;
    if (v >= SURROUND_TRIM_MIN_DB10 && v <= SURROUND_TRIM_MAX_DB10) {
      cfg.fields |= SURROUND_SET_TRIM;
      cfg.trim_db10 = (int16_t)v;
    } else {
      error = "trim_db: -30 .. 0";
    }
  }
  if (!error && delay) {
    const double d = cJSON_IsNumber(delay) ? delay->valuedouble : -1.0;
    const long v = d >= 0.0 && d <= 1000.0 ? lround(d * 1000.0) : -1;
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
  web_send_json(req, r);
  return ESP_OK;
}

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
  web_send_json(req, root);
  return ESP_OK;
}

static esp_err_t remote_post_handler(httpd_req_t *req) {
  char body[96];
  if (web_recv_json(req, body, sizeof(body)) != ESP_OK)
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
  web_send_json(req, root);
  return ESP_OK;
}

/* ---- Room calibration (gw_calib.c) ---- */
static const char *cal_state_name(gw_cal_state_t st) {
  switch (st) {
    case GW_CAL_BUSY: return "busy";
    case GW_CAL_DONE: return "done";
    case GW_CAL_FAILED: return "failed";
    default: return "idle";
  }
}

static esp_err_t calibration_get_handler(httpd_req_t *req) {
  static gw_cal_status_t st; /* httpd task only */
  gw_calib_get(&st);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", true);
  cJSON_AddBoolToObject(r, "mic_built", st.mic_built);
  cJSON_AddStringToObject(r, "state", cal_state_name(st.state));
  cJSON_AddBoolToObject(r, "mic_test", st.mic_test);
  cJSON_AddStringToObject(r, "message", st.message);
  cJSON_AddNumberToObject(r, "step", st.step);
  cJSON_AddNumberToObject(r, "steps", st.steps);
  cJSON_AddBoolToObject(r, "stream_active", !audio_receiver_is_idle());
  cJSON_AddBoolToObject(r, "sender_stopped", st.sender_stopped);
  if (st.noise_valid) {
    cJSON_AddNumberToObject(r, "noise_dbfs", roundf(st.noise_dbfs * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(r, "peak_dbfs", roundf(st.peak_dbfs * 10.0f) / 10.0f);
  }
  cJSON *arr = cJSON_AddArrayToObject(r, "speakers");
  for (int i = 0; arr && i < st.count; ++i) {
    const gw_cal_speaker_t *o = &st.spk[i];
    cJSON *j = cJSON_CreateObject();
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", o->mac[0],
             o->mac[1], o->mac[2], o->mac[3], o->mac[4], o->mac[5]);
    cJSON_AddStringToObject(j, "mac", mac);
    cJSON_AddStringToObject(j, "name", o->name);
    cJSON_AddNumberToObject(j, "channel", o->channel);
    cJSON_AddBoolToObject(j, "measured", o->measured);
    if (o->measured) {
      cJSON_AddNumberToObject(j, "distance_m", roundf(o->distance_m * 100.0f) / 100.0f);
      cJSON_AddNumberToObject(j, "tof_ms", roundf(o->tof_ms * 100.0f) / 100.0f);
      cJSON_AddNumberToObject(j, "level_db", roundf(o->level_db * 10.0f) / 10.0f);
      cJSON_AddNumberToObject(j, "match", roundf(o->match * 100.0f) / 100.0f);
    }
    if (o->has_suggestion) cJSON_AddNumberToObject(j, "delay_ms", o->delay_ms);
    if (o->has_trim) cJSON_AddNumberToObject(j, "trim_db", o->trim_db);
    if (o->note[0]) cJSON_AddStringToObject(j, "note", o->note);
    cJSON_AddItemToArray(arr, j);
  }
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  web_send_json(req, r);
  return ESP_OK;
}

static esp_err_t calibration_post_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  char b[64];
  if (web_recv_json(req, b, sizeof(b)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }
  cJSON *j = cJSON_Parse(b);
  const cJSON *a = j ? cJSON_GetObjectItem(j, "action") : NULL;
  const char *action = cJSON_IsString(a) ? a->valuestring : "";
  esp_err_t e = ESP_ERR_INVALID_ARG;
  int applied = 0;
  const char *error = NULL;
  if (strcmp(action, "test") == 0 || strcmp(action, "start") == 0) {
    e = gw_calib_start(strcmp(action, "test") == 0);
    if (e == ESP_ERR_NOT_SUPPORTED) error = "built without the calibration microphone";
    else if (e == ESP_ERR_INVALID_STATE) error = "a measurement is already running";
  } else if (strcmp(action, "apply") == 0) {
    e = gw_calib_apply(&applied);
    if (e == ESP_ERR_INVALID_STATE) error = "no results to apply";
    else if (e == ESP_ERR_NOT_FOUND) error = "no speaker took the results";
  } else if (strcmp(action, "discard") == 0) {
    gw_calib_discard();
    e = ESP_OK;
  } else {
    error = "action: test, start, apply or discard";
  }
  if (j) cJSON_Delete(j);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (e != ESP_OK) cJSON_AddStringToObject(r, "error", error ? error : esp_err_to_name(e));
  if (applied) cJSON_AddNumberToObject(r, "applied", applied);
  web_send_json(req, r);
  return ESP_OK;
}

const char *web_role_name(void) { return "gateway"; }

esp_err_t web_role_source_stop(void) {
  rtsp_server_stop();
  return ESP_OK;
}

bool web_role_source_idle(void) { return rtsp_server_is_idle(); }

esp_err_t web_role_source_start(void) { return rtsp_server_start(); }

void web_role_register(httpd_handle_t server) {
#define REG(U, M, H)                                                     \
  do {                                                                   \
    httpd_uri_t x = {.uri = U, .method = M, .handler = H};               \
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &x));             \
  } while (0)
  REG("/speakers", HTTP_GET, speakers_page_handler);
  REG("/api/speakers", HTTP_GET, speakers_get_handler);
  REG("/api/speakers", HTTP_POST, speakers_post_handler);
  REG("/api/audio/remote", HTTP_GET, remote_get_handler);
  REG("/api/audio/remote", HTTP_POST, remote_post_handler);
  REG("/api/calibration", HTTP_GET, calibration_get_handler);
  REG("/api/calibration", HTTP_POST, calibration_post_handler);
#undef REG
}
