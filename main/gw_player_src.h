#pragma once

/* USB player sources and containers (phase 3a: HTTP and WAV). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ---- source: bytes of a file ---- */

typedef struct player_source player_source_t;
struct player_source {
  /* > 0 bytes read, 0 at the end, < 0 on an error */
  int (*read)(player_source_t *s, uint8_t *buf, size_t len);
  void (*close)(player_source_t *s);
  int64_t size; /* -1 when unknown */
  void *ctx;
};

/* An http:// file (a plain GET; seeking comes in phase 3d). */
esp_err_t player_source_http_open(player_source_t *s, const char *url);

/* ---- WAV ---- */

typedef struct {
  int rate;
  int channels;      /* in the file */
  int container_bits; /* 16, 24 or 32 */
  int bits;          /* valid bits: 16 or 24 */
  uint32_t mask;     /* WAVE_FORMAT_EXTENSIBLE channel mask, or the default */
  int8_t slot[8];    /* file channel -> internal slot (L R C LFE Ls Rs Lrs Rrs), -1 none */
  uint8_t present;   /* internal slots that carry a channel */
  int layout;        /* 1, 2, 6 or 8 */
  size_t data_offset;
  uint32_t data_size; /* 0xFFFFFFFF or 0 when not known */
  char why[64];      /* why it is not supported */
} wav_info_t;

/* ---- FLAC ----
 * fLaC and its STREAMINFO (at the start of buf, after any ID3v2 tag the
 * caller skipped). Fills rate, channels, bits, layout, slot map and present
 * of info (container_bits stays 0: the decoder's output decides it) and the
 * total sample count (0 = unknown). Same returns as wav_parse. FLAC's own
 * channel order: 6 = FL FR FC LFE BL BR, 8 = FL FR FC LFE BL BR SL SR. */
esp_err_t flac_parse(const uint8_t *buf, size_t len, wav_info_t *info, uint64_t *total_samples);

/* The size of an ID3v2 tag at the start of buf (header included), 0 if
 * there is none. */
size_t id3v2_size(const uint8_t *buf, size_t len);

/* Parses the RIFF/WAVE header in buf. ESP_OK with *info filled once the
 * data chunk starts inside buf; ESP_ERR_INVALID_SIZE when more of the file
 * is needed; ESP_ERR_NOT_SUPPORTED (info->why) or ESP_FAIL otherwise. */
esp_err_t wav_parse(const uint8_t *buf, size_t len, wav_info_t *info);
