#include "audio_loudness.h"

#include <math.h>
#include <string.h>

#include "audio_receiver.h"
#include "esp_log.h"
#include "settings.h"
#include "airplay_log.h"

static const char *TAG = "loudness";

/* Settings, written by the web server, read by the playout task:
 * bit 31 = enabled, bits 0..7 = reference offset in dB. */
static volatile uint32_t s_cfg;

#define CFG_ENABLED 0x80000000U

/* Volume turned down (more correction): the boost glides up by at most this
 * many dB of attenuation per 256-frame block (5.8 ms), 86 dB/s, so it never
 * steps. Volume turned up (less correction): the boost follows at once, in
 * the same block as the volume ramp. Lagging there would leave a large bass
 * boost on a suddenly loud signal and clip it. */
#define ATT_STEP_DB 0.5f
/* Blocks at unity gain before the filters leave the path, so the transient
 * of the last coefficient change has died away (8 blocks = 46 ms, many time
 * constants of a 130 Hz shelf) and the switch to bit-exact is seamless. */
#define UNITY_BLOCKS_BEFORE_BYPASS 8U

typedef struct {
  float b0, b1, b2, a1, a2; /* normalised by a0 */
} lb_coeff_t;

typedef struct {
  float x1, x2, y1, y2;     /* direct form I: safe with changing coefficients */
} lb_state_t;

/* Playout task only. */
static float s_att_db;          /* attenuation the filters currently use */
static float s_coeff_att_db = -1.0f;
static lb_coeff_t s_low, s_high;
static lb_state_t s_state[2][2]; /* [channel][low, high] */
static bool s_running;           /* filters in the path (state live) */
static uint32_t s_unity_blocks;  /* consecutive blocks at unity gain */

static uint32_t pack_cfg(bool enabled, int32_t reference_db) {
  return (enabled ? CFG_ENABLED : 0U) | ((uint32_t)reference_db & 0xffU);
}

esp_err_t audio_loudness_init(void) {
  bool enabled = false;
  int32_t ref = 0;
  esp_err_t err = settings_get_loudness(&enabled, &ref);
  if (err != ESP_OK) {
    enabled = false;
    ref = 0;
  }
  if (ref < 0) ref = 0;
  if (ref > AUDIO_LOUDNESS_REF_MAX_DB) ref = AUDIO_LOUDNESS_REF_MAX_DB;
  __atomic_store_n(&s_cfg, pack_cfg(enabled, ref), __ATOMIC_RELEASE);
  ESP_LOGI(TAG, "Loudness %s, reference -%ld dB", enabled ? "on" : "off", (long)ref);
  return ESP_OK;
}

esp_err_t audio_loudness_set(bool enabled, int32_t reference_db, bool save) {
  if (reference_db < 0 || reference_db > AUDIO_LOUDNESS_REF_MAX_DB)
    return ESP_ERR_INVALID_ARG;
  const uint32_t cfg = pack_cfg(enabled, reference_db);
  const uint32_t old = __atomic_exchange_n(&s_cfg, cfg, __ATOMIC_ACQ_REL);
  if (old != cfg) {
    AP_LOGI(TAG, "Loudness %s, reference -%ld dB", enabled ? "on" : "off",
             (long)reference_db);
  }
  if (!save) return ESP_OK;
  return settings_set_loudness(enabled, reference_db);
}

/* Output gain (Q15) -> attenuation to compensate, dB (0 = none). */
static float target_att_db(uint32_t cfg, int32_t gain_q15) {
  if (!(cfg & CFG_ENABLED) || gain_q15 <= 0 || gain_q15 >= 32768) return 0.0f;
  const float level_db = 20.0f * log10f((float)gain_q15 * (1.0f / 32768.0f));
  float att = -level_db - (float)(cfg & 0xffU);
  if (att < 0.0f) att = 0.0f;
  if (att > AUDIO_LOUDNESS_MAX_ATT_DB) att = AUDIO_LOUDNESS_MAX_ATT_DB;
  return att;
}

void audio_loudness_get_status(audio_loudness_status_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  const uint32_t cfg = __atomic_load_n(&s_cfg, __ATOMIC_ACQUIRE);
  int32_t gain = audio_receiver_get_volume_q15();
  if (audio_receiver_get_output_mute_mask() == 3U) gain = 0;
  out->enabled = (cfg & CFG_ENABLED) != 0U;
  out->reference_db = (int32_t)(cfg & 0xffU);
  out->output_silent = gain <= 0;
  out->output_db = gain <= 0 ? 0.0f
                 : gain >= 32768 ? 0.0f
                 : 20.0f * log10f((float)gain * (1.0f / 32768.0f));
  out->correction_db = target_att_db(cfg, gain);
}

