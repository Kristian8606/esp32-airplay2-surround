#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ap2_hap.h"
#include "esp_err.h"

#define AP2_HAP_ED25519_PUBLIC_KEY_SIZE 32
#define AP2_HAP_ED25519_SECRET_KEY_SIZE 64
#define AP2_HAP_X25519_KEY_SIZE         32
#define AP2_HAP_CHACHA20_KEY_SIZE       32
#define AP2_HAP_POLY1305_TAG_SIZE       16

#define AP2_HAP_VERIFY_M2 2
#define AP2_HAP_VERIFY_M4 4

struct ap2_srp;

struct ap2_hap_session {
  /* Pair-verify: our ephemeral X25519 keys and the client's public key. */
  uint8_t session_public_key[AP2_HAP_X25519_KEY_SIZE];
  uint8_t session_secret_key[AP2_HAP_X25519_KEY_SIZE];
  uint8_t client_public_key[AP2_HAP_X25519_KEY_SIZE];
  uint8_t verify_key[AP2_HAP_CHACHA20_KEY_SIZE];

  /* The result of pairing. */
  uint8_t shared_secret[AP2_HAP_X25519_KEY_SIZE];
  uint8_t encrypt_key[AP2_HAP_CHACHA20_KEY_SIZE];
  uint8_t decrypt_key[AP2_HAP_CHACHA20_KEY_SIZE];
  uint64_t encrypt_nonce;
  uint64_t decrypt_nonce;

  int pair_verify_state;
  int pair_setup_state;
  bool pair_setup_transient;
  bool session_established;

  struct ap2_srp *srp;

  /* HomeKit controller of this connection: set by a full pair-setup (M5),
   * by pair-verify of a stored controller or by the first /pair-add. Empty
   * for transient pairing and for controllers this accessory does not
   * know. */
  char controller_id[64];
  uint8_t controller_id_len;
  uint8_t controller_perm;
  uint8_t controller_ltpk[32];
  bool controller_verified;

  /* Identifier used by M2; HomeKit always uses the paired "pi" identity. */
  bool pv_id_is_pi;
};

/* The identifier this accessory presents in pair-setup M6 and in
 * pair-verify M2: with HomeKit the advertised "pi", which a home learns over
 * /configure, else the MAC. */
const char *ap2_hap_presented_id(bool pi);

/* Same as ap2_hkdf_sha512, for the text salts and infos of the protocol. */
esp_err_t ap2_hap_kdf(const char *salt, const char *info, const uint8_t *ikm,
                      size_t ikm_len, uint8_t *out, size_t out_len);

/* Channel keys of this session from a shared secret. */
esp_err_t ap2_hap_set_control_keys(ap2_hap_session_t *session,
                                   const uint8_t *secret, size_t secret_len,
                                   bool swapped);

/* ChaCha20-Poly1305 with an 8-character message label as the nonce. */
esp_err_t ap2_hap_seal(const uint8_t key[32], const char *label,
                       const uint8_t *plain, size_t plain_len,
                       uint8_t *cipher);
esp_err_t ap2_hap_open(const uint8_t key[32], const char *label,
                       const uint8_t *cipher, size_t cipher_len,
                       uint8_t *plain);

/* This accessory's long-term secret key and its pairing identifier text. */
const uint8_t *ap2_hap_identity_secret_key(void);
const char *ap2_hap_identity_id(void);

/* A {State, Error} reply. */
esp_err_t ap2_hap_write_error(uint8_t state, uint8_t code, uint8_t *output,
                              size_t output_capacity, size_t *output_len);
