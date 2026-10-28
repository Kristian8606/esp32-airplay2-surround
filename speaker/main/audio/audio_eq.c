#include "audio_eq.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "airplay_log.h"

static const char *TAG = "audio_eq";
static const char *EQ_NS = "audio_eq";
static const char *EQ_KEY = "config";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
  /* Standard biquad naming: b = numerator, a = denominator (a0 = 1). */
  float b0;
  float b1;
  float b2;
  float a1;
  float a2;
} biquad_coeff_t;

typedef struct {
  /* Transposed Direct Form II state. */
  float z1;
  float z2;
} biquad_state_t;

typedef struct {
  audio_eq_config_t config;
  biquad_coeff_t coeff_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  biquad_coeff_t coeff_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  biquad_state_t state_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  biquad_state_t state_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  /* Filter identity at each ACTIVE position, so a live config change can tell
   * which delay-line states are still meaningful. */
  uint8_t type_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t type_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t slot_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t slot_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t active_l;
  uint8_t active_r;
  int sample_rate;
  float preamp_gain_l;
  float preamp_gain_r;
  bool ready;
} audio_eq_runtime_t;

static audio_eq_runtime_t s_eq;
static SemaphoreHandle_t s_eq_mutex;

static inline void eq_lock(void) {
  if (s_eq_mutex) xSemaphoreTake(s_eq_mutex, portMAX_DELAY);
}

static inline void eq_unlock(void) {
  if (s_eq_mutex) xSemaphoreGive(s_eq_mutex);
}

static bool eq_is_finite(float value) { return isfinite((double)value); }

void audio_eq_default_config(audio_eq_config_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->version = AUDIO_EQ_CONFIG_VERSION;
  out->enabled = 0;
  out->channel_mode = AUDIO_EQ_CHANNEL_STEREO;
  out->left_preamp_db = 0.0f;
  out->right_preamp_db = 0.0f;
}

bool audio_eq_validate_config(const audio_eq_config_t *config) {
  if (!config || config->version != AUDIO_EQ_CONFIG_VERSION ||
      config->enabled > 1 || config->channel_mode >= AUDIO_EQ_CHANNEL_COUNT ||
      !eq_is_finite(config->left_preamp_db) || config->left_preamp_db < -24.0f ||
      config->left_preamp_db > 0.0f ||
      !eq_is_finite(config->right_preamp_db) || config->right_preamp_db < -24.0f ||
      config->right_preamp_db > 0.0f ||
      config->left.filter_count > AUDIO_EQ_MAX_FILTERS_PER_CHANNEL ||
      config->right.filter_count > AUDIO_EQ_MAX_FILTERS_PER_CHANNEL) {
    return false;
  }

  const audio_eq_output_config_t *outputs[2] = {&config->left, &config->right};
  for (int ch = 0; ch < 2; ++ch) {
    const audio_eq_output_config_t *output = outputs[ch];
    for (uint8_t i = 0; i < output->filter_count; ++i) {
      const audio_eq_filter_config_t *f = &output->filters[i];
      if (f->enabled > 1 || f->type >= AUDIO_EQ_FILTER_COUNT ||
          !eq_is_finite(f->frequency_hz) || f->frequency_hz < 10.0f ||
          f->frequency_hz > 20000.0f || !eq_is_finite(f->gain_db) ||
          f->gain_db < -24.0f || f->gain_db > 24.0f || !eq_is_finite(f->q) ||
          f->q < 0.05f || f->q > 50.0f) {
        return false;
      }
    }
  }
  return true;
}

/* Exact version-2 NVS layout, before splitting the shared preamp. */
typedef struct {
  uint32_t version;
  uint8_t enabled;
  uint8_t channel_mode;
  uint16_t reserved;
  float preamp_db;
  audio_eq_output_config_t left;
  audio_eq_output_config_t right;
} audio_eq_config_v2_t;

