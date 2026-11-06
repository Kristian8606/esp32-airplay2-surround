/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The encrypted HAP channel (spec "Encryption"): frames of a 2-byte
 * little-endian length (at most 1024), that many bytes sealed with
 * ChaCha20-Poly1305 and a 16-byte tag. The length is the associated data;
 * the nonce is four zero bytes and the 64-bit counter of the direction,
 * little-endian. The cipher is libsodium's
 * crypto_aead_chacha20poly1305_ietf_*_detached(). No ESP-IDF. */

#define RTSP_FRAME_MAX 1024
#define RTSP_FRAME_OVERHEAD 18 /* length + tag */

typedef struct {
  uint8_t key[32];
  uint64_t counter;
} rtsp_cipher_t;

void rtsp_cipher_init(rtsp_cipher_t *cipher, const uint8_t key[32]);
void rtsp_cipher_wipe(rtsp_cipher_t *cipher);

/* Seals len bytes (1 to RTSP_FRAME_MAX) into one frame of
 * len + RTSP_FRAME_OVERHEAD bytes at out. in may be out + 2 (sealed in
 * place); otherwise they do not overlap. Returns its length, 0 if len is
 * out of range. */
size_t rtsp_frame_seal(rtsp_cipher_t *cipher, const uint8_t *in, size_t len,
                       uint8_t *out);

/* A message is its head and then its body, sent as one frame when it is
 * RTSP_FRAME_MAX bytes or less, else in frames of RTSP_FRAME_MAX with the
 * rest in the last; never head and body in frames of their own (spec
 * "Encryption"). Copies the bytes of the frame that starts at offset of
 * head + body to out (at most RTSP_FRAME_MAX) and returns their count, 0
 * past the end. Seal them in place with rtsp_frame_seal(cipher, out, n,
 * out - 2). */
size_t rtsp_frame_gather(const uint8_t *head, size_t head_len,
                         const uint8_t *body, size_t body_len, size_t offset,
                         uint8_t *out);

typedef enum {
  RTSP_FRAME_MORE,  /* everything consumed, the frame is not complete */
  RTSP_FRAME_DONE,  /* one frame opened */
  RTSP_FRAME_ERROR, /* length over the limit or the frame does not open */
} rtsp_frame_result_t;

typedef struct {
  rtsp_cipher_t cipher;
  uint8_t buf[2 + RTSP_FRAME_MAX + 16];
  size_t have;
} rtsp_frame_reader_t;

void rtsp_frame_reader_init(rtsp_frame_reader_t *reader, const uint8_t key[32]);
void rtsp_frame_reader_wipe(rtsp_frame_reader_t *reader);

/* Takes bytes until one frame is complete. *consumed says how many were
 * used. On RTSP_FRAME_DONE *plain points at the opened bytes inside the
 * reader (valid until the next call) and *plain_len is their count. */
rtsp_frame_result_t rtsp_frame_feed(rtsp_frame_reader_t *reader,
                                    const uint8_t *in, size_t len,
                                    size_t *consumed, const uint8_t **plain,
                                    size_t *plain_len);