/* RBJ shelving filter, slope form (Audio EQ Cookbook). */
static void shelf(lb_coeff_t *c, bool high, float f0, float slope, float gain_db) {
  const float A = powf(10.0f, gain_db / 40.0f);
  const float w0 = 2.0f * (float)M_PI * f0 / AUDIO_LOUDNESS_SAMPLE_RATE;
  const float cw = cosf(w0);
  const float alpha =
      sinf(w0) * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f);
  const float sa = 2.0f * sqrtf(A) * alpha;
  float b0, b1, b2, a0, a1, a2;
  if (!high) {
    b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + sa);
    b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
    b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - sa);
    a0 = (A + 1.0f) + (A - 1.0f) * cw + sa;
    a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
    a2 = (A + 1.0f) + (A - 1.0f) * cw - sa;
  } else {
    b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + sa);
    b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
    b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - sa);
    a0 = (A + 1.0f) - (A - 1.0f) * cw + sa;
    a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
    a2 = (A + 1.0f) - (A - 1.0f) * cw - sa;
  }
  const float inv = 1.0f / a0;
  c->b0 = b0 * inv;
  c->b1 = b1 * inv;
  c->b2 = b2 * inv;
  c->a1 = a1 * inv;
  c->a2 = a2 * inv;
}

/* After digital silence the filter memory decays toward denormal floats,
 * which some FPUs handle slowly. Clear values far below the 32-bit LSB
 * (2^-31 of full scale ~ 5e-10) once per block. */
static void flush_tiny_state(void) {
  float *v = &s_state[0][0].x1;
  for (size_t i = 0; i < sizeof(s_state) / sizeof(float); ++i) {
    if (fabsf(v[i]) < 1e-20f) v[i] = 0.0f;
  }
}

static inline float biquad(float x, const lb_coeff_t *c, lb_state_t *s) {
  const float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 -
                  c->a2 * s->y2;
  s->x2 = s->x1;
  s->x1 = x;
  s->y2 = s->y1;
  s->y1 = y;
  return y;
}

/* Per block: move toward the target attenuation, refresh the coefficients.
 * Returns false when the block can pass untouched (bit-exact). */
static bool loudness_prepare(int32_t gain_q15) {
  const uint32_t cfg = __atomic_load_n(&s_cfg, __ATOMIC_ACQUIRE);
  const float target = target_att_db(cfg, gain_q15);
  if (!s_running && target == 0.0f) return false;
  if (target > s_att_db + ATT_STEP_DB) s_att_db += ATT_STEP_DB;
  else s_att_db = target; /* down, or within one step: at once */

  if (s_att_db == 0.0f) {
    /* Unity gain: keep filtering a few blocks so the last change settles,
     * then leave the path (bit-exact from there on). */
    if (++s_unity_blocks > UNITY_BLOCKS_BEFORE_BYPASS) {
      memset(s_state, 0, sizeof(s_state));
      s_running = false;
      s_coeff_att_db = -1.0f;
      s_unity_blocks = 0;
      return false;
    }
  } else {
    s_unity_blocks = 0;
  }

  if (s_att_db != s_coeff_att_db) {
    shelf(&s_low, false, AUDIO_LOUDNESS_LOW_HZ, AUDIO_LOUDNESS_LOW_SLOPE,
          AUDIO_LOUDNESS_LOW_DB_PER_DB * s_att_db);
    shelf(&s_high, true, AUDIO_LOUDNESS_HIGH_HZ, AUDIO_LOUDNESS_HIGH_SLOPE,
          AUDIO_LOUDNESS_HIGH_DB_PER_DB * s_att_db);
    s_coeff_att_db = s_att_db;
  }
  s_running = true;
  return true;
}

void audio_loudness_reset(void) {
  memset(s_state, 0, sizeof(s_state));
  /* Rebuild the coefficients on the next block: the I2S rate may have
   * changed with the stream. */
  s_coeff_att_db = -1.0f;
}

/* Float PCM in place (1.0 = full scale), after the output volume. No
 * clipping here: the single saturation is the float -> int32 step before DMA. */
void audio_loudness_process_f32(float *pcm, uint32_t frames, int32_t gain_q15) {
  if (!pcm || frames == 0U || !loudness_prepare(gain_q15)) return;
  for (uint32_t i = 0; i < frames * 2U; ++i) {
    lb_state_t *st = s_state[i & 1U];
    const float y = biquad(pcm[i], &s_low, &st[0]);
    pcm[i] = biquad(y, &s_high, &st[1]);
  }
  flush_tiny_state();
}