static esp_err_t audio_eq_load_config(audio_eq_config_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  audio_eq_default_config(out);

  nvs_handle_t h;
  esp_err_t err = nvs_open(EQ_NS, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  union {
    audio_eq_config_t current;
    audio_eq_config_v2_t legacy;
  } saved;
  size_t len = sizeof(saved);
  err = nvs_get_blob(h, EQ_KEY, &saved, &len);
  nvs_close(h);

  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err == ESP_ERR_NVS_INVALID_LENGTH) {
    ESP_LOGW(TAG, "Ignoring unsupported EQ config size=%u", (unsigned)len);
    return ESP_OK;
  }
  if (err != ESP_OK) return err;
  if (len == sizeof(saved.current) &&
      audio_eq_validate_config(&saved.current)) {
    *out = saved.current;
  } else if (len == sizeof(saved.legacy) && saved.legacy.version == 2U) {
    /* Convert in RAM; the next explicit Save writes version 3. */
    audio_eq_default_config(out);
    out->enabled = saved.legacy.enabled;
    out->channel_mode = saved.legacy.channel_mode;
    out->left_preamp_db = saved.legacy.preamp_db;
    out->right_preamp_db = saved.legacy.preamp_db;
    out->left = saved.legacy.left;
    out->right = saved.legacy.right;
    if (!audio_eq_validate_config(out)) {
      audio_eq_default_config(out);
      ESP_LOGW(TAG, "Ignoring invalid version-2 EQ config");
    } else {
      ESP_LOGI(TAG, "Shared preamp migrated to both output channels");
    }
  } else {
    ESP_LOGW(TAG, "Ignoring invalid/old EQ config (size=%u)",
             (unsigned)len);
  }

  return ESP_OK;
}

