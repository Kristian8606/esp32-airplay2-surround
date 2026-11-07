/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sound_engine.h"

/* The volume and format tables and the small text formats of the requests.
 * No ESP-IDF. */

/* ---- Volume (spec "SET_PARAMETER", "Event channel") ---- */

#define RTSP_VOLUME_MUTE (-144.0)
#define RTSP_VOLUME_MIN (-30.0)
#define RTSP_VOLUME_MAX 0.0
#define RTSP_VOLUME_DEFAULT (-24.0) /* when the saved one cannot be read */

/* Gain ((v + 30) / 30)^2 as Q15 (32768 = 0 dB), cut to a whole number; 0
 * for -144 or anything below -30. Above 0 counts as 0. */
int32_t rtsp_volume_to_q15(double volume);

/* (v + 30) / 30, kept within 0 to 1. */
double rtsp_volume_to_fraction(double volume);
/* f * 30 - 30 for f within 0 to 1. */
double rtsp_fraction_to_volume(double fraction);
/* One step of 1/16 up (up = true) or down from the volume, as a fraction
 * kept within 0 to 1. */
double rtsp_volume_step(double volume, bool up);

/* "volume: <v>" value: a decimal number, spaces around allowed. False for
 * anything else (also NaN and infinities). */
bool rtsp_parse_volume(const char *text, double *volume);
/* A volume as it is kept: above 0 as 0, below -144 as -144, the rest as it
 * came. */
double rtsp_volume_clamp(double volume);

/* The volume with two decimals, as printf("%.2f") gives it ("-20.62"). */
void rtsp_format_volume(double volume, char *out, size_t capacity);

/* ---- Formats (spec "SETUP (with streams)") ---- */

/* The engine format of an audioFormat bit for a stream type (96 or 103).
 * False when the pair is not in the table. */
bool rtsp_format_lookup(int64_t stream_type, uint64_t audio_format,
                        sound_format_t *format);

/* ---- Time (spec "SETRATEANCHORTIME") ---- */

/* secs * 10^9 + frac * 10^9 / 2^64, the fraction cut to whole ns. */
uint64_t rtsp_network_time_ns(uint64_t secs, uint64_t frac);

/* ---- Text ---- */

/* The next line of a text body ("\r\n" or "\n"). False at the end. */
bool rtsp_next_line(const char **cursor, const char *end, const char **line,
                    size_t *line_len);

/* A "name: value" line: name and value trimmed into the buffers. False if
 * there is no colon or a part does not fit. */
bool rtsp_split_param(const char *line, size_t line_len, char *name,
                      size_t name_capacity, char *value,
                      size_t value_capacity);

/* The rtptime of an RTP-Info header ("seq=1;rtptime=2" and the like).
 * False if it is absent or not a 32-bit decimal number. */
bool rtsp_parse_rtp_info(const char *header, uint32_t *rtptime);

/* Whether a Content-Type header names this media type (parameters after
 * ';' and case ignored). */
bool rtsp_content_type_is(const char *header, const char *type);
/* Whether it is image/<anything>. */
bool rtsp_content_type_is_image(const char *header);
