#include "audio_crypto.h"

#include <string.h>

#include "sodium.h"

/* An encrypted packet ends with ciphertext, a 16-byte tag and an 8-byte nonce.
 * The nonce is padded to 12 bytes with leading zeros, and bytes 4 to 11 of the
 * RTP header are authenticated. */
#define RTP_HEADER_LEN  12u
#define RTP_AAD_OFFSET  4u
#define RTP_AAD_LEN     8u
#define TAG_LEN         crypto_aead_chacha20poly1305_ietf_ABYTES
#define NONCE_TAIL_LEN  8u

static int open_payload(const audio_encrypt_t *encrypt, const uint8_t *payload,
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
          header + RTP_AAD_OFFSET, RTP_AAD_LEN, nonce, encrypt->key) != 0) {
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

int audio_crypto_decrypt_rtp(const audio_encrypt_t *encrypt,
                             const uint8_t *input, size_t input_len,
                             uint8_t *output, size_t output_capacity,
                             const uint8_t *full_packet,
                             size_t full_packet_len) {
  if (!encrypt || !input || !output) {
    return -1;
  }
  if (encrypt->type == AUDIO_ENCRYPT_NONE) {
    return copy_plain(input, input_len, output, output_capacity);
  }
  /* The payload is part of the packet, after the header. */
  if (encrypt->type != AUDIO_ENCRYPT_CHACHA20_POLY1305 || !full_packet ||
      full_packet_len < RTP_HEADER_LEN + input_len) {
    return -1;
  }
  return open_payload(encrypt, input, input_len, full_packet, output,
                      output_capacity);
}

int audio_crypto_decrypt_buffered(const audio_encrypt_t *encrypt,
                                  const uint8_t *packet, size_t packet_len,
                                  uint8_t *output, size_t output_capacity) {
  if (!encrypt || !packet || !output || packet_len < RTP_HEADER_LEN) {
    return -1;
  }
  const uint8_t *payload = packet + RTP_HEADER_LEN;
  size_t payload_len = packet_len - RTP_HEADER_LEN;

  if (encrypt->type == AUDIO_ENCRYPT_NONE) {
    return payload_len ? copy_plain(payload, payload_len, output, output_capacity)
                       : -1;
  }
  if (encrypt->type != AUDIO_ENCRYPT_CHACHA20_POLY1305) {
    return -1;
  }
  return open_payload(encrypt, payload, payload_len, packet, output,
                      output_capacity);
}