esp_err_t audio_eq_save_config(const audio_eq_config_t *config) {
  if (!audio_eq_validate_config(config)) return ESP_ERR_INVALID_ARG;

  nvs_handle_t h;
  esp_err_t err = nvs_open(EQ_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(h, EQ_KEY, config, sizeof(*config));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

static bool calc_biquad(audio_eq_filter_type_t type, float frequency_hz,
                        float peak_gain_db, float q, int sample_rate,
                        biquad_coeff_t *out) {
  if (!out || sample_rate <= 0 || frequency_hz <= 0.0f ||
      frequency_hz >= ((float)sample_rate * 0.5f) || q <= 0.0f) {
    return false;
  }

  /* Bilinear-transform biquad equations as in Nigel Redmon's Biquad.h
   * (EarLevel Engineering). There Fc is pre-normalised as Hz / sample rate;
   * here it is normalised explicitly with the stream rate, so 44.1 and 48 kHz
   * are both handled correctly. */
  const double K = tan(M_PI * (double)frequency_hz / (double)sample_rate);
  const double V = pow(10.0, fabs((double)peak_gain_db) / 20.0);
  const double Q = (double)q;
  double norm = 1.0;
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

  switch (type) {
    case AUDIO_EQ_FILTER_LP:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = K * K * norm;
      b1 = 2.0 * b0;
      b2 = b0;
      a1 = 2.0 * (K * K - 1.0) * norm;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_HP:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = norm;
      b1 = -2.0 * b0;
      b2 = b0;
      a1 = 2.0 * (K * K - 1.0) * norm;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_BP:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = (K / Q) * norm;
      b1 = 0.0;
      b2 = -b0;
      a1 = 2.0 * (K * K - 1.0) * norm;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_NOTCH:
      norm = 1.0 / (1.0 + K / Q + K * K);
      b0 = (1.0 + K * K) * norm;
      b1 = 2.0 * (K * K - 1.0) * norm;
      b2 = b0;
      a1 = b1;
      a2 = (1.0 - K / Q + K * K) * norm;
      break;

    case AUDIO_EQ_FILTER_PK:
      if (peak_gain_db >= 0.0f) {
        norm = 1.0 / (1.0 + K / Q + K * K);
        b0 = (1.0 + V * K / Q + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - V * K / Q + K * K) * norm;
        a1 = b1;
        a2 = (1.0 - K / Q + K * K) * norm;
      } else {
        norm = 1.0 / (1.0 + V * K / Q + K * K);
        b0 = (1.0 + K / Q + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - K / Q + K * K) * norm;
        a1 = b1;
        a2 = (1.0 - V * K / Q + K * K) * norm;
      }
      break;

    case AUDIO_EQ_FILTER_LS:
      /* Fixed shelf slope as in that Biquad.h (Q is not used). */
      if (peak_gain_db >= 0.0f) {
        norm = 1.0 / (1.0 + sqrt(2.0) * K + K * K);
        b0 = (1.0 + sqrt(2.0 * V) * K + V * K * K) * norm;
        b1 = 2.0 * (V * K * K - 1.0) * norm;
        b2 = (1.0 - sqrt(2.0 * V) * K + V * K * K) * norm;
        a1 = 2.0 * (K * K - 1.0) * norm;
        a2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
      } else {
        norm = 1.0 / (1.0 + sqrt(2.0 * V) * K + V * K * K);
        b0 = (1.0 + sqrt(2.0) * K + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
        a1 = 2.0 * (V * K * K - 1.0) * norm;
        a2 = (1.0 - sqrt(2.0 * V) * K + V * K * K) * norm;
      }
      break;

    case AUDIO_EQ_FILTER_HS:
      /* Fixed shelf slope as in that Biquad.h (Q is not used). */
      if (peak_gain_db >= 0.0f) {
        norm = 1.0 / (1.0 + sqrt(2.0) * K + K * K);
        b0 = (V + sqrt(2.0 * V) * K + K * K) * norm;
        b1 = 2.0 * (K * K - V) * norm;
        b2 = (V - sqrt(2.0 * V) * K + K * K) * norm;
        a1 = 2.0 * (K * K - 1.0) * norm;
        a2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
      } else {
        norm = 1.0 / (V + sqrt(2.0 * V) * K + K * K);
        b0 = (1.0 + sqrt(2.0) * K + K * K) * norm;
        b1 = 2.0 * (K * K - 1.0) * norm;
        b2 = (1.0 - sqrt(2.0) * K + K * K) * norm;
        a1 = 2.0 * (K * K - V) * norm;
        a2 = (V - sqrt(2.0 * V) * K + K * K) * norm;
      }
      break;

    default:
      return false;
  }

  if (!isfinite(b0) || !isfinite(b1) || !isfinite(b2) || !isfinite(a1) ||
      !isfinite(a2)) {
    return false;
  }
  out->b0 = (float)b0;
  out->b1 = (float)b1;
  out->b2 = (float)b2;
  out->a1 = (float)a1;
  out->a2 = (float)a2;
  return true;
}

static void reset_state_unlocked(void) {
  memset(s_eq.state_l, 0, sizeof(s_eq.state_l));
  memset(s_eq.state_r, 0, sizeof(s_eq.state_r));
}

void audio_eq_reset_state(void) {
  eq_lock();
  reset_state_unlocked();
  eq_unlock();
}

static bool prepare_output(const audio_eq_output_config_t *cfg,
                           biquad_coeff_t *coeff, uint8_t *types,
                           uint8_t *slots,
                           uint8_t *active_count, int sample_rate,
                           const char *name) {
  uint8_t active = 0;
  for (uint8_t i = 0; i < cfg->filter_count; ++i) {
    const audio_eq_filter_config_t *f = &cfg->filters[i];
    if (!f->enabled) continue;

    if (f->frequency_hz >= ((float)sample_rate * 0.5f) ||
        !calc_biquad((audio_eq_filter_type_t)f->type, f->frequency_hz,
                     f->gain_db, f->q, sample_rate, &coeff[active])) {
      ESP_LOGE(TAG, "Invalid %s filter %u for %d Hz stream", name,
               (unsigned)(i + 1), sample_rate);
      return false;
    }
    types[active] = f->type;
    slots[active] = i;
    ++active;
  }
  *active_count = active;
  return true;
}

static bool prepare_for_rate_unlocked(int sample_rate) {
  if (sample_rate <= 0) return false;
  if (s_eq.ready && s_eq.sample_rate == sample_rate) return true;

  if (!prepare_output(&s_eq.config.left, s_eq.coeff_l, s_eq.type_l,
                      s_eq.slot_l, &s_eq.active_l, sample_rate, "left") ||
      !prepare_output(&s_eq.config.right, s_eq.coeff_r, s_eq.type_r,
                      s_eq.slot_r, &s_eq.active_r, sample_rate, "right")) {
    s_eq.ready = false;
    return false;
  }

  s_eq.preamp_gain_l = powf(10.0f, s_eq.config.left_preamp_db / 20.0f);
  s_eq.preamp_gain_r = powf(10.0f, s_eq.config.right_preamp_db / 20.0f);
  s_eq.sample_rate = sample_rate;
  s_eq.ready = true;
  reset_state_unlocked();
  AP_LOGI(TAG,
           "Ready: enabled=%u mode=%s left=%u/%u right=%u/%u preamp L=%.2fdB R=%.2fdB sr=%d",
           (unsigned)s_eq.config.enabled,
           audio_eq_channel_mode_name(
               (audio_eq_channel_mode_t)s_eq.config.channel_mode),
           (unsigned)s_eq.active_l,
           (unsigned)s_eq.config.left.filter_count,
           (unsigned)s_eq.active_r,
           (unsigned)s_eq.config.right.filter_count, s_eq.config.left_preamp_db, s_eq.config.right_preamp_db,
           sample_rate);
  return true;
}

esp_err_t audio_eq_get_active_config(audio_eq_config_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  eq_lock();
  *out = s_eq.config;
  eq_unlock();
  return ESP_OK;
}

esp_err_t audio_eq_apply_config(const audio_eq_config_t *config) {
  if (!audio_eq_validate_config(config)) return ESP_ERR_INVALID_ARG;

  int sample_rate = 0;
  eq_lock();
  sample_rate = s_eq.sample_rate;
  eq_unlock();

  biquad_coeff_t coeff_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL] = {0};
  biquad_coeff_t coeff_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL] = {0};
  uint8_t type_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL] = {0};
  uint8_t type_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL] = {0};
  uint8_t slot_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL] = {0};
  uint8_t slot_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL] = {0};
  uint8_t active_l = 0;
  uint8_t active_r = 0;
  const float preamp_gain_l = powf(10.0f, config->left_preamp_db / 20.0f);
  const float preamp_gain_r = powf(10.0f, config->right_preamp_db / 20.0f);
  bool prepared = false;

  if (sample_rate > 0) {
    if (!prepare_output(&config->left, coeff_l, type_l, slot_l, &active_l,
                        sample_rate, "left") ||
        !prepare_output(&config->right, coeff_r, type_r, slot_r, &active_r,
                        sample_rate, "right")) {
      return ESP_ERR_INVALID_ARG;
    }
    prepared = true;
  }

  eq_lock();
  const bool was_ready = s_eq.ready;
  const bool was_enabled = s_eq.config.enabled != 0U;
  const bool mode_changed = s_eq.config.channel_mode != config->channel_mode;
  const uint8_t old_active_l = s_eq.active_l;
  const uint8_t old_active_r = s_eq.active_r;
  uint8_t old_type_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t old_type_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t old_slot_l[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t old_slot_r[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  memcpy(old_type_l, s_eq.type_l, sizeof(old_type_l));
  memcpy(old_type_r, s_eq.type_r, sizeof(old_type_r));
  memcpy(old_slot_l, s_eq.slot_l, sizeof(old_slot_l));
  memcpy(old_slot_r, s_eq.slot_r, sizeof(old_slot_r));

  s_eq.config = *config;
  if (prepared && s_eq.sample_rate == sample_rate) {
    memcpy(s_eq.coeff_l, coeff_l, sizeof(coeff_l));
    memcpy(s_eq.coeff_r, coeff_r, sizeof(coeff_r));
    memcpy(s_eq.type_l, type_l, sizeof(type_l));
    memcpy(s_eq.type_r, type_r, sizeof(type_r));
    memcpy(s_eq.slot_l, slot_l, sizeof(slot_l));
    memcpy(s_eq.slot_r, slot_r, sizeof(slot_r));
    s_eq.active_l = active_l;
    s_eq.active_r = active_r;
    s_eq.preamp_gain_l = preamp_gain_l;
    s_eq.preamp_gain_r = preamp_gain_r;
    s_eq.ready = true;

    if (!was_ready || !was_enabled || !config->enabled || mode_changed) {
      /* The filters were not running (or the input routing changed), so the
       * old delay lines are stale: start from silence. */
      reset_state_unlocked();
    } else {
      /* Live tweak while audio is playing. Zeroing the delay lines here would
       * inject a step into the signal (an audible click on every slider
       * move). A biquad in transposed direct form II tolerates coefficient
       * changes, so retain a state only when its configured slot and type
       * still occupy the same active position. Enabling/disabling an earlier
       * filter shifts this position and changes its cascade input. */
      for (uint8_t i = 0; i < AUDIO_EQ_MAX_FILTERS_PER_CHANNEL; ++i) {
        if (i >= active_l || i >= old_active_l || type_l[i] != old_type_l[i] ||
            slot_l[i] != old_slot_l[i])
          memset(&s_eq.state_l[i], 0, sizeof(s_eq.state_l[i]));
        if (i >= active_r || i >= old_active_r || type_r[i] != old_type_r[i] ||
            slot_r[i] != old_slot_r[i])
          memset(&s_eq.state_r[i], 0, sizeof(s_eq.state_r[i]));
      }
    }
  } else {
    /* A stream-rate transition raced the HTTP request, or no stream has run
     * yet. Rebuild from the new config on the next decoded PCM block. */
    s_eq.ready = false;
    reset_state_unlocked();
  }
  eq_unlock();

  AP_LOGI(TAG,
           "Live apply: enabled=%u mode=%s left=%u right=%u preamp L=%.2fdB R=%.2fdB%s",
           (unsigned)config->enabled,
           audio_eq_channel_mode_name(
               (audio_eq_channel_mode_t)config->channel_mode),
           (unsigned)config->left.filter_count,
           (unsigned)config->right.filter_count, config->left_preamp_db, config->right_preamp_db,
           prepared ? "" : " (coefficients deferred until audio)" );
  return ESP_OK;
}

esp_err_t audio_eq_init(void) {
  if (!s_eq_mutex) {
    s_eq_mutex = xSemaphoreCreateMutex();
    if (!s_eq_mutex) return ESP_ERR_NO_MEM;
  }
  memset(&s_eq, 0, sizeof(s_eq));
  esp_err_t err = audio_eq_load_config(&s_eq.config);
  if (err != ESP_OK) {
    audio_eq_default_config(&s_eq.config);
    ESP_LOGW(TAG, "EQ config load failed: %s; using defaults",
             esp_err_to_name(err));
    return ESP_OK;
  }
  ESP_LOGI(TAG,
           "Config loaded: enabled=%u mode=%s left=%u right=%u preamp L=%.2fdB R=%.2fdB",
           (unsigned)s_eq.config.enabled,
           audio_eq_channel_mode_name(
               (audio_eq_channel_mode_t)s_eq.config.channel_mode),
           (unsigned)s_eq.config.left.filter_count,
           (unsigned)s_eq.config.right.filter_count, s_eq.config.left_preamp_db, s_eq.config.right_preamp_db);
  return ESP_OK;
}

static inline float biquad_process(float x, const biquad_coeff_t *c,
                                   biquad_state_t *state) {
  const float y = c->b0 * x + state->z1;
  state->z1 = c->b1 * x - c->a1 * y + state->z2;
  state->z2 = c->b2 * x - c->a2 * y;
  return y;
}

static inline float process_chain_hot(float x, const biquad_coeff_t *coeff,
                                      biquad_state_t *states,
                                      uint8_t active_count, float preamp_gain) {
  x *= preamp_gain;
  for (uint8_t i = 0; i < active_count; ++i) {
    x = biquad_process(x, &coeff[i], &states[i]);
  }
  return x;
}

/* The EQ works on float PCM (1.0 = full scale, see audio_out_sample.h) and
 * writes float back: nothing is clipped or requantised here. A boost above
 * full scale simply stays above 1.0 until the single saturation right before
 * DMA, so lowering the volume brings it back without distortion. */

/* After digital silence the delay lines decay toward denormal floats, which
 * the FPU handles slowly. Clear values far below the 32-bit LSB (2^-31 of full
 * scale ~ 5e-10) once per block. */
static void flush_tiny_state(biquad_state_t *states, uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (fabsf(states[i].z1) < 1e-20f) states[i].z1 = 0.0f;
    if (fabsf(states[i].z2) < 1e-20f) states[i].z2 = 0.0f;
  }
}

