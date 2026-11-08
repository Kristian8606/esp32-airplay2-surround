#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* AirPlay 2 pairing of an accessory: its long-term identity, pair-setup
 * (SRP, transient or persistent) and pair-verify, and the keys a paired
 * session has for the encrypted RTSP channel, the event channel and the
 * audio. */

/* ---- Identity ---- */

/* Loads the Ed25519 identity from NVS, or creates and stores it. Call once
 * before any session. */
esp_err_t ap2_hap_identity_init(void);
/* 32 bytes. */
const uint8_t *ap2_hap_identity_public_key(void);

/* ---- Session ---- */

typedef struct ap2_hap_session ap2_hap_session_t;

/* NULL when out of memory. */
ap2_hap_session_t *ap2_hap_session_new(void);
/* Wipes the keys. NULL is allowed. */
void ap2_hap_session_delete(ap2_hap_session_t *session);

/* Each step takes the client's TLV8 message and writes the reply. A refused
 * client gets ESP_OK with a {State, Error} reply when its message was
 * well-formed; any other error leaves the reply to the caller. */
esp_err_t ap2_hap_setup_m1(ap2_hap_session_t *session, const uint8_t *input,
                           size_t input_len, uint8_t *output,
                           size_t output_capacity, size_t *output_len);
esp_err_t ap2_hap_setup_m3(ap2_hap_session_t *session, const uint8_t *input,
                           size_t input_len, uint8_t *output,
                           size_t output_capacity, size_t *output_len);
esp_err_t ap2_hap_setup_m5(ap2_hap_session_t *session, const uint8_t *input,
                           size_t input_len, uint8_t *output,
                           size_t output_capacity, size_t *output_len);

esp_err_t ap2_hap_verify_m1(ap2_hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len);
esp_err_t ap2_hap_verify_m3(ap2_hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len);

/* The older pair-verify without TLV8: M1 is 4 bytes of flags, the client's
 * X25519 key and its Ed25519 key. It does not encrypt the RTSP channel. */
esp_err_t ap2_hap_verify_raw_m1(ap2_hap_session_t *session,
                                const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t output_capacity,
                                size_t *output_len);
esp_err_t ap2_hap_verify_raw_m3(ap2_hap_session_t *session,
                                const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t output_capacity,
                                size_t *output_len);

/* The number of the last pair-setup or pair-verify reply sent (2, 4 or 6),
 * or 0 before the first. */
int ap2_hap_setup_state(const ap2_hap_session_t *session);
int ap2_hap_verify_state(const ap2_hap_session_t *session);

/* Pairing ended with keys: transient pair-setup or pair-verify. */
bool ap2_hap_established(const ap2_hap_session_t *session);
/* The last pair-setup was transient. */
bool ap2_hap_setup_transient(const ap2_hap_session_t *session);

/* ---- HomeKit controller of the session ----
 *
 * Set by a full pair-setup, by pair-verify of a stored controller or with
 * ap2_hap_set_controller(). False (and nothing written) when the session has
 * no verified controller. id is not NUL-terminated. */
bool ap2_hap_controller(const ap2_hap_session_t *session, const char **id,
                        size_t *id_len, uint8_t *perm);
/* A formerly verified controller has been removed or its stored key changed. */
bool ap2_hap_controller_revoked(const ap2_hap_session_t *session);
void ap2_hap_set_controller(ap2_hap_session_t *session, const uint8_t *id,
                            size_t id_len, uint8_t perm);

/* ---- Keys of an established session ---- */

/* 32 bytes: the X25519 secret of pair-verify, or after transient pair-setup
 * the start of the SRP session key. */
const uint8_t *ap2_hap_shared_secret(const ap2_hap_session_t *session);

/* The whole secret pairing ended with: the 64-byte SRP session key after
 * transient pair-setup, else the 32-byte X25519 secret. NULL (and *len = 0)
 * before the session is established. */
const uint8_t *ap2_hap_pairing_secret(const ap2_hap_session_t *session,
                                      size_t *len);

/* The RTSP channel: ChaCha20-Poly1305 with a 32-byte key and a 64-bit
 * message counter per direction, both starting at 0 when pairing ends. */
typedef enum {
  AP2_HAP_SEND = 0,
  AP2_HAP_RECEIVE = 1,
} ap2_hap_direction_t;

const uint8_t *ap2_hap_channel_key(const ap2_hap_session_t *session,
                                   ap2_hap_direction_t direction);
uint64_t ap2_hap_channel_counter(const ap2_hap_session_t *session,
                                 ap2_hap_direction_t direction);
/* After each message sent or received. */
void ap2_hap_channel_count(ap2_hap_session_t *session,
                           ap2_hap_direction_t direction);

/* The audio key, derived from the shared secret. */
esp_err_t ap2_hap_audio_key(const ap2_hap_session_t *session,
                            uint8_t *audio_key, size_t key_len);

/* HKDF-SHA512 (RFC 5869). Returns 0 on success. */
int ap2_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                    size_t ikm_len, const uint8_t *info, size_t info_len,
                    uint8_t *okm, size_t okm_len);
