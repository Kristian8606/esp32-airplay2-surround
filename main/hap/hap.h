#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* AirPlay 2 pairing: pair-setup (SRP, usually transient) and pair-verify,
 * and the keys of the encrypted control channel they produce. */

#define HAP_ED25519_PUBLIC_KEY_SIZE 32
#define HAP_ED25519_SECRET_KEY_SIZE 64
#define HAP_X25519_KEY_SIZE         32
#define HAP_CHACHA20_KEY_SIZE       32
#define HAP_CHACHA20_NONCE_SIZE     12
#define HAP_POLY1305_TAG_SIZE       16

struct srp_session;

typedef struct {
  /* Pair-verify: our ephemeral X25519 keys and the client's public key. */
  uint8_t session_public_key[HAP_X25519_KEY_SIZE];
  uint8_t session_secret_key[HAP_X25519_KEY_SIZE];
  uint8_t client_public_key[HAP_X25519_KEY_SIZE];
  uint8_t verify_key[HAP_CHACHA20_KEY_SIZE];

  /* The result of pairing. */
  uint8_t shared_secret[HAP_X25519_KEY_SIZE];
  uint8_t encrypt_key[HAP_CHACHA20_KEY_SIZE];
  uint8_t decrypt_key[HAP_CHACHA20_KEY_SIZE];
  uint64_t encrypt_nonce;
  uint64_t decrypt_nonce;

  int pair_verify_state;
  int pair_setup_state;
  bool pair_setup_transient;
  bool session_established;

  struct srp_session *srp;
} hap_session_t;

/* Loads the accessory's Ed25519 identity from NVS, or creates it. */
esp_err_t hap_init(void);
const uint8_t *hap_get_public_key(void);

hap_session_t *hap_session_create(void);
void hap_session_free(hap_session_t *session);

/* Each step takes the client's message and writes the reply. A refused
 * client gets ESP_OK with a {State, Error} reply when the message itself was
 * well-formed; other errors leave the reply to the caller. */
esp_err_t hap_pair_setup_m1(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len);
esp_err_t hap_pair_setup_m3(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len);
esp_err_t hap_pair_setup_m5(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len);

esp_err_t hap_pair_verify_m1(hap_session_t *session, const uint8_t *input,
                             size_t input_len, uint8_t *output,
                             size_t output_capacity, size_t *output_len);
esp_err_t hap_pair_verify_m3(hap_session_t *session, const uint8_t *input,
                             size_t input_len, uint8_t *output,
                             size_t output_capacity, size_t *output_len);

/* The older form of pair-verify without TLV: M1 is flags, the client's X25519
 * key and its Ed25519 key. It does not encrypt the RTSP channel. */
esp_err_t hap_pair_verify_m1_raw(hap_session_t *session, const uint8_t *input,
                                 size_t input_len, uint8_t *output,
                                 size_t output_capacity, size_t *output_len);
esp_err_t hap_pair_verify_m3_raw(hap_session_t *session, const uint8_t *input,
                                 size_t input_len, uint8_t *output,
                                 size_t output_capacity, size_t *output_len);

/* Audio key from the shared secret of an established session. */
esp_err_t hap_derive_audio_key(hap_session_t *session, uint8_t *audio_key,
                               size_t key_len);
