#pragma once

#include <stddef.h>
#include <stdint.h>

/* AirPlay 2 audio packets sealed with ChaCha20-Poly1305: the payload ends
 * with the 16-byte tag and an 8-byte nonce, and bytes 4 to 11 of the RTP
 * header are authenticated. key is the stream's 32-byte key, or NULL for a
 * stream that is not encrypted (the payload is copied).
 *
 * Each returns the length of the audio written, or -1 if the packet is too
 * short, does not open with the key, or does not fit the output. */

/* A realtime (type 96) packet: input is its payload, packet the whole
 * packet with its 12-byte RTP header. */
int ap2_audio_decrypt_rtp(const uint8_t *key, const uint8_t *input,
                          size_t input_len, uint8_t *output,
                          size_t output_capacity, const uint8_t *packet,
                          size_t packet_len);

/* A buffered (type 103) packet: the 12-byte RTP header and the payload. An
 * empty payload is an error. */
int ap2_audio_decrypt_buffered(const uint8_t *key, const uint8_t *packet,
                               size_t packet_len, uint8_t *output,
                               size_t output_capacity);
