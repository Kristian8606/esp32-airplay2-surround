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
  /* Phase 3d, NULL when the source cannot: read goes on from byte off of
   * the file (ESP_ERR_NOT_SUPPORTED when the server ignores Range) */
  esp_err_t (*seek)(player_source_t *s, int64_t off);
  /* len bytes from byte off, apart from read's place; bytes read or < 0 */
  int (*read_at)(player_source_t *s, int64_t off, uint8_t *buf, size_t len);
  int64_t size; /* -1 when unknown */
  void *ctx;
};

/* An http:// file. Seeking and read_at use Range requests; a connection that
 * breaks (a long pause) opens again where it was. */
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
  char why[96];      /* why it is not supported */
} wav_info_t;

/* ---- FLAC ----
 * fLaC and its STREAMINFO (at the start of buf, after any ID3v2 tag the
 * caller skipped). Fills rate, channels, bits, layout, slot map and present
 * of info (container_bits stays 0: the decoder's output decides it) and the
 * total sample count (0 = unknown). Same returns as wav_parse. FLAC's own
 * channel order: 6 = FL FR FC LFE BL BR, 8 = FL FR FC LFE BL BR SL SR. */
esp_err_t flac_parse(const uint8_t *buf, size_t len, wav_info_t *info, uint64_t *total_samples);

/* FLAC's STREAMINFO, kept for seeking: the block sizes (a fixed-block
 * stream numbers its frames, not its samples) and the 34 bytes to start a
 * new decoder with. fLaC points at the file's "fLaC". */
typedef struct {
  uint32_t min_block, max_block;
  uint32_t max_frame; /* bytes, 0 = not known */
  uint8_t streaminfo[34];
} flac_stream_t;
void flac_stream_info(const uint8_t *fLaC, flac_stream_t *out);

/* One FLAC frame header at b (sync 0xFFF8/0xFFF9, CRC-8 checked, the
 * channel count and depth as the stream's): its first sample. False when
 * it is not a frame header. */
/* The CRC-16 at the end of a whole FLAC frame (len bytes at b) is right. */
bool flac_frame_crc_ok(const uint8_t *b, size_t len);

bool flac_frame_at(const uint8_t *b, size_t len, const flac_stream_t *st, int channels,
                   int bits, uint64_t *first_sample);

/* The size of an ID3v2 tag at the start of buf (header included), 0 if
 * there is none. */
size_t id3v2_size(const uint8_t *buf, size_t len);

/* ---- MP3 (phase 3d.2) ----
 * The first frame at buf (after an ID3v2 tag): its format, and a Xing or
 * Info tag in it (frame count, byte count, TOC) when there is one. False
 * when buf does not start with an MPEG audio layer III frame. */
typedef struct {
  int rate, channels, spf, kbps;
  uint32_t xing_frames, xing_bytes; /* 0 = not known */
  bool has_toc;
  uint8_t toc[100];
  /* the LAME tag (phase 3d.3, gapless): the encoder's delay and padding in
   * samples; lame_ok when there is one */
  bool lame_ok;
  int enc_delay, enc_padding;
} mp3_info_t;
bool mp3_parse_first(const uint8_t *buf, size_t len, mp3_info_t *m);

/* Parses the RIFF/WAVE header in buf. ESP_OK with *info filled once the
 * data chunk starts inside buf; ESP_ERR_INVALID_SIZE when more of the file
 * is needed; ESP_ERR_NOT_SUPPORTED (info->why) or ESP_FAIL otherwise. */
esp_err_t wav_parse(const uint8_t *buf, size_t len, wav_info_t *info);
