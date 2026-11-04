#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hap.h"

/* HKDF-SHA512 (RFC 5869). Returns 0 on success. */
int hap_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                    size_t ikm_len, const uint8_t *info, size_t info_len,
                    uint8_t *okm, size_t okm_len);

/* Same, for the text salts and infos of the pairing protocol. */
esp_err_t hap_kdf(const char *salt, const char *info, const uint8_t *ikm,
                  size_t ikm_len, uint8_t *out, size_t out_len);

/* Control channel keys of this session, from a shared secret. */
esp_err_t hap_set_control_keys(hap_session_t *session, const uint8_t *secret,
                               size_t secret_len, bool swapped);

/* ChaCha20-Poly1305 with an 8-character message label as the nonce. */
esp_err_t hap_seal(const uint8_t key[32], const char *label,
                   const uint8_t *plain, size_t plain_len, uint8_t *cipher);
esp_err_t hap_open(const uint8_t key[32], const char *label,
                   const uint8_t *cipher, size_t cipher_len, uint8_t *plain);

/* This accessory's long-term keys and its pairing identifier text. */
const uint8_t *hap_identity_secret_key(void);
const char *hap_identity_id(void);

/* A {State, Error} reply. */
esp_err_t hap_write_error(uint8_t state, uint8_t code, uint8_t *output,
                          size_t output_capacity, size_t *output_len);
