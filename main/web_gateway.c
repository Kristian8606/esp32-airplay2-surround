/* The Gateway's own pages and APIs: the speakers page, and play/pause and
 * volume of the current AirPlay sender. */
#include "web_server_internal.h"
#include "homekit_reset.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_log.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "cal_chirp.h"
#include "gw_calib.h"
#include "gw_mic.h"
#include "gw_fw.h"
#include "gw_radio.h"
#include "gw_speakers.h"
#include "sound_engine.h"
#include "rtsp_remote.h"
#include "rtsp_server.h"

extern const uint8_t web_speakers_start[] asm("_binary_web_speakers_html_start");
extern const uint8_t web_speakers_end[] asm("_binary_web_speakers_html_end");
extern const uint8_t web_roomeq_start[] asm("_binary_web_roomeq_html_start");
extern const uint8_t web_roomeq_end[] asm("_binary_web_roomeq_html_end");

static esp_err_t speakers_page_handler(httpd_req_t *req) {
  return web_serve_page(req, web_speakers_start, web_speakers_end);
}

static esp_err_t roomeq_page_handler(httpd_req_t *req) {
  return web_serve_page(req, web_roomeq_start, web_roomeq_end);
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
    if (sp->fw_update) cJSON_AddStringToObject(o, "fw_update", sp->fw_update);
    cJSON_AddStringToObject(o, "state", sp->state);
    cJSON_AddBoolToObject(o, "audio", sp->audio);
    if (sp->config_valid) {
      cJSON_AddNumberToObject(o, "channel", sp->config.channel);
      cJSON_AddNumberToObject(o, "trim_db", sp->config.trim_db10 / 10.0);
      cJSON_AddNumberToObject(o, "delay_ms", sp->config.delay_us / 1000.0);
      cJSON_AddBoolToObject(o, "multichannel", sp->config.multichannel != 0U);
      cJSON_AddNumberToObject(o, "lowpass_hz", sp->config.lowpass_hz);
      cJSON_AddNumberToObject(o, "lfe_gain_db", sp->config.lfe_gain_db);
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
  cJSON *lowpass = j ? cJSON_GetObjectItem(j, "lowpass_hz") : NULL;
  cJSON *lfe_gain = j ? cJSON_GetObjectItem(j, "lfe_gain_db") : NULL;
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
  if (!error && lowpass) {
    const double d = cJSON_IsNumber(lowpass) ? lowpass->valuedouble : -1.0;
    const long v = d >= 0.0 && d <= 1000.0 ? lround(d) : -1;
    if (v == SURROUND_LOWPASS_OFF ||
        (v >= SURROUND_LOWPASS_MIN_HZ && v <= SURROUND_LOWPASS_MAX_HZ)) {
      cfg.fields |= SURROUND_SET_LOWPASS;
      cfg.lowpass_hz = (uint16_t)v;
    } else {
      error = "lowpass_hz: 0 (off) or 40 .. 250";
    }
  }
  if (!error && lfe_gain) {
    const double d = cJSON_IsNumber(lfe_gain) ? lfe_gain->valuedouble : -1.0;
    const long v = d >= 0.0 && d <= 100.0 ? lround(d) : -1;
    if (v >= 0 && v <= SURROUND_LFE_GAIN_MAX_DB) {
      cfg.fields |= SURROUND_SET_LFE_GAIN;
      cfg.lfe_gain_db = (uint8_t)v;
    } else {
      error = "lfe_gain_db: 0 .. 10";
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
        "Too many commands waiting" : "No AirPlay session. Start AirPlay first.");
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

static void mac_str(const uint8_t m[6], char out[18]) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* What the Room EQ page needs to deconvolve the recording: the sweep's
 * definition (cal_chirp.h) and where each one starts in it. */
static void add_sweep_json(cJSON *r, const gw_cal_sweep_t *sw) {
  cJSON *j = cJSON_AddObjectToObject(r, "sweep");
  if (!j) return;
  char mac[18];
  mac_str(sw->mac, mac);
  cal_sweep_def_t d = {0};
  (void)cal_sweep_def((cal_chirp_kind_t)sw->kind, &d);
  cJSON_AddNumberToObject(j, "id", sw->id);
  cJSON_AddStringToObject(j, "mac", mac);
  cJSON_AddStringToObject(j, "name", sw->name);
  cJSON_AddNumberToObject(j, "channel", sw->channel);
  cJSON_AddStringToObject(j, "kind", cal_chirp_name((cal_chirp_kind_t)sw->kind));
  cJSON_AddBoolToObject(j, "raw", sw->raw);
  cJSON_AddNumberToObject(j, "level_db", sw->level_db);
  cJSON_AddNumberToObject(j, "rate", GW_MIC_RATE_HZ);
  cJSON_AddNumberToObject(j, "count", sw->count);
  cJSON_AddNumberToObject(j, "scale", sw->scale);
  cJSON_AddNumberToObject(j, "peak_dbfs", roundf(sw->peak_dbfs * 10.0f) / 10.0f);
  cJSON_AddNumberToObject(j, "f0_hz", d.f0_hz);
  cJSON_AddNumberToObject(j, "f1_hz", d.f1_hz);
  cJSON_AddNumberToObject(j, "length_s", d.length_s);
  cJSON_AddNumberToObject(j, "fade_in_s", d.fade_in_s);
  cJSON_AddNumberToObject(j, "fade_out_s", d.fade_out_s);
  cJSON_AddNumberToObject(j, "period_s", cal_chirp_period_s((cal_chirp_kind_t)sw->kind));
  cJSON *arr = cJSON_AddArrayToObject(j, "bursts");
  for (int k = 0; arr && k < sw->n; ++k) {
    cJSON *b = cJSON_CreateObject();
    const uint8_t w = sw->way[k];
    const bool tw = w & SURROUND_CAL_WAY_TWEETER, wf = w & SURROUND_CAL_WAY_WOOFER;
    cJSON_AddNumberToObject(b, "start", round(sw->start[k] * 100.0) / 100.0);
    cJSON_AddStringToObject(b, "way", tw && !wf ? "tweeter" : wf && !tw ? "woofer"
                                                 : tw ? "both" : "full");
    const bool l = w & SURROUND_CAL_WAY_LEFT, rr = w & SURROUND_CAL_WAY_RIGHT;
    cJSON_AddStringToObject(b, "outputs", l && rr ? "both" : l ? "left" : rr ? "right" : "none");
    cJSON_AddNumberToObject(b, "snr_db", roundf(sw->snr_db[k] * 10.0f) / 10.0f);
    cJSON_AddItemToArray(arr, b);
  }
}

/* The sweep recording as 16-bit PCM (little endian, mono, `rate`), scaled
 * by `scale` (its peak just under full scale). */
static esp_err_t calibration_sweep_handler(httpd_req_t *req) {
  char q[32], v[16];
  uint32_t id = 0;
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK) {
    id = (uint32_t)strtoul(v, NULL, 10);
  }
  const float *x = NULL;
  gw_cal_sweep_t sw;
  if (!gw_calib_sweep_samples(id, &x, &sw)) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "No such sweep recording");
  }
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  int16_t buf[1024];
  for (uint32_t i = 0; i < sw.count;) {
    const uint32_t m = sw.count - i < 1024U ? sw.count - i : 1024U;
    for (uint32_t k = 0; k < m; ++k) {
      float f = x[i + k] * sw.scale * 32767.0f;
      if (f > 32767.0f) f = 32767.0f;
      if (f < -32768.0f) f = -32768.0f;
      buf[k] = (int16_t)lrintf(f);
    }
    if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)(m * sizeof(int16_t))) != ESP_OK) {
      return ESP_FAIL;
    }
    i += m;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
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
  cJSON_AddBoolToObject(r, "stream_active", !sound_engine_is_idle());
  cJSON_AddBoolToObject(r, "sender_stopped", st.sender_stopped);
  if (st.noise_valid) {
    cJSON_AddNumberToObject(r, "noise_dbfs", roundf(st.noise_dbfs * 10.0f) / 10.0f);
    cJSON_AddNumberToObject(r, "peak_dbfs", roundf(st.peak_dbfs * 10.0f) / 10.0f);
  }
  if (st.sweep_run) cJSON_AddBoolToObject(r, "sweep_run", true);
  if (st.sweep_run && st.sweep.valid) add_sweep_json(r, &st.sweep);
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
      cJSON_AddNumberToObject(j, "level_spread_db",
                              roundf(o->level_spread_db * 10.0f) / 10.0f);
      cJSON_AddNumberToObject(j, "snr_db", roundf(o->snr_db * 10.0f) / 10.0f);
      cJSON_AddNumberToObject(j, "spread_ms", roundf(o->spread_ms * 100.0f) / 100.0f);
      cJSON_AddNumberToObject(j, "match", roundf(o->match * 100.0f) / 100.0f);
      if (o->inverted) cJSON_AddBoolToObject(j, "inverted", true);
    }
    if (o->silent) cJSON_AddBoolToObject(j, "silent", true);
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
  char b[160];
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
  } else if (strcmp(action, "sweep") == 0) {
    const cJSON *m = cJSON_GetObjectItem(j, "mac");
    const cJSON *raw = cJSON_GetObjectItem(j, "raw");
    const cJSON *per_way = cJSON_GetObjectItem(j, "per_way");
    uint8_t mac[6];
    if (!cJSON_IsString(m) || !parse_mac(m->valuestring, mac)) {
      error = "mac: the speaker's MAC";
    } else {
      e = gw_calib_sweep_start(mac, !cJSON_IsFalse(raw), !cJSON_IsFalse(per_way));
      if (e == ESP_ERR_NOT_SUPPORTED) error = "built without the calibration microphone";
      else if (e == ESP_ERR_INVALID_STATE) error = "a measurement is already running";
      else if (e == ESP_ERR_NOT_FOUND) error = "the speaker is not connected";
    }
  } else if (strcmp(action, "apply") == 0) {
    e = gw_calib_apply(&applied);
    if (e == ESP_ERR_INVALID_STATE) error = "no results to apply";
    else if (e == ESP_ERR_NOT_FOUND) error = "no speaker took the results";
  } else if (strcmp(action, "discard") == 0) {
    gw_calib_discard();
    e = ESP_OK;
  } else {
    error = "action: test, start, sweep, apply or discard";
  }
  if (j) cJSON_Delete(j);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (e != ESP_OK) cJSON_AddStringToObject(r, "error", error ? error : esp_err_to_name(e));
  if (applied) cJSON_AddNumberToObject(r, "applied", applied);
  web_send_json(req, r);
  return ESP_OK;
}

