/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RTSP/1.0 style messages (spec "Connections and messages"): a start line,
 * headers, an empty line, then Content-Length bytes of body. The reader
 * takes bytes as they come and hands out one message at a time. No
 * ESP-IDF. */

/* The start line and headers together. */
#define RTSP_HEAD_MAX 4096
#define RTSP_MAX_HEADERS 32
/* At least 16 KiB must be accepted (spec); bodies go to rtsp_port_malloc. */
#ifndef RTSP_BODY_MAX
#define RTSP_BODY_MAX (64 * 1024)
#endif

typedef struct {
  const char *name;
  const char *value;
} rtsp_header_t;

typedef enum {
  RTSP_BODY_OK,        /* body holds body_len bytes (NULL when 0) */
  RTSP_BODY_TOO_LARGE, /* Content-Length over the limit; read and dropped */
  RTSP_BODY_NO_MEMORY, /* no buffer; read and dropped */
} rtsp_body_state_t;

typedef struct {
  /* Request: method, URI, version. Response: version, code, reason. The
   * third may be "". */
  const char *start[3];
  rtsp_header_t headers[RTSP_MAX_HEADERS];
  size_t header_count;
  uint8_t *body;
  size_t body_len; /* the Content-Length */
  rtsp_body_state_t body_state;
} rtsp_msg_t;

typedef enum {
  RTSP_READ_MORE,    /* everything consumed, no complete message yet */
  RTSP_READ_MESSAGE, /* reader->msg is complete */
  RTSP_READ_BAD,     /* malformed start line or headers: stop reading */
} rtsp_read_result_t;

typedef struct {
  char head[RTSP_HEAD_MAX + 1];
  size_t head_len;
  int phase;
  size_t body_max;
  size_t body_got;
  rtsp_msg_t msg;
} rtsp_reader_t;

void rtsp_reader_init(rtsp_reader_t *reader, size_t body_max);

/* Takes bytes up to the end of one message; *consumed says how many. After
 * RTSP_READ_MESSAGE the message stays valid until rtsp_reader_next(). */
rtsp_read_result_t rtsp_reader_feed(rtsp_reader_t *reader, const uint8_t *data,
                                    size_t len, size_t *consumed);

/* Frees the body and gets ready for the next message. */
void rtsp_reader_next(rtsp_reader_t *reader);

/* The value of a header (names are case-insensitive), or NULL. */
const char *rtsp_msg_header(const rtsp_msg_t *msg, const char *name);

/* Case-insensitive compare of NUL-terminated strings. */
bool rtsp_str_ieq(const char *a, const char *b);

/* The reply head: "RTSP/1.0 <code> <text>", CSeq (when cseq is not NULL),
 * Server, the extra header lines (each ending in CRLF, may be NULL), then
 * Content-Type and Content-Length when body_len > 0, and the empty line.
 * Returns its length, 0 if it does not fit. */
size_t rtsp_reply_head(char *out, size_t capacity, int code, const char *cseq,
                       const char *extra, const char *content_type,
                       size_t body_len);

/* The text of a status code. */
const char *rtsp_reason(int code);
