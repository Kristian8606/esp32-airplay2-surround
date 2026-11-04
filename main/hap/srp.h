#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Server side of SRP-6a with the RFC 5054 3072-bit group, generator 5 and
 * SHA-512, as pair-setup uses it. */

#define SRP_PRIME_BYTES       384
#define SRP_SALT_BYTES        16
#define SRP_PROOF_BYTES       64
#define SRP_SESSION_KEY_BYTES 64

typedef struct srp_session srp_session_t;

srp_session_t *srp_session_create(void);
void srp_session_free(srp_session_t *session);

/* Picks a salt and an ephemeral key and computes B. */
esp_err_t srp_start(srp_session_t *session, const char *username,
                    const char *password);

const uint8_t *srp_get_salt(srp_session_t *session);
const uint8_t *srp_get_public_key(srp_session_t *session, size_t *len);

/* Checks the client's proof. ESP_OK if the client knows the password;
 * srp_get_proof and srp_get_session_key are valid from then on. */
esp_err_t srp_verify_client(srp_session_t *session,
                            const uint8_t *client_public_key,
                            size_t client_pk_len, const uint8_t *client_proof,
                            size_t proof_len);

const uint8_t *srp_get_proof(srp_session_t *session);

/* NULL (and *len = 0) until the client has been verified. */
const uint8_t *srp_get_session_key(srp_session_t *session, size_t *len);
