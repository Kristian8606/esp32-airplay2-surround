#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base64.h"

/* Property lists for AirPlay: an XML writer for HTTP /info, a reader for the
 * binary plists senders send, and the binary replies this receiver sends. */

/* ---- XML writer ---- */

typedef struct {
  char *buffer;
  size_t size;
  size_t capacity;
} plist_t;

void plist_init(plist_t *p, char *buffer, size_t capacity);
void plist_begin(plist_t *p);
void plist_dict_begin(plist_t *p);
void plist_dict_string(plist_t *p, const char *key, const char *value);
void plist_dict_int(plist_t *p, const char *key, int64_t value);
void plist_dict_uint(plist_t *p, const char *key, uint64_t value);
void plist_dict_data(plist_t *p, const char *key, const uint8_t *data,
                     size_t len);
void plist_dict_end(plist_t *p);
void plist_dict_array_begin(plist_t *p, const char *key);
/* Close with plist_dict_end. */
void plist_dict_dict_begin(plist_t *p, const char *key);
void plist_array_end(plist_t *p);
/* Returns the document's length. The text is always NUL-terminated; what did
 * not fit is left out. */
size_t plist_end(plist_t *p);

/* ---- Binary plist reader ----
 *
 * The root must be a dictionary. All functions return false on malformed
 * input, and only read inside the given buffer. */

/* The value of a key in the root, or else the first in the nested
 * dictionaries and arrays, depth first. A key in the root that is not data or
 * does not fit ends the search. */
bool bplist_find_data_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, uint8_t *out_data,
                           size_t out_capacity, size_t *out_len);

/* These read keys of the root dictionary only. */
bool bplist_find_int(const uint8_t *plist, size_t plist_len, const char *key,
                     int64_t *out_value);
/* Accepts a real or an integer. */
bool bplist_find_real(const uint8_t *plist, size_t plist_len, const char *key,
                      double *out_value);
/* An ASCII string including its NUL must fit. */
bool bplist_find_string(const uint8_t *plist, size_t plist_len, const char *key,
                        char *out_str, size_t out_capacity);

/* The root's "streams" array. */
bool bplist_get_streams_count(const uint8_t *plist, size_t plist_len,
                              size_t *count);

/* One stream's integer "type" and the lengths of its data keys, 0 if absent. */
bool bplist_get_stream_info(const uint8_t *plist, size_t plist_len,
                            size_t index, int64_t *type, size_t *ekey_len,
                            size_t *eiv_len, size_t *shk_len);

#define BPLIST_VALUE_UNKNOWN 0
#define BPLIST_VALUE_INT     1
#define BPLIST_VALUE_DATA    2
#define BPLIST_VALUE_STRING  3
#define BPLIST_VALUE_UID     4
#define BPLIST_VALUE_ARRAY   5
#define BPLIST_VALUE_DICT    6

typedef struct {
  char key[64];
  uint8_t value_type;
  size_t value_len; /* data bytes, string characters */
  int64_t int_value;
} bplist_kv_info_t;

/* Summary of every key of one stream. "sr" may be a real: a whole, positive
 * value counts as an integer, any other makes the call fail. Keys that do not
 * fit key[] are left out; false for an empty stream or if more keys remain
 * than out_capacity. */
bool bplist_get_stream_kv_info(const uint8_t *plist, size_t plist_len,
                               size_t index, bplist_kv_info_t *out,
                               size_t out_capacity, size_t *out_count);

/* Data keys of the first stream of this type. True if any was found; a value
 * that does not fit its buffer counts as absent. Buffers may be NULL. */
bool bplist_find_stream_crypto(const uint8_t *plist, size_t plist_len,
                               int64_t stream_type, uint8_t *ekey,
                               size_t ekey_capacity, size_t *ekey_len,
                               uint8_t *eiv, size_t eiv_capacity,
                               size_t *eiv_len, uint8_t *shk,
                               size_t shk_capacity, size_t *shk_len);

/* ---- Binary plist replies ----
 *
 * Each returns the length written, or 0 if it does not fit or the arguments
 * are not valid. */

/* First SETUP: event port, and timing port 0 because timing uses PTP. */
size_t bplist_build_initial_setup(uint8_t *out, size_t capacity,
                                  uint16_t event_port);

/* Stream SETUP. audioBufferSize is sent for buffered streams (type 103). */
size_t bplist_build_stream_setup(uint8_t *out, size_t capacity,
                                 int64_t stream_type, uint16_t data_port,
                                 uint16_t control_port,
                                 uint32_t audio_buffer_size);

/* /feedback keep-alive: without it a sender tears a paused stream down. */
size_t bplist_build_feedback_response(uint8_t *out, size_t capacity,
                                      int64_t stream_type, double sample_rate);

/* RTSP /info, the same content as the XML one. */
size_t bplist_build_info_response(uint8_t *out, size_t capacity,
                                  const char *device_id,
                                  const char *device_name,
                                  const uint8_t *public_key,
                                  size_t public_key_len, uint64_t features,
                                  double initial_volume);

/* Commands on the event connection. Valid commands are 0 to 5. */
size_t bplist_build_media_remote_command(uint8_t *out, size_t capacity,
                                         uint8_t command);

/* Volume notification, 0 to 1. */
size_t bplist_build_media_remote_volume(uint8_t *out, size_t capacity,
                                        double volume);
