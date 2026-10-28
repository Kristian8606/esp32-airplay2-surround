#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_EQ_CONFIG_VERSION 3U
#define AUDIO_EQ_MAX_FILTERS_PER_CHANNEL 24U

typedef enum {
  AUDIO_EQ_FILTER_PK = 0,
  AUDIO_EQ_FILTER_LP,
  AUDIO_EQ_FILTER_HP,
  AUDIO_EQ_FILTER_BP,
  AUDIO_EQ_FILTER_NOTCH,
  AUDIO_EQ_FILTER_LS,
  AUDIO_EQ_FILTER_HS,
  AUDIO_EQ_FILTER_COUNT
} audio_eq_filter_type_t;

typedef enum {
  AUDIO_EQ_CHANNEL_STEREO = 0,
  AUDIO_EQ_CHANNEL_MONO,
  AUDIO_EQ_CHANNEL_LEFT,
  AUDIO_EQ_CHANNEL_RIGHT,
  AUDIO_EQ_CHANNEL_COUNT
} audio_eq_channel_mode_t;

typedef struct {
  uint8_t enabled;
  uint8_t type;
  uint16_t reserved;
  float frequency_hz;
  float gain_db;
  float q;
} audio_eq_filter_config_t;

typedef struct {
  uint8_t filter_count;
  uint8_t reserved[3];
  audio_eq_filter_config_t filters[AUDIO_EQ_MAX_FILTERS_PER_CHANNEL];
} audio_eq_output_config_t;

typedef struct {
  uint32_t version;
  uint8_t enabled;
  uint8_t channel_mode;
  uint16_t reserved;
  float left_preamp_db;
  float right_preamp_db;
  audio_eq_output_config_t left;
  audio_eq_output_config_t right;
} audio_eq_config_t;

/* Load the saved configuration and initialise the runtime DSP state. */
esp_err_t audio_eq_init(void);

esp_err_t audio_eq_save_config(const audio_eq_config_t *config);
esp_err_t audio_eq_apply_config(const audio_eq_config_t *config);
esp_err_t audio_eq_get_active_config(audio_eq_config_t *out);
void audio_eq_default_config(audio_eq_config_t *out);
bool audio_eq_validate_config(const audio_eq_config_t *config);

/* Process interleaved float stereo PCM in place (1.0 = full scale; see
 * audio_out_sample.h). Output is not clipped: values above full scale are
 * saturated only once, right before DMA.
 * Source selection:
 *   stereo: L source -> left EQ, R source -> right EQ
 *   mono:   (L+R)/2 -> both independent EQ chains
 *   left:   L source -> both independent EQ chains
 *   right:  R source -> both independent EQ chains
 * Coefficients are calculated once per sample-rate change, never per sample. */
void audio_eq_process(float *pcm, size_t frames, int channels,
                      int sample_rate);
/* Calculate the coefficients for sample_rate now (stream SETUP), so the
 * first audio_eq_process() on the playout task finds them ready and neither
 * calculates nor logs there. */
void audio_eq_prepare(int sample_rate);

/* Clear biquad delay state only at a real output-chain boundary (new stream/
 * session or play stop). A normal AAC timestamp gap or codec-chain switch is
 * recovered by decoding/muting the first discontinuous block, so the EQ chain
 * sees silence rather than being asynchronously reset. */
void audio_eq_reset_state(void);

const char *audio_eq_filter_type_name(audio_eq_filter_type_t type);
bool audio_eq_filter_type_from_name(const char *name,
                                    audio_eq_filter_type_t *out);
const char *audio_eq_channel_mode_name(audio_eq_channel_mode_t mode);
bool audio_eq_channel_mode_from_name(const char *name,
                                     audio_eq_channel_mode_t *out);

#ifdef __cplusplus
}
#endif
