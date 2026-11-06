/* SPDX-License-Identifier: Apache-2.0 */
#include "rtsp_frame.h"

#include <string.h>

#include <sodium.h>

/* libsodium does the cipher (spec "Encryption"); its IETF form has the
 * same 32-byte key, 12-byte nonce and 16-byte tag as the frames. */
_Static_assert(crypto_aead_chacha20poly1305_ietf_KEYBYTES == 32, "key");
_Static_assert(crypto_aead_chacha20poly1305_ietf_NPUBBYTES == 12, "nonce");
_Static_assert(crypto_aead_chacha20poly1305_ietf_ABYTES == 16, "tag");

static void make_nonce(uint64_t counter, uint8_t nonce[12]) {
  memset(nonce, 0, 4);
  for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(counter >> (8 * i));
}

void rtsp_cipher_init(rtsp_cipher_t *cipher, const uint8_t key[32]) {
  memcpy(cipher->key, key, sizeof(cipher->key));
  cipher->counter = 0;
}

void rtsp_cipher_wipe(rtsp_cipher_t *cipher) {
  sodium_memzero(cipher, sizeof(*cipher));
}

size_t rtsp_frame_seal(rtsp_cipher_t *cipher, const uint8_t *in, size_t len,
                       uint8_t *out) {
  uint8_t nonce[12];
  if (len == 0 || len > RTSP_FRAME_MAX) return 0;
  out[0] = (uint8_t)len;
  out[1] = (uint8_t)(len >> 8);
  make_nonce(cipher->counter, nonce);
  crypto_aead_chacha20poly1305_ietf_encrypt_detached(
      out + 2, out + 2 + len, NULL, in, len, out, 2, NULL, nonce, cipher->key);
  cipher->counter++;
  return len + RTSP_FRAME_OVERHEAD;
}

size_t rtsp_frame_gather(const uint8_t *head, size_t head_len,
                         const uint8_t *body, size_t body_len, size_t offset,
                         uint8_t *out) {
  size_t total = head_len + body_len;
  size_t n, from_head = 0;
  if (offset >= total) return 0;
  n = total - offset < RTSP_FRAME_MAX ? total - offset : RTSP_FRAME_MAX;
  if (offset < head_len) {
    from_head = head_len - offset < n ? head_len - offset : n;
    memcpy(out, head + offset, from_head);
  }
  if (n > from_head) {
    memcpy(out + from_head, body + (offset + from_head - head_len),
           n - from_head);
  }
  return n;
}

void rtsp_frame_reader_init(rtsp_frame_reader_t *reader,
                            const uint8_t key[32]) {
  rtsp_cipher_init(&reader->cipher, key);
  reader->have = 0;
}

void rtsp_frame_reader_wipe(rtsp_frame_reader_t *reader) {
  sodium_memzero(reader, sizeof(*reader));
}

rtsp_frame_result_t rtsp_frame_feed(rtsp_frame_reader_t *reader,
                                    const uint8_t *in, size_t len,
                                    size_t *consumed, const uint8_t **plain,
                                    size_t *plain_len) {
  size_t used = 0;
  size_t need;
  size_t frame_len;
  uint8_t nonce[12];

  *consumed = 0;
  /* The length first. */
  while (reader->have < 2 && used < len) reader->buf[reader->have++] = in[used++];
  if (reader->have < 2) {
    *consumed = used;
    return RTSP_FRAME_MORE;
  }
  frame_len = (size_t)reader->buf[0] | ((size_t)reader->buf[1] << 8);
  if (frame_len > RTSP_FRAME_MAX) {
    *consumed = used;
    return RTSP_FRAME_ERROR;
  }
  need = 2 + frame_len + 16;
  {
    size_t n = need - reader->have;
    if (n > len - used) n = len - used;
    memcpy(reader->buf + reader->have, in + used, n);
    reader->have += n;
    used += n;
  }
  *consumed = used;
  if (reader->have < need) return RTSP_FRAME_MORE;

  make_nonce(reader->cipher.counter, nonce);
  reader->have = 0;
  /* opened in place */
  if (crypto_aead_chacha20poly1305_ietf_decrypt_detached(
          reader->buf + 2, NULL, reader->buf + 2, frame_len,
          reader->buf + 2 + frame_len, reader->buf, 2, nonce,
          reader->cipher.key) != 0) {
    return RTSP_FRAME_ERROR;
  }
  reader->cipher.counter++;
  *plain = reader->buf + 2;
  *plain_len = frame_len;
  return RTSP_FRAME_DONE;
}