static inline void process_stereo_eq(float *pcm, size_t frames) {
  for (size_t i = 0; i < frames; ++i) {
    const size_t p = i * 2;
    pcm[p] = process_chain_hot(pcm[p], s_eq.coeff_l, s_eq.state_l,
                               s_eq.active_l, s_eq.preamp_gain_l);
    pcm[p + 1] = process_chain_hot(pcm[p + 1], s_eq.coeff_r, s_eq.state_r,
                                   s_eq.active_r, s_eq.preamp_gain_r);
  }
}

/* One source feeds both independent chains (mono mix, left or right). */
static inline void process_source_eq(float *pcm, size_t frames,
                                     audio_eq_channel_mode_t mode) {
  for (size_t i = 0; i < frames; ++i) {
    const size_t p = i * 2;
    const float source = mode == AUDIO_EQ_CHANNEL_MONO
                             ? 0.5f * (pcm[p] + pcm[p + 1])
                         : mode == AUDIO_EQ_CHANNEL_LEFT ? pcm[p] : pcm[p + 1];
    pcm[p] = process_chain_hot(source, s_eq.coeff_l, s_eq.state_l,
                               s_eq.active_l, s_eq.preamp_gain_l);
    pcm[p + 1] = process_chain_hot(source, s_eq.coeff_r, s_eq.state_r,
                                   s_eq.active_r, s_eq.preamp_gain_r);
  }
}

