/* SPDX-License-Identifier: Apache-2.0 */
/* A small test harness and a test sender talking to rtsp_core. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fakes.h"
#include "rtsp_core.h"
#include "rtsp_frame.h"

extern int t_checks;
extern int t_failures;

#define CHECK(cond)                                                          \
  do {                                                                       \
    t_checks++;                                                              \
    if (!(cond)) {                                                           \
      t_failures++;                                                          \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,       \
              #cond);                                                        \
    }                                                                        \
  } while (0)

#define CHECK_INT(a, b)                                                      \
  do {                                                                       \
    long long _a = (long long)(a), _b = (long long)(b);                      \
    t_checks++;                                                              \
    if (_a != _b) {                                                          \
      t_failures++;                                                          \
      fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__,        \
              __LINE__, #a, _a, _b);                                         \
    }                                                                        \
  } while (0)

#define CHECK_STR(a, b)                                                      \
  do {                                                                       \
    const char *_a = (a), *_b = (b);                                         \
    t_checks++;                                                              \
    if (_a == NULL || strcmp(_a, _b) != 0) {                                 \
      t_failures++;                                                          \
      fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__,    \
              __LINE__, #a, _a ? _a : "(null)", _b);                         \
    }                                                                        \
  } while (0)

/* A call was made, in this order relative to the previous CHECK_CALL. */
#define CHECK_CALL(pos, prefix)                                              \
  do {                                                                       \
    int _i = calls_find_from((pos), (prefix));                               \
    t_checks++;                                                              \
    if (_i < 0) {                                                            \
      t_failures++;                                                          \
      fprintf(stderr, "%s:%d: no call %s after #%d\n", __FILE__, __LINE__,   \
              (prefix), (pos));                                              \
      calls_dump();                                                          \
    } else {                                                                 \
      (pos) = _i + 1;                                                        \
    }                                                                        \
  } while (0)

void run_test(const char *name, void (*fn)(void));
#define RUN(fn) run_test(#fn, fn)

/* ---- A test sender ---- */

typedef struct {
  int code;
  char head[2048];
  uint8_t body[8192];
  size_t body_len;
} reply_t;

typedef struct {
  rtsp_conn_t *conn;
  uint32_t ip;
  int rc; /* of the last rtsp_conn_receive */
  bool encrypted;
  rtsp_cipher_t send_cipher;
  rtsp_frame_reader_t recv_frames;
  uint8_t out[1 << 17]; /* raw bytes from the server, not yet read */
  size_t out_len;
  uint8_t plain[1 << 17];
  size_t plain_len;
  int cseq;
  bool fail_send;
  int frames_in; /* frames received from the Gateway */
  size_t frame_lens[16]; /* the plain length of the first 16 */
} client_t;

/* ip as a.b.c.d */
client_t *client_new(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void client_free(client_t *client); /* rtsp_conn_end, then free */
/* Raw bytes to the server (sealed in frames when encrypted). */
int client_raw(client_t *client, const void *data, size_t len);
/* Bytes to the server exactly as given. */
int client_feed(client_t *client, const void *data, size_t len);
/* One request; returns rtsp_conn_receive's result. */
int client_request(client_t *client, const char *method, const char *uri,
                   const char *headers, const void *body, size_t body_len);
int client_request_text(client_t *client, const char *method, const char *uri,
                        const char *headers, const char *body);
/* The next reply. False when there is none. */
bool client_reply(client_t *client, reply_t *reply);
/* Header value of a reply, or NULL. */
const char *reply_header(const reply_t *reply, const char *name, char *buf,
                         size_t cap);
/* Body as a NUL-terminated string. */
const char *reply_text(reply_t *reply);

/* Pairing like a sender: transient pair-setup or pair-verify. The
 * connection is encrypted afterwards. */
void client_pair_transient(client_t *client);
void client_pair_verify(client_t *client);
/* Session SETUP; returns the event port of the reply, 0 if none. */
int client_setup_session(client_t *client);
/* Stream SETUP with one stream dict (fake plist text inside {}). */
int client_setup_stream(client_t *client, const char *stream, reply_t *reply);

/* The channel keys a pairing ends with, as the spec derives them. */
void derive_test_key(bool transient, const char *salt, const char *info,
                     uint8_t key[32]);
