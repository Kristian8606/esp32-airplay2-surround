#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Server side of SRP-6a with the RFC 5054 3072-bit group, generator 5 and
 * SHA-512, as pair-setup uses it. */

#define AP2_SRP_PRIME_BYTES       384
#define AP2_SRP_SALT_BYTES        16
#define AP2_SRP_PROOF_BYTES       64
#define AP2_SRP_SESSION_KEY_BYTES 64

typedef struct ap2_srp ap2_srp_t;

ap2_srp_t *ap2_srp_new(void);
void ap2_srp_delete(ap2_srp_t *session);

/* Picks a salt and an ephemeral key and computes B. */
esp_err_t ap2_srp_start(ap2_srp_t *session, const char *username,
                        const char *password);

const uint8_t *ap2_srp_salt(ap2_srp_t *session);
const uint8_t *ap2_srp_public_key(ap2_srp_t *session, size_t *len);

/* Checks the client's proof. ESP_OK if the client knows the password;
 * ap2_srp_proof and ap2_srp_session_key are valid from then on. */
esp_err_t ap2_srp_verify_client(ap2_srp_t *session,
                                const uint8_t *client_public_key,
                                size_t client_pk_len, const uint8_t *client_proof,
                                size_t proof_len);

const uint8_t *ap2_srp_proof(ap2_srp_t *session);

/* NULL (and *len = 0) until the client has been verified. */
const uint8_t *ap2_srp_session_key(ap2_srp_t *session, size_t *len);
