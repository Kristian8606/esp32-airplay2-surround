#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_EQ_CONFIG_VERSION 4U
#define AUDIO_EQ_MAX_FILTERS_PER_CHANNEL 24U

/* Bi-amp: the crossover between the two amplifier outputs. */
#define AUDIO_EQ_XOVER_MIN_HZ      800.0f
#define AUDIO_EQ_XOVER_MAX_HZ      8000.0f
#define AUDIO_EQ_XOVER_DEFAULT_HZ  2500.0f
#define AUDIO_EQ_XOVER_DEFAULT_DB  24U
/* Delay of one amplifier output (driver time alignment): 1 ms = 34 cm. */
#define AUDIO_EQ_DELAY_MAX_MS      2.0f

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

/* What the two amplifier outputs drive.
 *   mono:   both outputs play the speaker's full range (one driver or two in
 *           parallel), each through its own EQ chain.
 *   bi-amp: one output drives the tweeter, the other the woofer; there is no
 *           passive crossover in the speaker, so the DSP crossover is the
 *           only one. It runs even with the EQ disabled, and the tweeter
 *           output never gets audio that has not passed its high-pass. */
typedef enum {
  AUDIO_EQ_OUTPUT_MONO = 0,
  AUDIO_EQ_OUTPUT_BIAMP,
  AUDIO_EQ_OUTPUT_COUNT
} audio_eq_output_mode_t;

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
  /* Version 4: the amplifier outputs. All of it is used in bi-amp only.
   * "left"/"right" above and below are the physical amplifier outputs. */
  uint8_t output_mode;    /* audio_eq_output_mode_t */
  uint8_t tweeter_output; /* 0 = left output, 1 = right output */
  uint8_t xover_slope_db; /* Linkwitz-Riley: 12 (LR2), 24 (LR4), 48 (LR8) */
  uint8_t invert_mask;    /* polarity: bit 0 = left output, bit 1 = right */
  float xover_hz;
  float left_delay_ms;
  float right_delay_ms;
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
 * Bi-amp then runs on each output: preamp, EQ, crossover (high-pass on the
 * tweeter output, low-pass on the woofer output), polarity, delay. LR2 sums
 * flat only with one output inverted (the tweeter, by convention); LR4 and
 * LR8 sum flat in phase. A change of the output layout (mode, tweeter
 * output, slope, polarity, delay) under running audio fades both outputs
 * out over 5 ms, installs the new layout from silence, keeps them silent
 * for 20 ms more and fades them back in over 10 ms; a new crossover
 * frequency alone (like an EQ tweak) is applied live with no mute.
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

/* The physical output that drives the tweeter (0 = left, 1 = right), or -1
 * when the outputs are not bi-amped. Audio written to I2S past
 * audio_eq_process() (the wired latency test's bursts) must keep this
 * output silent. */
int audio_eq_tweeter_output(void);

const char *audio_eq_filter_type_name(audio_eq_filter_type_t type);
bool audio_eq_filter_type_from_name(const char *name,
                                    audio_eq_filter_type_t *out);
const char *audio_eq_channel_mode_name(audio_eq_channel_mode_t mode);
bool audio_eq_channel_mode_from_name(const char *name,
                                     audio_eq_channel_mode_t *out);
const char *audio_eq_output_mode_name(audio_eq_output_mode_t mode);
bool audio_eq_output_mode_from_name(const char *name,
                                    audio_eq_output_mode_t *out);

#ifdef __cplusplus
}
#endif
