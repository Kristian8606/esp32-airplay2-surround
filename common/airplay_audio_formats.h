#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airplay_identity.h"

typedef struct {
  uint64_t audio_format;
  uint32_t ssrc;
  int codec_type;
  const char *codec;
  int sample_rate;
  int channels;
  /* ALAC: encoded integer PCM depth. AAC: AirPlay F24 profile precision;
   * lossy AAC has no recoverable source PCM depth. Decoder output is separate. */
  int bits_per_sample;
  int frame_size;
  const char *name;
} airplay_audio_format_t;

static const airplay_audio_format_t airplay_audio_formats[] = {
    {AIRPLAY_FMT_ALAC_44100_S16_2, 0x0000faceU, 2, "ALAC", 44100, 2, 16,
     352, "ALAC 44100 Hz / 16-bit / stereo"},
    {AIRPLAY_FMT_ALAC_48000_S24_2, 0x15000000U, 2, "ALAC", 48000, 2, 24,
     352, "ALAC 48000 Hz / 24-bit / stereo"},
    {AIRPLAY_FMT_AAC_LC_44100_2, 0x16000000U, 4, "AAC", 44100, 2, 24,
     1024, "AAC-LC 44100 Hz / F24 / stereo"},
    {AIRPLAY_FMT_AAC_LC_48000_2, 0x17000000U, 4, "AAC", 48000, 2, 24,
     1024, "AAC-LC 48000 Hz / F24 / stereo"},
    {AIRPLAY_FMT_AAC_LC_48000_5_1, 0x27000000U, 4, "AAC", 48000, 6, 24,
     1024, "AAC-LC 48000 Hz / F24 / 5.1"},
    {AIRPLAY_FMT_AAC_LC_48000_7_1, 0x28000000U, 4, "AAC", 48000, 8, 24,
     1024, "AAC-LC 48000 Hz / F24 / 7.1"},
};

static inline const airplay_audio_format_t *airplay_audio_format_by_id(
    uint64_t audio_format) {
  for (size_t index = 0;
       index < sizeof(airplay_audio_formats) / sizeof(airplay_audio_formats[0]);
       ++index) {
    if (airplay_audio_formats[index].audio_format == audio_format)
      return &airplay_audio_formats[index];
  }
  return NULL;
}

static inline const airplay_audio_format_t *airplay_audio_format_by_ssrc(
    uint32_t ssrc) {
  for (size_t index = 0;
       index < sizeof(airplay_audio_formats) / sizeof(airplay_audio_formats[0]);
       ++index) {
    if (airplay_audio_formats[index].ssrc == ssrc)
      return &airplay_audio_formats[index];
  }
  return NULL;
}

static inline bool airplay_audio_format_supported(
    const airplay_audio_format_t *format, int64_t stream_type) {
  if (!format) return false;
  const uint64_t supported = stream_type == 96 ? AIRPLAY_SUPPORTED_AUDIO_STREAM :
      stream_type == 103 ? AIRPLAY_SUPPORTED_BUFFER_STREAM : 0;
  return (supported & format->audio_format) != 0;
}

static inline const airplay_audio_format_t *airplay_audio_format_resolve(
    int64_t stream_type, uint64_t audio_format, int64_t codec_type,
    int64_t sample_rate, int64_t frame_size, int64_t bits_per_sample,
    int64_t channels) {
  const airplay_audio_format_t *selected = NULL;
  if (audio_format != 0) {
    selected = airplay_audio_format_by_id(audio_format);
  } else {
    if (sample_rate == -1) sample_rate = 44100;
    if (channels == -1) channels = 2;
    for (size_t index = 0;
         index < sizeof(airplay_audio_formats) / sizeof(airplay_audio_formats[0]);
         ++index) {
      const airplay_audio_format_t *candidate = &airplay_audio_formats[index];
      if (candidate->codec_type == codec_type &&
          candidate->sample_rate == sample_rate &&
          candidate->channels == channels) {
        selected = candidate;
        break;
      }
    }
  }
  if (!airplay_audio_format_supported(selected, stream_type) ||
      (codec_type != -1 && codec_type != selected->codec_type) ||
      (sample_rate != -1 && sample_rate != selected->sample_rate) ||
      (frame_size != -1 && frame_size != selected->frame_size) ||
      /* ss is not an encoded PCM depth for AAC. Accept the legacy S16 sender
       * hint and F24 / 32-bit container hints without selecting an S16 decoder.
       * ALAC must still match its actual lossless depth exactly. */
      (bits_per_sample != -1 &&
       (selected->codec_type == 4
            ? (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32)
            : bits_per_sample != selected->bits_per_sample)) ||
      (channels != -1 && channels != selected->channels)) return NULL;
  return selected;
}
