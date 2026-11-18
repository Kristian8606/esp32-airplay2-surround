#pragma once

/* M4A for the Gateway's player (phase 3d.2): the audio track's index from
 * moov, wherever moov is in the file, read with the source's read_at. The
 * player feeds each sample (an ALAC, AAC or MP3 frame) to the decoder
 * itself, so it can start at any sample: exact seeking, and files with moov
 * after mdat. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_player_src.h"

typedef enum { M4A_ALAC = 1, M4A_AAC, M4A_MP3 } m4a_codec_t;

typedef struct {
  uint32_t count, delta;
} m4a_stts_t;

typedef struct {
  uint32_t first_chunk, per_chunk;
} m4a_stsc_t;

typedef struct {
  m4a_codec_t codec;
  int rate, channels, bits;   /* bits: ALAC's depth; 16 for AAC and MP3 */
  uint32_t timescale;
  uint64_t duration_ts;       /* mdhd, in timescale units */
  uint8_t config[64];         /* ALAC: the 24-byte cookie; AAC: AudioSpecificConfig */
  size_t config_len;
  /* AAC as ADTS for the decoder: from the AudioSpecificConfig */
  uint8_t adts_profile, adts_rate_ix, adts_channels;
  /* the sample table */
  uint32_t samples;
  uint32_t const_size;        /* stsz: every sample this size (0 = table) */
  uint32_t *sizes;
  uint32_t max_size;
  uint32_t chunks;
  uint64_t *chunk_off;
  m4a_stsc_t *stsc;
  uint32_t n_stsc;
  m4a_stts_t *stts;
  uint32_t n_stts;
  int64_t moov_off;           /* where moov is, for the log */
  bool moov_last;             /* moov after mdat */
} m4a_track_t;

/* Where a sample is: a cursor that steps on cheaply. */
typedef struct {
  uint32_t sample;        /* the next sample */
  uint32_t chunk;         /* its chunk (0-based) */
  uint32_t in_chunk;      /* its place in the chunk */
  uint32_t stsc_ix;
  uint64_t off;           /* its byte in the file */
} m4a_iter_t;

/* Reads the top-level boxes from byte base (after an ID3 tag), finds moov
 * and the first sound track in it. ESP_ERR_NOT_SUPPORTED with why. */
esp_err_t m4a_open(player_source_t *src, int64_t base, m4a_track_t *t, char *why, size_t why_len);
void m4a_close(m4a_track_t *t);

/* The cursor at sample i. */
void m4a_iter_seek(const m4a_track_t *t, m4a_iter_t *it, uint32_t i);
/* The cursor's sample (byte and size); false past the last. Then step on
 * with m4a_iter_next. */
bool m4a_iter_get(const m4a_track_t *t, const m4a_iter_t *it, uint64_t *off, uint32_t *size);
void m4a_iter_next(const m4a_track_t *t, m4a_iter_t *it);

/* The sample that holds time ts (timescale units) and where it starts. */
uint32_t m4a_sample_at(const m4a_track_t *t, uint64_t ts, uint64_t *start_ts);
/* A sample's length in timescale units. */
uint32_t m4a_sample_delta(const m4a_track_t *t, uint32_t i);

/* An ADTS header for an AAC sample of len bytes. */
void m4a_adts_header(const m4a_track_t *t, uint32_t len, uint8_t hdr[7]);
