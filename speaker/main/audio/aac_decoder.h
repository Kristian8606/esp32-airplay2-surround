#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AAC_DECODER_INPUT_HEADROOM 7U

typedef struct aac_decoder aac_decoder_t;

typedef struct {
  int sample_rate;
  int channels;
  int bits_per_sample; /* Announced AirPlay metadata; ignored by AAC decode. */
} aac_decoder_config_t;

/* EXPERIMENT (multichannel): the speaker of a 5.1/7.1 stream this receiver
 * plays, on both stereo slots. Names follow Apple's 7.1 order; see the
 * Kconfig help of AIRPLAY_MULTICHANNEL_SPEAKER for the ISO caveat. */
typedef enum {
  AAC_SPEAKER_MIX = -1, /* all channels except LFE, mono */
  AAC_SPEAKER_L = 0,
  AAC_SPEAKER_R,
  AAC_SPEAKER_C,
  AAC_SPEAKER_LFE,
  AAC_SPEAKER_LS,
  AAC_SPEAKER_RS,
  AAC_SPEAKER_LRS, /* 5.1: plays Ls */
  AAC_SPEAKER_RRS, /* 5.1: plays Rs */
  AAC_SPEAKER_COUNT
} aac_speaker_t;

typedef struct {
  int channels;
} aac_decode_info_t;

aac_decoder_t *aac_decoder_create(const aac_decoder_config_t *config);
void aac_decoder_destroy(aac_decoder_t *decoder);
/* input points to payload with AAC_DECODER_INPUT_HEADROOM writable bytes
 * immediately before it. Raw AU input receives an ADTS header there; payload
 * is never moved. Input that already has ADTS is passed through unchanged.
 * Output is stereo float32 (mono duplicated), normalized from FDK signed32
 * by 2^31 without a 16-bit intermediate. Capacity counts stereo frames. */
int aac_decoder_decode(aac_decoder_t *decoder, uint8_t *input,
                       size_t input_len, float *output,
                       size_t output_capacity_frames,
                       aac_decode_info_t *info);

/* EXPERIMENT (multichannel): log the reset reason and, after a crash, the
 * FDK decoder step that was running (kept in RTC memory across the reboot).
 * Call once at boot after logging is up. No-op in normal builds. */
void aac_decoder_report_last_run(void);

/* Multichannel speaker selection. set takes effect on the next decoded frame
 * of a running stream (any task may call it); get returns the selection.
 * Out-of-range values select AAC_SPEAKER_MIX. */
void aac_decoder_set_speaker(int speaker);
int aac_decoder_get_speaker(void);
/* "Front Left (L)" etc.; "Mono mix" for AAC_SPEAKER_MIX. */
const char *aac_speaker_name(int speaker);
/* While a 5.1/7.1 stream decodes: the decoded channel index now played (-1 =
 * mix) and FDK's label for it ("front #1"). false when none is decoding or
 * the build has no multichannel support. */
bool aac_decoder_get_mc_status(int *output_channel, int *stream_channels,
                               char *label, size_t label_len);
