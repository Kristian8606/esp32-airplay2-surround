#pragma once

/* USB player (ROADMAP_USB_PLAYER.md, phase 1): the formats the Gateway's
 * own player sends, one per speaker lane. Each lane carries one channel
 * (the speaker's role); the speaker writes it to both output slots, so its
 * stereo route, EQ and bi-amp see what a LEFT/RIGHT route gives today.
 *
 * Kept apart from airplay_audio_formats (the table RTSP advertises to Apple
 * senders): nothing here is offered to Apple, and a speaker without
 * CONFIG_AIRPLAY_SURROUND_PLAYER_STREAMS skips these SSRCs as unknown.
 *
 * SSRC: 0x50 ('P'), source and channels, rate in kHz (44 / 48), bits
 * (0x16 / 0x24). The source byte is 0x01 for a lane of a stereo or mono
 * file and 0x81 for a lane of a 5.1/7.1 file: like a 5.1/7.1 AAC block
 * today, such a block is already the speaker's own channel (no stereo
 * route) and its subwoofer gets the LFE gain. The flag travels with every
 * block, so a change of file never plays a block with the other's flag. */

#include <stddef.h>
#include <stdint.h>

#include "airplay_audio_formats.h"

static const airplay_audio_format_t surround_player_formats[] = {
    {0, 0x50014416U, 2, "ALAC", 44100, 1, 16, 352, "ALAC 44100 Hz / 16-bit / one channel"},
    {0, 0x50014424U, 2, "ALAC", 44100, 1, 24, 352, "ALAC 44100 Hz / 24-bit / one channel"},
    {0, 0x50014816U, 2, "ALAC", 48000, 1, 16, 352, "ALAC 48000 Hz / 16-bit / one channel"},
    {0, 0x50014824U, 2, "ALAC", 48000, 1, 24, 352, "ALAC 48000 Hz / 24-bit / one channel"},
    {0, 0x50814416U, 2, "ALAC", 44100, 1, 16, 352, "ALAC 44100 Hz / 16-bit / one channel of 5.1/7.1"},
    {0, 0x50814424U, 2, "ALAC", 44100, 1, 24, 352, "ALAC 44100 Hz / 24-bit / one channel of 5.1/7.1"},
    {0, 0x50814816U, 2, "ALAC", 48000, 1, 16, 352, "ALAC 48000 Hz / 16-bit / one channel of 5.1/7.1"},
    {0, 0x50814824U, 2, "ALAC", 48000, 1, 24, 352, "ALAC 48000 Hz / 24-bit / one channel of 5.1/7.1"},
};

#define SURROUND_PLAYER_SSRC_MULTICHANNEL 0x00800000U

#define SURROUND_PLAYER_FORMAT_COUNT \
  (sizeof(surround_player_formats) / sizeof(surround_player_formats[0]))

static inline const airplay_audio_format_t *surround_player_format_by_ssrc(uint32_t ssrc) {
  for (size_t i = 0; i < SURROUND_PLAYER_FORMAT_COUNT; ++i)
    if (surround_player_formats[i].ssrc == ssrc) return &surround_player_formats[i];
  return NULL;
}

/* A lane of a 5.1/7.1 file: no stereo route, LFE gain on the subwoofer. */
static inline bool surround_player_format_multichannel(const airplay_audio_format_t *f) {
  return f && (f->ssrc & SURROUND_PLAYER_SSRC_MULTICHANNEL) != 0U;
}

/* The lane a player sends: one channel, rate, depth, source layout. */
static inline const airplay_audio_format_t *surround_player_lane_format(
    int sample_rate, int bits_per_sample, bool multichannel_source) {
  for (size_t i = 0; i < SURROUND_PLAYER_FORMAT_COUNT; ++i) {
    const airplay_audio_format_t *f = &surround_player_formats[i];
    if (f->sample_rate == sample_rate && f->bits_per_sample == bits_per_sample &&
        surround_player_format_multichannel(f) == multichannel_source)
      return f;
  }
  return NULL;
}

/* The first lane format for a rate, depth and channel count (either source
 * layout: the session format check); NULL if none. */
static inline const airplay_audio_format_t *surround_player_format_find(
    int sample_rate, int bits_per_sample, int channels, int frame_size) {
  for (size_t i = 0; i < SURROUND_PLAYER_FORMAT_COUNT; ++i) {
    const airplay_audio_format_t *f = &surround_player_formats[i];
    if (f->sample_rate == sample_rate && f->bits_per_sample == bits_per_sample &&
        f->channels == channels && f->frame_size == frame_size)
      return f;
  }
  return NULL;
}

static inline bool surround_player_format_is(const airplay_audio_format_t *f) {
  return f >= &surround_player_formats[0] &&
         f < &surround_player_formats[SURROUND_PLAYER_FORMAT_COUNT];
}
