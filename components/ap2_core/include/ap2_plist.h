#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Property lists in AirPlay 2: an XML writer for the HTTP /info reply, a
 * reader for the binary plists a sender sends, and the binary plists this
 * accessory sends. */

/* ---- XML writer ---- */

/* Writes into a caller's buffer. The fields are private. */
typedef struct {
  char *buffer;
  size_t size;
  size_t capacity;
} ap2_xml_t;

void ap2_xml_init(ap2_xml_t *xml, char *buffer, size_t capacity);
/* The XML header and the opening <plist>. */
void ap2_xml_begin(ap2_xml_t *xml);
void ap2_xml_dict_begin(ap2_xml_t *xml);
/* Keys and values of the open dictionary. */
void ap2_xml_string(ap2_xml_t *xml, const char *key, const char *value);
void ap2_xml_int(ap2_xml_t *xml, const char *key, int64_t value);
void ap2_xml_uint(ap2_xml_t *xml, const char *key, uint64_t value);
void ap2_xml_data(ap2_xml_t *xml, const char *key, const uint8_t *data,
                  size_t len);
void ap2_xml_dict_end(ap2_xml_t *xml);
/* An array under a key of the open dictionary; close with
 * ap2_xml_array_end. */
void ap2_xml_array_begin(ap2_xml_t *xml, const char *key);
void ap2_xml_array_end(ap2_xml_t *xml);
/* A dictionary under a key of the open dictionary; close with
 * ap2_xml_dict_end. */
void ap2_xml_child_dict_begin(ap2_xml_t *xml, const char *key);
/* Closes the document and returns its length. The text always ends with a
 * NUL; what did not fit is left out. */
size_t ap2_xml_end(ap2_xml_t *xml);

/* ---- Binary plist reader ----
 *
 * The root must be a dictionary. Every function returns false for malformed
 * input and reads only inside the given buffer. */

/* The data under this key in the root, or else the first found in the
 * nested dictionaries and arrays, depth first. A key in the root whose value
 * is not data, or does not fit, ends the search. */
bool ap2_bplist_data_deep(const uint8_t *plist, size_t plist_len,
                          const char *key, uint8_t *out, size_t out_capacity,
                          size_t *out_len);

/* These read keys of the root only. */
bool ap2_bplist_int(const uint8_t *plist, size_t plist_len, const char *key,
                    int64_t *out);
/* A real or an integer. */
bool ap2_bplist_real(const uint8_t *plist, size_t plist_len, const char *key,
                     double *out);
/* An ASCII string; it and its NUL must fit. */
bool ap2_bplist_string(const uint8_t *plist, size_t plist_len,
                       const char *key, char *out, size_t out_capacity);

/* The length of the root's "streams" array. */
bool ap2_bplist_stream_count(const uint8_t *plist, size_t plist_len,
                             size_t *count);

/* One stream's integer "type" and the lengths of its "ekey", "eiv" and "shk"
 * data, 0 for a key that is absent. */
bool ap2_bplist_stream_info(const uint8_t *plist, size_t plist_len,
                            size_t index, int64_t *type, size_t *ekey_len,
                            size_t *eiv_len, size_t *shk_len);

/* Kinds of value in ap2_bplist_key_t. */
#define AP2_BPLIST_UNKNOWN 0
#define AP2_BPLIST_INT     1
#define AP2_BPLIST_DATA    2
#define AP2_BPLIST_STRING  3
#define AP2_BPLIST_UID     4
#define AP2_BPLIST_ARRAY   5
#define AP2_BPLIST_DICT    6

/* One key of a stream. */
typedef struct {
  char key[64];
  uint8_t value_type; /* AP2_BPLIST_* */
  size_t value_len;   /* bytes of data, characters of a string */
  int64_t int_value;
} ap2_bplist_key_t;

/* Every key of one stream. "sr" may be a real: a whole positive value counts
 * as an integer, any other makes the call fail. A key longer than key[] is
 * left out. False for an empty stream or when more keys remain than
 * out_capacity. */
bool ap2_bplist_stream_keys(const uint8_t *plist, size_t plist_len,
                            size_t index, ap2_bplist_key_t *out,
                            size_t out_capacity, size_t *out_count);

/* The "ekey", "eiv" and "shk" data of the first stream of this type. True
 * if any was found; a value too big for its buffer counts as absent. Any
 * buffer may be NULL, and so may its length pointer. */
bool ap2_bplist_stream_crypto(const uint8_t *plist, size_t plist_len,
                              int64_t stream_type, uint8_t *ekey,
                              size_t ekey_capacity, size_t *ekey_len,
                              uint8_t *eiv, size_t eiv_capacity,
                              size_t *eiv_len, uint8_t *shk,
                              size_t shk_capacity, size_t *shk_len);

/* ---- Binary plists this accessory sends ----
 *
 * Each returns the length written, or 0 if it does not fit or an argument is
 * not valid. */

/* The reply to the first SETUP: the event port, and timing port 0 because
 * timing goes over PTP. */
size_t ap2_reply_setup_session(uint8_t *out, size_t capacity,
                               uint16_t event_port);

/* The reply to a stream SETUP. audioBufferSize is sent for a buffered stream
 * (type 103). */
size_t ap2_reply_setup_stream(uint8_t *out, size_t capacity,
                              int64_t stream_type, uint16_t data_port,
                              uint16_t control_port,
                              uint32_t audio_buffer_size);

/* The reply to /feedback: without it a sender ends a paused stream. */
size_t ap2_reply_feedback(uint8_t *out, size_t capacity, int64_t stream_type,
                          double sample_rate);

/* What the RTSP /info reply says about the accessory. */
typedef struct {
  const char *device_id;         /* "deviceid", AA:BB:CC:DD:EE:FF */
  const char *name;              /* "name", UTF-8 */
  const char *model;             /* "model" */
  const char *source_version;    /* "srcvers" */
  const char *protocol_versions; /* "protovers" */
  int64_t protocol_version;      /* "vv" */
  int64_t status_flags;          /* "statusFlags" */
  uint64_t features;             /* "features" */
  const uint8_t *public_key;     /* "pk" */
  size_t public_key_len;
  const char *pairing_id;        /* "pi" */
  double initial_volume;         /* "initialVolume" */
  uint64_t audio_stream_formats;  /* "supportedFormats" "audioStream" */
  uint64_t buffer_stream_formats; /* "supportedFormats" "bufferStream" */
} ap2_info_t;

size_t ap2_reply_info(uint8_t *out, size_t capacity, const ap2_info_t *info);

/* Messages on the event channel. */
/* A media remote command, 0 to 5. */
size_t ap2_event_command(uint8_t *out, size_t capacity, uint8_t command);
/* The volume, 0 to 1. */
size_t ap2_event_volume(uint8_t *out, size_t capacity, double volume);
