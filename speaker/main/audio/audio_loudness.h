#pragma once

/* Loudness compensation (ISO 226 equal-loudness contours).
 *
 * As the output is turned down, the ear loses bass (and a little of the top
 * end) faster than the midrange. Two shelving filters, fitted to ISO 226
 * (reference 80 phon, within 0.6 dB from 31.5 Hz to 10 kHz for 10..30 dB of
 * attenuation), put that back in proportion to how far the output is below
 * the reference level:
 *
 *   low shelf  130 Hz, slope 0.4, gain 0.52 dB per dB of attenuation
 *   high shelf 10.5 kHz, slope 1.0, gain 0.15 dB per dB of attenuation
 *
 * Attenuation = output level below the reference (the output gain set by the
 * AirPlay volume, minus the user's reference offset), limited to 40 dB. The
 * boost is always smaller than the attenuation, so the result never exceeds
 * full scale.
 *
 * Runs in the playout task right after the output volume, so it follows the
 * volume at once (the EQ runs before the PCM buffer, seconds earlier). When
 * off, or at the reference level, the output is left bit-exact. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_playout.h" /* audio_playout_get_rate() */

#define AUDIO_LOUDNESS_REF_MAX_DB 20
#define AUDIO_LOUDNESS_MAX_ATT_DB 40.0f
#define AUDIO_LOUDNESS_LOW_HZ 130.0f
#define AUDIO_LOUDNESS_LOW_SLOPE 0.4f
#define AUDIO_LOUDNESS_LOW_DB_PER_DB 0.52f
#define AUDIO_LOUDNESS_HIGH_HZ 10500.0f
#define AUDIO_LOUDNESS_HIGH_SLOPE 1.0f
#define AUDIO_LOUDNESS_HIGH_DB_PER_DB 0.15f
/* The filters run at the current I2S rate (switched per stream in the
 * multichannel build; audio_loudness_reset() then rebuilds them). */
#define AUDIO_LOUDNESS_SAMPLE_RATE ((float)audio_playout_get_rate())

typedef struct {
  bool enabled;
  int32_t reference_db;   /* 0..AUDIO_LOUDNESS_REF_MAX_DB */
  bool output_silent;     /* volume at minimum / muted */
  float output_db;        /* current output gain, dB (<= 0) */
  float correction_db;    /* attenuation the filters compensate, dB */
} audio_loudness_status_t;

/* Load the saved settings (off and 0 dB when nothing is saved). */
esp_err_t audio_loudness_init(void);
/* Apply now; also store in NVS when save is true. */
esp_err_t audio_loudness_set(bool enabled, int32_t reference_db, bool save);
void audio_loudness_get_status(audio_loudness_status_t *out);

/* Playout task only. gain_q15 is the output volume just applied
 * (0..32768, 32768 = 0 dB). Samples are interleaved stereo. */
void audio_loudness_process_f32(float *pcm, uint32_t frames, int32_t gain_q15);
/* Playout task only: the output stopped (flush). Clear the filter memory so
 * the next audio starts from silence, as the output did. */
void audio_loudness_reset(void);