/* EQ disabled: only the source selection. The mono mix of two float samples
 * is exact (no rounding back to 16 bits any more). */
static inline void process_source_bypass(float *pcm, size_t frames,
                                         audio_eq_channel_mode_t mode) {
  for (size_t i = 0; i < frames; ++i) {
    const size_t p = i * 2;
    const float source = mode == AUDIO_EQ_CHANNEL_MONO
                             ? 0.5f * (pcm[p] + pcm[p + 1])
                         : mode == AUDIO_EQ_CHANNEL_LEFT ? pcm[p] : pcm[p + 1];
    pcm[p] = source;
    pcm[p + 1] = source;
  }
}

void audio_eq_prepare(int sample_rate) {
  eq_lock();
  (void)prepare_for_rate_unlocked(sample_rate);
  eq_unlock();
}

void audio_eq_process(float *pcm, size_t frames, int channels,
                      int sample_rate) {
  if (!pcm || frames == 0 || channels != 2) return;

  eq_lock();
  if (!prepare_for_rate_unlocked(sample_rate)) {
    eq_unlock();
    return;
  }

  const audio_eq_channel_mode_t mode =
      (audio_eq_channel_mode_t)s_eq.config.channel_mode;

  if (!s_eq.config.enabled) {
    if (mode != AUDIO_EQ_CHANNEL_STEREO) process_source_bypass(pcm, frames, mode);
    eq_unlock();
    return;
  }

  if (mode == AUDIO_EQ_CHANNEL_STEREO && s_eq.active_l == 0U &&
      s_eq.active_r == 0U && s_eq.preamp_gain_l == 1.0f &&
      s_eq.preamp_gain_r == 1.0f) {
    /* Neutral chain: exact pass-through, filter state untouched. */
    eq_unlock();
    return;
  }

  if (mode == AUDIO_EQ_CHANNEL_STEREO) {
    process_stereo_eq(pcm, frames);
  } else {
    process_source_eq(pcm, frames, mode);
  }
  flush_tiny_state(s_eq.state_l, s_eq.active_l);
  flush_tiny_state(s_eq.state_r, s_eq.active_r);
  eq_unlock();
}