/* ---- A speaker's own EQ through the Gateway (the Room EQ page) ----
 * POST /api/speakers/eq?mac=..&action=get|apply|both: get reads the
 * speaker's GET /api/eq, apply and both send the body to its POST /api/eq
 * (apply: now, not saved; both: now and saved). The answer is the speaker's,
 * passed through; the browser's Authorization goes along (the same admin
 * password on every board of one build). */
#define SPK_EQ_BODY_MAX 16384

static esp_err_t speaker_eq_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  char q[96], mac_s[40] = "", action[8] = "get";
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    (void)httpd_query_key_value(q, "mac", mac_s, sizeof(mac_s));
    (void)httpd_query_key_value(q, "action", action, sizeof(action));
  }
  /* The query is not URL-decoded: a ':' may come as %3A. */
  for (char *r = mac_s, *w = mac_s;; ++r, ++w) {
    if (r[0] == '%' && r[1] == '3' && (r[2] == 'A' || r[2] == 'a')) {
      *w = ':';
      r += 2;
    } else {
      *w = *r;
    }
    if (!*r) break;
  }
  uint8_t mac[6];
  const bool get = strcmp(action, "get") == 0;
  if (!parse_mac(mac_s, mac) ||
      (!get && strcmp(action, "apply") != 0 && strcmp(action, "both") != 0)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mac and action: get, apply or both");
    return ESP_FAIL;
  }
  gw_speaker_info_t *list = malloc(sizeof(gw_speaker_info_t) * SURROUND_MAX_SPEAKERS);
  char ip[16] = "";
  const size_t n = list ? gw_speakers_list(list) : 0U;
  for (size_t i = 0; i < n && !ip[0]; ++i) {
    if (memcmp(list[i].mac, mac, 6) == 0) strlcpy(ip, list[i].ip, sizeof(ip));
  }
  free(list);
  if (!ip[0]) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "The speaker is not connected");
  }
  if (!get && (req->content_len <= 0 || req->content_len > SPK_EQ_BODY_MAX)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid EQ body");
    return ESP_FAIL;
  }
  char *buf = malloc(SPK_EQ_BODY_MAX + 1);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_ERR_NO_MEM;
  }
  int len = 0;
  if (!get) {
    while (len < (int)req->content_len) {
      const int r = httpd_req_recv(req, buf + len, req->content_len - len);
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      if (r <= 0) {
        free(buf);
        return ESP_FAIL;
      }
      len += r;
    }
  }
  char auth[160] = "";
  const size_t alen = httpd_req_get_hdr_value_len(req, "Authorization");
  if (alen > 0 && alen < sizeof(auth)) {
    (void)httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth));
  }
  char url[64];
  snprintf(url, sizeof(url), "http://%s/api/eq%s%s", ip, get ? "" : "?action=", get ? "" : action);
  const esp_http_client_config_t cfg = {
      .url = url,
      .method = get ? HTTP_METHOD_GET : HTTP_METHOD_POST,
      .timeout_ms = 5000,
      .disable_auto_redirect = true,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  esp_err_t err = c ? ESP_OK : ESP_ERR_NO_MEM;
  int status = 0, got = 0;
  if (err == ESP_OK) {
    if (!get) esp_http_client_set_header(c, "Content-Type", "application/json");
    if (auth[0]) esp_http_client_set_header(c, "Authorization", auth);
    err = esp_http_client_open(c, get ? 0 : len);
  }
  if (err == ESP_OK && !get && esp_http_client_write(c, buf, len) != len) err = ESP_FAIL;
  if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) err = ESP_FAIL;
  if (err == ESP_OK) {
    status = esp_http_client_get_status_code(c);
    while (got < SPK_EQ_BODY_MAX) {
      const int r = esp_http_client_read(c, buf + got, SPK_EQ_BODY_MAX - got);
      if (r <= 0) break;
      got += r;
    }
  }
  if (c) esp_http_client_cleanup(c);
  if (err != ESP_OK) {
    free(buf);
    AP_LOGW("web_gateway", "EQ of %s: %s", ip, esp_err_to_name(err));
    httpd_resp_set_status(req, "502 Bad Gateway");
    return httpd_resp_sendstr(req, "The speaker did not answer");
  }
  char st[40];
  snprintf(st, sizeof(st), "%d %s", status, status == 200 ? "OK" : "Speaker");
  httpd_resp_set_status(req, st);
  httpd_resp_set_type(req, status == 200 ? "application/json" : "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t send = httpd_resp_send(req, buf, got);
  free(buf);
  return send;
}

