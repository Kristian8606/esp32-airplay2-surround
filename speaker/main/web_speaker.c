/* The speaker's own pages and APIs: EQ, loudness, its role, output latency
 * and output mute. */
#include "web_server_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aac_decoder.h"
#include "airplay_log.h"
#include "audio_eq.h"
#include "audio_loudness.h"
#include "audio_receiver.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "settings.h"
#include "spk_link.h"

static const char *TAG = "web_speaker";

extern const uint8_t web_eq_start[] asm("_binary_web_eq_html_start");
extern const uint8_t web_eq_end[] asm("_binary_web_eq_html_end");

static esp_err_t eq_page_handler(httpd_req_t *req) {
  return web_serve_page(req, web_eq_start, web_eq_end);
}

/* ---- Output latency (manual value + optional wired measurement) ---- */
/* ---- Output latency (manual value + optional wired measurement) ---- */
#ifndef CONFIG_AIRPLAY_OUTPUT_LATENCY_US
#define CONFIG_AIRPLAY_OUTPUT_LATENCY_US 0
#endif
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
  web_send_json(req, r);
  return ESP_OK;
}
static esp_err_t latency_post_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  char b[128];
  if (web_recv_json(req, b, sizeof(b)) != ESP_OK) {
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
  web_send_json(req, r);
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
  web_send_json(req, r);
  return ESP_OK;
}
static esp_err_t loudness_post_handler(httpd_req_t *req) {
  char b[96];
  if (web_recv_json(req, b, sizeof(b)) != ESP_OK) {
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
  web_send_json(req, r);
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
  web_send_json(req, r);
  return ESP_OK;
}
static esp_err_t speaker_post_handler(httpd_req_t *req) {
  char b[64];
  if (web_recv_json(req, b, sizeof(b)) != ESP_OK) {
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
    spk_link_config_changed();
  }
  if (j) cJSON_Delete(j);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "success", e == ESP_OK);
  if (e != ESP_OK)
    cJSON_AddStringToObject(r, "error", e == ESP_ERR_INVALID_ARG
                                            ? "speaker out of range"
                                            : esp_err_to_name(e));
  add_speaker_state(r);
  web_send_json(req, r);
  return ESP_OK;
}
static esp_err_t latency_measure_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  /* optional body {"audible":false}; default: switch the amplifier on */
  bool audible = true;
  if (req->content_len > 0 && req->content_len < 64) {
    char b[64];
    if (web_recv_json(req, b, sizeof(b)) == ESP_OK) {
      cJSON *j = cJSON_Parse(b);
      cJSON *a = j ? cJSON_GetObjectItem(j, "audible") : NULL;
      if (a && cJSON_IsBool(a)) audible = cJSON_IsTrue(a);
      if (j) cJSON_Delete(j);
    }
  }
  /* Same lifecycle as the Wi-Fi scan: stop the link to the Gateway,
   * release the audio memory, measure, restore. */
  bool restore = false;
  if (web_audio_pause(req, "Latency test", &restore) != ESP_OK) {
    return ESP_FAIL;
  }
  if (restore) web_log_memory("latency-test-released");
  latency_cal_result_t res;
  esp_err_t e = audio_receiver_measure_output_latency(&res, audible);
  if (restore && web_audio_resume(req, "Latency test") != ESP_OK) {
    return ESP_FAIL;
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
  web_send_json(req, r);
  return ESP_OK;
}

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

static void output_mute_to_json(cJSON *root) {
  const uint32_t mask = audio_receiver_get_output_mute_mask();
  cJSON_AddBoolToObject(root, "left_muted", (mask & 1U) != 0U);
  cJSON_AddBoolToObject(root, "right_muted", (mask & 2U) != 0U);
}

static esp_err_t output_mute_post_handler(httpd_req_t *req) {
  char body[128];
  if (web_recv_json(req, body, sizeof(body)) != ESP_OK) {
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
  web_send_json(req, root);
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
  esp_err_t recv_err = web_recv_json(req, body, (size_t)req->content_len + 1U);
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

const char *web_role_name(void) { return "speaker"; }

/* Before the audio engine is released, the link to the Gateway drops, so no
 * forwarded call reaches the released engine; it reconnects afterwards. */
esp_err_t web_role_source_stop(void) {
  spk_link_suspend();
  return ESP_OK;
}

bool web_role_source_idle(void) { return true; }

esp_err_t web_role_source_start(void) {
  spk_link_resume();
  return ESP_OK;
}

void web_role_register(httpd_handle_t server) {
#define REG(U, M, H)                                                     \
  do {                                                                   \
    httpd_uri_t x = {.uri = U, .method = M, .handler = H};               \
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &x));             \
  } while (0)
  REG("/eq", HTTP_GET, eq_page_handler);
  REG("/api/eq", HTTP_GET, eq_get_handler);
  REG("/api/eq", HTTP_POST, eq_post_handler);
  REG("/api/audio/mute", HTTP_POST, output_mute_post_handler);
  REG("/api/audio/latency", HTTP_GET, latency_get_handler);
  REG("/api/audio/latency", HTTP_POST, latency_post_handler);
  REG("/api/audio/latency/measure", HTTP_POST, latency_measure_handler);
  REG("/api/audio/loudness", HTTP_GET, loudness_get_handler);
  REG("/api/audio/loudness", HTTP_POST, loudness_post_handler);
  REG("/api/audio/speaker", HTTP_GET, speaker_get_handler);
  REG("/api/audio/speaker", HTTP_POST, speaker_post_handler);
#undef REG
}