const char *audio_eq_filter_type_name(audio_eq_filter_type_t type) {
  switch (type) {
    case AUDIO_EQ_FILTER_PK: return "PK";
    case AUDIO_EQ_FILTER_LP: return "LP";
    case AUDIO_EQ_FILTER_HP: return "HP";
    case AUDIO_EQ_FILTER_BP: return "BP";
    case AUDIO_EQ_FILTER_NOTCH: return "NOTCH";
    case AUDIO_EQ_FILTER_LS: return "LS";
    case AUDIO_EQ_FILTER_HS: return "HS";
    default: return "PK";
  }
}

bool audio_eq_filter_type_from_name(const char *name,
                                    audio_eq_filter_type_t *out) {
  if (!name || !out) return false;
  for (int i = 0; i < AUDIO_EQ_FILTER_COUNT; ++i) {
    if (strcasecmp(name, audio_eq_filter_type_name((audio_eq_filter_type_t)i)) == 0) {
      *out = (audio_eq_filter_type_t)i;
      return true;
    }
  }
  return false;
}

const char *audio_eq_channel_mode_name(audio_eq_channel_mode_t mode) {
  switch (mode) {
    case AUDIO_EQ_CHANNEL_STEREO: return "stereo";
    case AUDIO_EQ_CHANNEL_MONO: return "mono";
    case AUDIO_EQ_CHANNEL_LEFT: return "left";
    case AUDIO_EQ_CHANNEL_RIGHT: return "right";
    default: return "stereo";
  }
}

bool audio_eq_channel_mode_from_name(const char *name,
                                     audio_eq_channel_mode_t *out) {
  if (!name || !out) return false;
  for (int i = 0; i < AUDIO_EQ_CHANNEL_COUNT; ++i) {
    if (strcasecmp(name, audio_eq_channel_mode_name((audio_eq_channel_mode_t)i)) == 0) {
      *out = (audio_eq_channel_mode_t)i;
      return true;
    }
  }
  return false;
}
