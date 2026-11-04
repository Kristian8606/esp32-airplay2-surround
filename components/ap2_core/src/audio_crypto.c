#include "ap2_audio_crypto.h"

#include <string.h>

#include "sodium.h"

/* The 8-byte nonce at the end of the payload is padded to 12 bytes with
 * leading zeros. */
#define RTP_HEADER_LEN  12u
#define RTP_AAD_OFFSET  4u
#define RTP_AAD_LEN     8u
#define TAG_LEN         crypto_aead_chacha20poly1305_ietf_ABYTES
#define NONCE_TAIL_LEN  8u

static int open_payload(const uint8_t *key, const uint8_t *payload,
                        size_t payload_len, const uint8_t *header,
                        uint8_t *output, size_t output_capacity) {
  uint8_t nonce[crypto_aead_chacha20poly1305_ietf_NPUBBYTES] = {0};
  unsigned long long plain_len = 0;

  if (payload_len < TAG_LEN + NONCE_TAIL_LEN) {
    return -1;
  }
  size_t sealed_len = payload_len - NONCE_TAIL_LEN;
  if (sealed_len - TAG_LEN > output_capacity) {
    return -1;
  }
  memcpy(nonce + sizeof(nonce) - NONCE_TAIL_LEN, payload + sealed_len,
         NONCE_TAIL_LEN);
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          output, &plain_len, NULL, payload, sealed_len,
          header + RTP_AAD_OFFSET, RTP_AAD_LEN, nonce, key) != 0) {
    return -1;
  }
  return (int)plain_len;
}

static int copy_plain(const uint8_t *payload, size_t payload_len,
                      uint8_t *output, size_t output_capacity) {
  if (payload_len > output_capacity) {
    return -1;
  }
  if (payload_len) {
    memcpy(output, payload, payload_len);
  }
  return (int)payload_len;
}

int ap2_audio_decrypt_rtp(const uint8_t *key, const uint8_t *input,
                          size_t input_len, uint8_t *output,
                          size_t output_capacity, const uint8_t *packet,
                          size_t packet_len) {
  if (!input || !output) {
    return -1;
  }
  if (!key) {
    return copy_plain(input, input_len, output, output_capacity);
  }
  /* The payload is part of the packet, after the header. */
  if (!packet || packet_len < RTP_HEADER_LEN + input_len) {
    return -1;
  }
  return open_payload(key, input, input_len, packet, output, output_capacity);
}

int ap2_audio_decrypt_buffered(const uint8_t *key, const uint8_t *packet,
                               size_t packet_len, uint8_t *output,
                               size_t output_capacity) {
  if (!packet || !output || packet_len < RTP_HEADER_LEN) {
    return -1;
  }
  const uint8_t *payload = packet + RTP_HEADER_LEN;
  size_t payload_len = packet_len - RTP_HEADER_LEN;

  if (!key) {
    return payload_len ? copy_plain(payload, payload_len, output, output_capacity)
                       : -1;
  }
  return open_payload(key, payload, payload_len, packet, output,
                      output_capacity);
}
