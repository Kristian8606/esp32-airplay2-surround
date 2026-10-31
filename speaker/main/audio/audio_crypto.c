#include <string.h>

#include "audio_crypto.h"

#include "sodium.h"

int audio_crypto_decrypt_rtp(const audio_encrypt_t *encrypt,
                             const uint8_t *input, size_t input_len,
                             uint8_t *output, size_t output_capacity,
                             const uint8_t *full_packet,
                             size_t full_packet_len) {
  if (!encrypt || !input || !output) {
    return -1;
  }

  if (encrypt->type == AUDIO_ENCRYPT_NONE) {
    if (input_len > output_capacity) {
      return -1;
    }
    memcpy(output, input, input_len);
    return (int)input_len;
  }

  if (encrypt->type == AUDIO_ENCRYPT_CHACHA20_POLY1305) {
    // AirPlay 2 RTP: nonce = 4 zero bytes + last 8 bytes of packet,
    // AAD = RTP timestamp + SSRC (bytes 4-11 of the full packet).
    if (!full_packet || full_packet_len < 12) {
      return -1;
    }

    if (input_len < crypto_aead_chacha20poly1305_ietf_ABYTES + 8) {
      return -1;
    }

    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, full_packet + full_packet_len - 8, 8);

    const uint8_t *aad = full_packet + 4;
    size_t aad_len = 8;

    size_t ciphertext_len = input_len - 8;
    if (ciphertext_len - crypto_aead_chacha20poly1305_ietf_ABYTES >
        output_capacity) return -1;

    unsigned long long decrypted_len = 0;
    int ret = crypto_aead_chacha20poly1305_ietf_decrypt(
        output, &decrypted_len, NULL, input, ciphertext_len, aad, aad_len,
        nonce, encrypt->key);
    if (ret != 0) {
      return -1;
    }

    return (int)decrypted_len;
  }

  return -1;
}

int audio_crypto_decrypt_buffered(const audio_encrypt_t *encrypt,
                                  const uint8_t *packet, size_t packet_len,
                                  uint8_t *output, size_t output_capacity) {
  if (!packet || !output) {
    return -1;
  }

  if (!encrypt || encrypt->type != AUDIO_ENCRYPT_CHACHA20_POLY1305) {
    if (packet_len <= 12) {
      return -1;
    }
    size_t payload_len = packet_len - 12;
    if (payload_len > output_capacity) {
      return -1;
    }
    memcpy(output, packet + 12, payload_len);
    return (int)payload_len;
  }

  if (packet_len < 36) {
    return -1;
  }

  // Buffered audio: AAD is bytes 4-11 (timestamp + SSRC), nonce in last 8
  // bytes.
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, packet + packet_len - 8, 8);

  const uint8_t *aad = packet + 4;
  size_t aad_len = 8;

  const uint8_t *ciphertext = packet + 12;
  size_t ciphertext_len = packet_len - 12 - 8;

  if (ciphertext_len - crypto_aead_chacha20poly1305_ietf_ABYTES >
      output_capacity) {
    return -1;
  }

  unsigned long long decrypted_len = 0;
  int ret = crypto_aead_chacha20poly1305_ietf_decrypt(
      output, &decrypted_len, NULL, ciphertext, ciphertext_len, aad, aad_len,
      nonce, encrypt->key);
  if (ret != 0) {
    return -1;
  }

  return (int)decrypted_len;
}
