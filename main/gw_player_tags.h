#pragma once

/* Tags and cover art for the Gateway's player (ROADMAP_USB_PLAYER.md,
 * phase 6a): FLAC Vorbis comments and PICTURE, ID3v2.3/2.4 text frames and
 * APIC (any file that starts with an ID3 tag), MP4 ilst (the M4A index
 * fills those in gw_player_m4a.c). Everything through the source's
 * read_at, so the stream keeps its place. Text comes out as UTF-8. */

#include <stdbool.h>
#include <stdint.h>

#include "gw_player_src.h"

typedef struct {
  char title[96], artist[96], album[96];
  char track[12], year[12];
  /* the picture: where it is in the file (len 0 = none) */
  int64_t cover_off;
  uint32_t cover_len;
  char cover_mime[24];
} player_tags_t;

/* An ID3v2 tag at the file's start (its size, header included, tag_bytes). */
void tags_read_id3(player_source_t *src, int64_t tag_bytes, player_tags_t *t);

/* FLAC's VORBIS_COMMENT and PICTURE blocks: the metadata from fLaC at
 * flac_at (after an ID3 tag) to audio_start. */
void tags_read_flac(player_source_t *src, int64_t flac_at, int64_t audio_start, player_tags_t *t);

/* A Vorbis comment list (VORBIS_COMMENT's body): TITLE, ARTIST, ALBUM,
 * TRACKNUMBER, DATE. */
void tags_vorbis(const uint8_t *b, size_t len, player_tags_t *t);

/* UTF-16 (le or be) to UTF-8, at most out_len - 1 bytes and a NUL. */
void tags_utf16_to_utf8(const uint8_t *in, size_t len, bool be, char *out, size_t out_len);
