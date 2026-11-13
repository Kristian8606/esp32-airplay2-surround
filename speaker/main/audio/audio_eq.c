#include "audio_eq.h"

#include <math.h>
#include <stdlib.h>
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

/* Linkwitz-Riley crossover: LR2 = one 2nd-order section with Q 0.5 (two
 * 1st-order Butterworths), LR4 = 2nd-order Butterworth twice, LR8 =
 * 4th-order Butterworth (Q 0.5412 and 1.3066) twice. */
#define XOVER_MAX_SECTIONS 4U
/* Delay line per output: a power of two above 2 ms at 48 kHz (96). */
#define DELAY_LEN  128U
#define DELAY_MASK (DELAY_LEN - 1U)
/* Output layout switch: fade out, silence, fade in (see audio_eq.h). */
#define SWITCH_FADE_OUT_MS 5U
#define SWITCH_HUSH_MS     20U
#define SWITCH_FADE_IN_MS  10U

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

/* Everything calculated from one configuration at one sample rate: what
 * audio_eq_process() runs. Index 0 = left output, 1 = right output. */
typedef struct {
  audio_eq_config_t config;
  biquad_coeff_t coeff[2][AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  /* Filter identity at each ACTIVE position, so a live config change can tell
   * which delay-line states are still meaningful. */
  uint8_t type[2][AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t slot[2][AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  uint8_t active[2];
  float preamp_gain[2];
  /* Bi-amp: high-pass on the tweeter output, low-pass on the other. */
  biquad_coeff_t xover[2][XOVER_MAX_SECTIONS];
  uint8_t xover_sections;
  uint32_t delay_frames[2];
  int sample_rate;
} eq_plan_t;

typedef enum {
  SWITCH_NONE = 0,
  SWITCH_FADE_OUT, /* old layout, gain down to 0 */
  SWITCH_HUSH,     /* new layout installed, silent */
  SWITCH_FADE_IN,  /* new layout, gain up to 1 */
} eq_switch_phase_t;

typedef struct {
  eq_plan_t plan; /* what plays */
  bool ready;     /* plan is built for plan.sample_rate */
  biquad_state_t state[2][AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
  biquad_state_t xover_state[2][XOVER_MAX_SECTIONS];
  float delay_line[2][DELAY_LEN];
  uint32_t delay_pos;
  /* A new output layout waits here until the old one has faded out. */
  eq_plan_t next;
  bool next_pending;
  uint8_t switch_phase; /* eq_switch_phase_t */
  float switch_gain;
  uint32_t hush_left;
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

static void default_outputs(audio_eq_config_t *out) {
  out->output_mode = AUDIO_EQ_OUTPUT_MONO;
  out->tweeter_output = 0U;
  out->xover_slope_db = AUDIO_EQ_XOVER_DEFAULT_DB;
  out->invert_mask = 0U;
  out->xover_hz = AUDIO_EQ_XOVER_DEFAULT_HZ;
  out->left_delay_ms = 0.0f;
  out->right_delay_ms = 0.0f;
}

void audio_eq_default_config(audio_eq_config_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->version = AUDIO_EQ_CONFIG_VERSION;
  out->enabled = 0;
  out->channel_mode = AUDIO_EQ_CHANNEL_STEREO;
  out->left_preamp_db = 0.0f;
  out->right_preamp_db = 0.0f;
  default_outputs(out);
}

static bool delay_valid(float ms) {
  return eq_is_finite(ms) && ms >= 0.0f && ms <= AUDIO_EQ_DELAY_MAX_MS;
}

bool audio_eq_validate_config(const audio_eq_config_t *config) {
  if (!config || config->version != AUDIO_EQ_CONFIG_VERSION ||
      config->enabled > 1 || config->channel_mode >= AUDIO_EQ_CHANNEL_COUNT ||
      !eq_is_finite(config->left_preamp_db) || config->left_preamp_db < -24.0f ||
      config->left_preamp_db > 0.0f ||
      !eq_is_finite(config->right_preamp_db) || config->right_preamp_db < -24.0f ||
      config->right_preamp_db > 0.0f ||
      config->left.filter_count > AUDIO_EQ_MAX_FILTERS_PER_CHANNEL ||
      config->right.filter_count > AUDIO_EQ_MAX_FILTERS_PER_CHANNEL ||
      config->output_mode >= AUDIO_EQ_OUTPUT_COUNT ||
      config->tweeter_output > 1U ||
      (config->xover_slope_db != 12U && config->xover_slope_db != 24U &&
       config->xover_slope_db != 48U) ||
      config->invert_mask > 3U || !eq_is_finite(config->xover_hz) ||
      config->xover_hz < AUDIO_EQ_XOVER_MIN_HZ ||
      config->xover_hz > AUDIO_EQ_XOVER_MAX_HZ ||
      !delay_valid(config->left_delay_ms) ||
      !delay_valid(config->right_delay_ms)) {
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

/* Exact version-3 NVS layout, before the amplifier outputs (bi-amp). */
typedef struct {
  uint32_t version;
  uint8_t enabled;
  uint8_t channel_mode;
  uint16_t reserved;
  float left_preamp_db;
  float right_preamp_db;
  audio_eq_output_config_t left;
  audio_eq_output_config_t right;
} audio_eq_config_v3_t;

static esp_err_t audio_eq_load_config(audio_eq_config_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  audio_eq_default_config(out);

  nvs_handle_t h;
  esp_err_t err = nvs_open(EQ_NS, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  union {
    audio_eq_config_t current;
    audio_eq_config_v3_t v3;
    audio_eq_config_v2_t v2;
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
  } else if (len == sizeof(saved.v3) && saved.v3.version == 3U) {
    /* Convert in RAM; the next explicit Save writes version 4. Both
     * outputs keep playing the full range (mono), as before. */
    audio_eq_default_config(out);
    out->enabled = saved.v3.enabled;
    out->channel_mode = saved.v3.channel_mode;
    out->left_preamp_db = saved.v3.left_preamp_db;
    out->right_preamp_db = saved.v3.right_preamp_db;
    out->left = saved.v3.left;
    out->right = saved.v3.right;
    if (!audio_eq_validate_config(out)) {
      audio_eq_default_config(out);
      ESP_LOGW(TAG, "Ignoring invalid version-3 EQ config");
    } else {
      ESP_LOGI(TAG, "Version-3 EQ config loaded: outputs mono (full range)");
    }
  } else if (len == sizeof(saved.v2) && saved.v2.version == 2U) {
    /* Convert in RAM; the next explicit Save writes version 4. */
    audio_eq_default_config(out);
    out->enabled = saved.v2.enabled;
    out->channel_mode = saved.v2.channel_mode;
    out->left_preamp_db = saved.v2.preamp_db;
    out->right_preamp_db = saved.v2.preamp_db;
    out->left = saved.v2.left;
    out->right = saved.v2.right;
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

static void reset_eq_state_unlocked(void) {
  memset(s_eq.state, 0, sizeof(s_eq.state));
}

/* Every filter and delay line back to silence (the switch envelope is not
 * touched). */
static void reset_state_unlocked(void) {
  reset_eq_state_unlocked();
  memset(s_eq.xover_state, 0, sizeof(s_eq.xover_state));
  memset(s_eq.delay_line, 0, sizeof(s_eq.delay_line));
  s_eq.delay_pos = 0U;
}

/* The waiting output layout becomes the one that plays, from silence. */
static void install_next_unlocked(void) {
  s_eq.plan = s_eq.next;
  s_eq.next_pending = false;
  s_eq.ready = true;
  reset_state_unlocked();
}

void audio_eq_reset_state(void) {
  eq_lock();
  /* A real stop: nothing is playing, so a waiting layout needs no fade. */
  if (s_eq.next_pending) install_next_unlocked();
  s_eq.switch_phase = SWITCH_NONE;
  s_eq.switch_gain = 1.0f;
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

static uint32_t delay_frames(float ms, int sample_rate) {
  if (ms <= 0.0f || sample_rate <= 0) return 0U;
  const double n = floor((double)ms * (double)sample_rate / 1000.0 + 0.5);
  return n >= (double)DELAY_LEN ? DELAY_MASK : (uint32_t)n;
}

/* Calculate everything for one configuration at one sample rate. False (and
 * nothing to play) when a filter is not possible at this rate. */
static bool build_plan(const audio_eq_config_t *config, int sample_rate,
                       eq_plan_t *plan) {
  memset(plan, 0, sizeof(*plan));
  plan->config = *config;
  if (sample_rate <= 0) return false;
  if (!prepare_output(&config->left, plan->coeff[0], plan->type[0],
                      plan->slot[0], &plan->active[0], sample_rate, "left") ||
      !prepare_output(&config->right, plan->coeff[1], plan->type[1],
                      plan->slot[1], &plan->active[1], sample_rate, "right")) {
    return false;
  }
  plan->preamp_gain[0] = powf(10.0f, config->left_preamp_db / 20.0f);
  plan->preamp_gain[1] = powf(10.0f, config->right_preamp_db / 20.0f);

  if (config->output_mode == AUDIO_EQ_OUTPUT_BIAMP) {
    static const float lr2[] = {0.5f};
    static const float lr4[] = {0.70710678f, 0.70710678f};
    static const float lr8[] = {0.54119610f, 1.30656296f, 0.54119610f,
                                1.30656296f};
    const float *q = config->xover_slope_db == 12U   ? lr2
                     : config->xover_slope_db == 48U ? lr8
                                                     : lr4;
    const uint8_t n = config->xover_slope_db == 12U   ? 1U
                      : config->xover_slope_db == 48U ? 4U
                                                      : 2U;
    for (int ch = 0; ch < 2; ++ch) {
      const audio_eq_filter_type_t type = ch == (int)config->tweeter_output
                                              ? AUDIO_EQ_FILTER_HP
                                              : AUDIO_EQ_FILTER_LP;
      for (uint8_t k = 0; k < n; ++k) {
        if (!calc_biquad(type, config->xover_hz, 0.0f, q[k], sample_rate,
                         &plan->xover[ch][k])) {
          ESP_LOGE(TAG, "Invalid crossover for %d Hz stream", sample_rate);
          return false;
        }
      }
    }
    plan->xover_sections = n;
    plan->delay_frames[0] = delay_frames(config->left_delay_ms, sample_rate);
    plan->delay_frames[1] = delay_frames(config->right_delay_ms, sample_rate);
  }
  plan->sample_rate = sample_rate;
  return true;
}

/* A change that cannot be made under running audio without a step: it goes
 * through the fade out / silence / fade in switch. A new crossover frequency
 * alone does not (the filters keep their state, as EQ tweaks do). */
static bool layout_differs(const audio_eq_config_t *a,
                           const audio_eq_config_t *b) {
  if (a->output_mode != b->output_mode) return true;
  if (a->output_mode != AUDIO_EQ_OUTPUT_BIAMP) return false;
  return a->tweeter_output != b->tweeter_output ||
         a->xover_slope_db != b->xover_slope_db ||
         a->invert_mask != b->invert_mask ||
         a->left_delay_ms != b->left_delay_ms ||
         a->right_delay_ms != b->right_delay_ms;
}

static void log_layout(const char *what, const audio_eq_config_t *c) {
  if (c->output_mode != AUDIO_EQ_OUTPUT_BIAMP) {
    AP_LOGI(TAG, "%s: outputs mono (full range on both)", what);
    return;
  }
  AP_LOGI(TAG,
          "%s: outputs bi-amp, tweeter on %s, crossover %.0f Hz LR%u, "
          "invert L=%u R=%u, delay L=%.2f R=%.2f ms",
          what, c->tweeter_output ? "right" : "left", (double)c->xover_hz,
          (unsigned)(c->xover_slope_db / 6U), (unsigned)(c->invert_mask & 1U),
          (unsigned)((c->invert_mask >> 1) & 1U), (double)c->left_delay_ms,
          (double)c->right_delay_ms);
}

static bool prepare_for_rate_unlocked(int sample_rate) {
  if (sample_rate <= 0) return false;
  if (s_eq.ready && s_eq.plan.sample_rate == sample_rate) return true;

  /* A new rate means a new stream (it starts with a fade-in of its own): a
   * waiting layout is simply taken now. */
  if (s_eq.next_pending) {
    s_eq.plan.config = s_eq.next.config;
    s_eq.next_pending = false;
  }
  s_eq.switch_phase = SWITCH_NONE;
  s_eq.switch_gain = 1.0f;

  const audio_eq_config_t config = s_eq.plan.config;
  if (!build_plan(&config, sample_rate, &s_eq.plan)) {
    s_eq.ready = false;
    return false;
  }
  s_eq.ready = true;
  reset_state_unlocked();
  AP_LOGI(TAG,
           "Ready: enabled=%u mode=%s left=%u/%u right=%u/%u preamp L=%.2fdB R=%.2fdB sr=%d",
           (unsigned)config.enabled,
           audio_eq_channel_mode_name(
               (audio_eq_channel_mode_t)config.channel_mode),
           (unsigned)s_eq.plan.active[0],
           (unsigned)config.left.filter_count,
           (unsigned)s_eq.plan.active[1],
           (unsigned)config.right.filter_count, config.left_preamp_db,
           config.right_preamp_db, sample_rate);
  log_layout("Ready", &config);
  return true;
}

esp_err_t audio_eq_get_active_config(audio_eq_config_t *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  eq_lock();
  *out = s_eq.next_pending ? s_eq.next.config : s_eq.plan.config;
  eq_unlock();
  return ESP_OK;
}

int audio_eq_tweeter_output(void) {
  eq_lock();
  int out = -1;
  if (s_eq.plan.config.output_mode == AUDIO_EQ_OUTPUT_BIAMP)
    out = s_eq.plan.config.tweeter_output;
  else if (s_eq.next_pending &&
           s_eq.next.config.output_mode == AUDIO_EQ_OUTPUT_BIAMP)
    out = s_eq.next.config.tweeter_output;
  eq_unlock();
  return out;
}

esp_err_t audio_eq_apply_config(const audio_eq_config_t *config) {
  if (!audio_eq_validate_config(config)) return ESP_ERR_INVALID_ARG;

  int sample_rate = 0;
  eq_lock();
  sample_rate = s_eq.plan.sample_rate;
  eq_unlock();

  /* Calculated outside the lock (the playout task keeps running) and on the
   * heap (a plan is ~2 KB, too much for the HTTP task's stack). */
  eq_plan_t *built = NULL;
  if (sample_rate > 0) {
    built = malloc(sizeof(*built));
    if (!built) return ESP_ERR_NO_MEM;
    if (!build_plan(config, sample_rate, built)) {
      free(built);
      return ESP_ERR_INVALID_ARG;
    }
  }

  const char *how = "";
  eq_lock();
  if (!built || s_eq.plan.sample_rate != sample_rate) {
    /* A stream-rate transition raced the HTTP request, or no stream has run
     * yet. Rebuild from the new config on the next decoded PCM block. */
    s_eq.plan.config = *config;
    s_eq.ready = false;
    s_eq.next_pending = false;
    s_eq.switch_phase = SWITCH_NONE;
    s_eq.switch_gain = 1.0f;
    reset_state_unlocked();
    how = " (coefficients deferred until audio)";
  } else if (s_eq.next_pending ||
             (s_eq.ready && layout_differs(&s_eq.plan.config, config))) {
    /* New output layout under running audio: it waits until the old one
     * has faded out (audio_eq_process). A config sent while one waits
     * replaces it: it is the complete new state. */
    s_eq.next = *built;
    s_eq.next_pending = true;
    if (s_eq.switch_phase == SWITCH_NONE || s_eq.switch_phase == SWITCH_FADE_IN)
      s_eq.switch_phase = SWITCH_FADE_OUT;
    how = " (output layout: switching with a short mute)";
  } else {
    const bool was_ready = s_eq.ready;
    const bool was_enabled = s_eq.plan.config.enabled != 0U;
    const bool mode_changed =
        s_eq.plan.config.channel_mode != config->channel_mode;
    uint8_t old_active[2];
    uint8_t old_type[2][AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
    uint8_t old_slot[2][AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
    memcpy(old_active, s_eq.plan.active, sizeof(old_active));
    memcpy(old_type, s_eq.plan.type, sizeof(old_type));
    memcpy(old_slot, s_eq.plan.slot, sizeof(old_slot));

    s_eq.plan = *built;
    s_eq.ready = true;

    if (!was_ready) {
      reset_state_unlocked();
    } else if (!was_enabled || !config->enabled || mode_changed) {
      /* The filters were not running (or the input routing changed), so the
       * old delay lines are stale: start from silence. The crossover runs
       * whether the EQ is on or not: it keeps its state. */
      reset_eq_state_unlocked();
    } else {
      /* Live tweak while audio is playing. Zeroing the delay lines here would
       * inject a step into the signal (an audible click on every slider
       * move). A biquad in transposed direct form II tolerates coefficient
       * changes, so retain a state only when its configured slot and type
       * still occupy the same active position. Enabling/disabling an earlier
       * filter shifts this position and changes its cascade input. The
       * crossover (same layout, maybe a new frequency) keeps its state. */
      for (int ch = 0; ch < 2; ++ch) {
        for (uint8_t i = 0; i < AUDIO_EQ_MAX_FILTERS_PER_CHANNEL; ++i) {
          if (i >= s_eq.plan.active[ch] || i >= old_active[ch] ||
              s_eq.plan.type[ch][i] != old_type[ch][i] ||
              s_eq.plan.slot[ch][i] != old_slot[ch][i])
            memset(&s_eq.state[ch][i], 0, sizeof(s_eq.state[ch][i]));
        }
      }
    }
  }
  eq_unlock();
  free(built);

  AP_LOGI(TAG,
           "Live apply: enabled=%u mode=%s left=%u right=%u preamp L=%.2fdB R=%.2fdB%s",
           (unsigned)config->enabled,
           audio_eq_channel_mode_name(
               (audio_eq_channel_mode_t)config->channel_mode),
           (unsigned)config->left.filter_count,
           (unsigned)config->right.filter_count, config->left_preamp_db,
           config->right_preamp_db, how);
  log_layout("Live apply", config);
  return ESP_OK;
}

esp_err_t audio_eq_init(void) {
  if (!s_eq_mutex) {
    s_eq_mutex = xSemaphoreCreateMutex();
    if (!s_eq_mutex) return ESP_ERR_NO_MEM;
  }
  memset(&s_eq, 0, sizeof(s_eq));
  s_eq.switch_gain = 1.0f;
  esp_err_t err = audio_eq_load_config(&s_eq.plan.config);
  if (err != ESP_OK) {
    audio_eq_default_config(&s_eq.plan.config);
    ESP_LOGW(TAG, "EQ config load failed: %s; using defaults",
             esp_err_to_name(err));
    return ESP_OK;
  }
  const audio_eq_config_t *c = &s_eq.plan.config;
  ESP_LOGI(TAG,
           "Config loaded: enabled=%u mode=%s left=%u right=%u preamp L=%.2fdB R=%.2fdB",
           (unsigned)c->enabled,
           audio_eq_channel_mode_name((audio_eq_channel_mode_t)c->channel_mode),
           (unsigned)c->left.filter_count, (unsigned)c->right.filter_count,
           c->left_preamp_db, c->right_preamp_db);
  log_layout("Config loaded", c);
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
  const eq_plan_t *p = &s_eq.plan;
  for (size_t i = 0; i < frames; ++i) {
    const size_t k = i * 2;
    pcm[k] = process_chain_hot(pcm[k], p->coeff[0], s_eq.state[0],
                               p->active[0], p->preamp_gain[0]);
    pcm[k + 1] = process_chain_hot(pcm[k + 1], p->coeff[1], s_eq.state[1],
                                   p->active[1], p->preamp_gain[1]);
  }
}

static inline float source_of(const float *frame,
                              audio_eq_channel_mode_t mode) {
  return mode == AUDIO_EQ_CHANNEL_MONO   ? 0.5f * (frame[0] + frame[1])
         : mode == AUDIO_EQ_CHANNEL_LEFT ? frame[0]
                                         : frame[1];
}

/* One source feeds both independent chains (mono mix, left or right). */
static inline void process_source_eq(float *pcm, size_t frames,
                                     audio_eq_channel_mode_t mode) {
  const eq_plan_t *p = &s_eq.plan;
  for (size_t i = 0; i < frames; ++i) {
    const size_t k = i * 2;
    const float source = source_of(&pcm[k], mode);
    pcm[k] = process_chain_hot(source, p->coeff[0], s_eq.state[0],
                               p->active[0], p->preamp_gain[0]);
    pcm[k + 1] = process_chain_hot(source, p->coeff[1], s_eq.state[1],
                                   p->active[1], p->preamp_gain[1]);
  }
}

/* EQ disabled: only the source selection. The mono mix of two float samples
 * is exact (no rounding back to 16 bits any more). */
static inline void process_source_bypass(float *pcm, size_t frames,
                                         audio_eq_channel_mode_t mode) {
  for (size_t i = 0; i < frames; ++i) {
    const size_t k = i * 2;
    const float source = source_of(&pcm[k], mode);
    pcm[k] = source;
    pcm[k + 1] = source;
  }
}

/* Bi-amp, on each output after its EQ: crossover, polarity, delay. One
 * output at a time, so its sections and state stay in registers. */
static void process_biamp(float *pcm, size_t frames) {
  const eq_plan_t *p = &s_eq.plan;
  const uint8_t n = p->xover_sections;
  for (int ch = 0; ch < 2; ++ch) {
    const biquad_coeff_t *c = p->xover[ch];
    biquad_state_t *st = s_eq.xover_state[ch];
    const bool invert = ((p->config.invert_mask >> ch) & 1U) != 0U;
    float *line = s_eq.delay_line[ch];
    const uint32_t d = p->delay_frames[ch];
    uint32_t w = s_eq.delay_pos;
    for (size_t i = 0; i < frames; ++i) {
      float x = pcm[i * 2 + (size_t)ch];
      for (uint8_t k = 0; k < n; ++k) x = biquad_process(x, &c[k], &st[k]);
      if (invert) x = -x;
      line[w] = x;
      pcm[i * 2 + (size_t)ch] = line[(w - d) & DELAY_MASK];
      w = (w + 1U) & DELAY_MASK;
    }
    flush_tiny_state(st, n);
  }
  s_eq.delay_pos = (s_eq.delay_pos + (uint32_t)frames) & DELAY_MASK;
}

/* The output layout switch envelope over this block; at the end of the
 * block that reached silence the waiting layout is installed, from silence:
 * the old one never plays with the new filters and the tweeter output never
 * plays anything that missed its high-pass. */
static void process_switch(float *pcm, size_t frames, int sample_rate) {
  const float out_step = 1000.0f / ((float)SWITCH_FADE_OUT_MS * (float)sample_rate);
  const float in_step = 1000.0f / ((float)SWITCH_FADE_IN_MS * (float)sample_rate);
  uint8_t phase = s_eq.switch_phase;
  float g = s_eq.switch_gain;
  for (size_t i = 0; i < frames; ++i) {
    switch (phase) {
      case SWITCH_FADE_OUT:
        g -= out_step;
        if (g < 0.0f) g = 0.0f;
        break;
      case SWITCH_HUSH:
        g = 0.0f;
        if (s_eq.hush_left > 0U) --s_eq.hush_left;
        else phase = SWITCH_FADE_IN;
        break;
      case SWITCH_FADE_IN:
        g += in_step;
        if (g >= 1.0f) {
          g = 1.0f;
          phase = SWITCH_NONE;
        }
        break;
      default:
        g = 1.0f;
        break;
    }
    pcm[i * 2] *= g;
    pcm[i * 2 + 1] *= g;
  }
  if ((phase == SWITCH_FADE_OUT && g == 0.0f) ||
      (phase == SWITCH_HUSH && s_eq.next_pending)) {
    if (s_eq.next_pending) install_next_unlocked();
    phase = SWITCH_HUSH;
    s_eq.hush_left = (uint32_t)((uint64_t)SWITCH_HUSH_MS * (uint32_t)sample_rate / 1000U);
  }
  s_eq.switch_phase = phase;
  s_eq.switch_gain = g;
}

/* Nothing could be prepared for this rate: with bi-amp the tweeter output
 * (of the layout playing or the one waiting) gets silence, never the
 * unfiltered full range. */
static void guard_tweeter(float *pcm, size_t frames) {
  int mask = 0;
  if (s_eq.plan.config.output_mode == AUDIO_EQ_OUTPUT_BIAMP)
    mask |= 1 << s_eq.plan.config.tweeter_output;
  if (s_eq.next_pending &&
      s_eq.next.config.output_mode == AUDIO_EQ_OUTPUT_BIAMP)
    mask |= 1 << s_eq.next.config.tweeter_output;
  if (!mask) return;
  for (size_t i = 0; i < frames; ++i) {
    if (mask & 1) pcm[i * 2] = 0.0f;
    if (mask & 2) pcm[i * 2 + 1] = 0.0f;
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
    guard_tweeter(pcm, frames);
    eq_unlock();
    return;
  }

  const eq_plan_t *p = &s_eq.plan;
  const audio_eq_channel_mode_t mode =
      (audio_eq_channel_mode_t)p->config.channel_mode;

  if (!p->config.enabled) {
    if (mode != AUDIO_EQ_CHANNEL_STEREO) process_source_bypass(pcm, frames, mode);
  } else if (mode != AUDIO_EQ_CHANNEL_STEREO || p->active[0] != 0U ||
             p->active[1] != 0U || p->preamp_gain[0] != 1.0f ||
             p->preamp_gain[1] != 1.0f) {
    /* (A neutral chain is an exact pass-through, filter state untouched.) */
    if (mode == AUDIO_EQ_CHANNEL_STEREO) {
      process_stereo_eq(pcm, frames);
    } else {
      process_source_eq(pcm, frames, mode);
    }
    flush_tiny_state(s_eq.state[0], p->active[0]);
    flush_tiny_state(s_eq.state[1], p->active[1]);
  }
  /* The crossover is the speaker's only one: it runs with the EQ off too. */
  if (p->config.output_mode == AUDIO_EQ_OUTPUT_BIAMP) process_biamp(pcm, frames);
  if (s_eq.switch_phase != SWITCH_NONE) process_switch(pcm, frames, sample_rate);
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

const char *audio_eq_output_mode_name(audio_eq_output_mode_t mode) {
  switch (mode) {
    case AUDIO_EQ_OUTPUT_MONO: return "mono";
    case AUDIO_EQ_OUTPUT_BIAMP: return "biamp";
    default: return "mono";
  }
}

bool audio_eq_output_mode_from_name(const char *name,
                                    audio_eq_output_mode_t *out) {
  if (!name || !out) return false;
  for (int i = 0; i < AUDIO_EQ_OUTPUT_COUNT; ++i) {
    if (strcasecmp(name, audio_eq_output_mode_name((audio_eq_output_mode_t)i)) == 0) {
      *out = (audio_eq_output_mode_t)i;
      return true;
    }
  }
  return false;
}