const char *web_role_name(void) { return "gateway"; }

esp_err_t web_role_source_stop(void) {
  rtsp_server_stop();
  return ESP_OK;
}

bool web_role_source_idle(void) { return rtsp_server_is_idle(); }

esp_err_t web_role_source_start(void) { return rtsp_server_start(); }

/* ---- Speaker firmware the Gateway hands out (gw_fw.c) ---- */
static esp_err_t speaker_fw_get_handler(httpd_req_t *req) {
  gw_fw_image_t img;
  gw_fw_get(&img);
  gw_fw_busy_t busy;
  gw_speakers_fw_busy(&busy);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", true);
  cJSON_AddBoolToObject(r, "stored", img.valid);
  cJSON_AddNumberToObject(r, "capacity", (double)gw_fw_capacity());
  if (img.valid) {
    cJSON_AddStringToObject(r, "build", img.id);
    cJSON_AddNumberToObject(r, "size", img.size);
  }
  if (img.valid && img.halted) cJSON_AddStringToObject(r, "halted_by", img.halted_by);
  if (busy.active) {
    cJSON *b = cJSON_AddObjectToObject(r, "updating");
    cJSON_AddStringToObject(b, "name", busy.name);
    cJSON_AddStringToObject(b, "ip", busy.ip);
    cJSON_AddNumberToObject(b, "seconds", busy.seconds);
  }
  web_send_json(req, r);
  return ESP_OK;
}
static esp_err_t speaker_fw_post_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  const char *why = NULL;
  const esp_err_t err = gw_fw_receive(req, &why);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", err == ESP_OK);
  if (err != ESP_OK) cJSON_AddStringToObject(r, "error", why ? why : esp_err_to_name(err));
  gw_fw_image_t img;
  gw_fw_get(&img);
  if (img.valid) cJSON_AddStringToObject(r, "build", img.id);
  web_send_json(req, r);
  return ESP_OK;
}
static esp_err_t speaker_fw_bin_handler(httpd_req_t *req) { return gw_fw_serve(req); }

void web_role_register(httpd_handle_t server) {
  ESP_ERROR_CHECK(homekit_reset_register(server));
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
  REG("/api/calibration/sweep", HTTP_GET, calibration_sweep_handler);
  REG("/roomeq", HTTP_GET, roomeq_page_handler);
  REG("/api/speakers/eq", HTTP_POST, speaker_eq_handler);
  REG("/api/speakers/firmware", HTTP_GET, speaker_fw_get_handler);
  REG("/api/speakers/firmware", HTTP_POST, speaker_fw_post_handler);
  REG(SURROUND_FW_PATH, HTTP_GET, speaker_fw_bin_handler);
#undef REG
  gw_radio_register_web(server);
}
