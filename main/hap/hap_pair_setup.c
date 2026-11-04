#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "hap.h"
#include "hap_internal.h"
#include "sodium.h"
#include "srp.h"
#include "tlv8.h"

static const char *TAG = "hap_setup";

#define SETUP_USERNAME "Pair-Setup"
#define SETUP_PASSWORD "3939"

#define SETUP_STATE_IDLE 0
#define SETUP_STATE_M2   2
#define SETUP_STATE_M4   4
#define SETUP_STATE_M6   6

#define MAX_ID_LEN      64
#define MAX_M5_PLAIN    (2u * (MAX_ID_LEN + 32 + 64) + 16)
#define MAX_M6_PLAIN    (MAX_ID_LEN + 32 + 64 + 6)

static bool wants_transient(const uint8_t *input, size_t input_len) {
  size_t len = 0;
  const uint8_t *flags = tlv8_find(input, input_len, TLV_TYPE_FLAGS, &len);
  uint32_t value = 0;

  if (!flags || len > sizeof(value)) {
    return false;
  }
  for (size_t i = 0; i < len; i++) { /* little-endian */
    value |= (uint32_t)flags[i] << (8u * i);
  }
  return (value & TLV_FLAG_TRANSIENT) != 0;
}

esp_err_t hap_pair_setup_m1(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  size_t pub_len = 0;
  tlv8_encoder_t enc;
  esp_err_t err;

  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }

  /* A new attempt replaces whatever this connection did before. */
  srp_session_free(session->srp);
  session->srp = srp_session_create();
  session->pair_setup_state = SETUP_STATE_IDLE;
  session->session_established = false;
  if (!session->srp) {
    return ESP_ERR_NO_MEM;
  }
  session->pair_setup_transient = wants_transient(input, input_len);

  err = srp_start(session->srp, SETUP_USERNAME, SETUP_PASSWORD);
  if (err != ESP_OK) {
    return err;
  }

  const uint8_t *pub = srp_get_public_key(session->srp, &pub_len);
  tlv8_encoder_init(&enc, output, output_capacity);
  if (!tlv8_encode_byte(&enc, TLV_TYPE_STATE, SETUP_STATE_M2) ||
      !tlv8_encode(&enc, TLV_TYPE_SALT, srp_get_salt(session->srp),
                   SRP_SALT_BYTES) ||
      !tlv8_encode(&enc, TLV_TYPE_PUBLIC_KEY, pub, pub_len)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = SETUP_STATE_M2;
  return ESP_OK;
}

esp_err_t hap_pair_setup_m3(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  uint8_t client_pub[SRP_PRIME_BYTES], proof[SRP_PROOF_BYTES];
  size_t client_pub_len = 0, proof_len = 0;
  tlv8_encoder_t enc;

  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!session->srp || session->pair_setup_state != SETUP_STATE_M2) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!tlv8_decode_concat(input, input_len, TLV_TYPE_PUBLIC_KEY, client_pub,
                          sizeof(client_pub), &client_pub_len) ||
      !tlv8_decode_concat(input, input_len, TLV_TYPE_PROOF, proof,
                          sizeof(proof), &proof_len) ||
      client_pub_len == 0 || proof_len != sizeof(proof)) {
    return ESP_ERR_INVALID_ARG;
  }

  if (srp_verify_client(session->srp, client_pub, client_pub_len, proof,
                        proof_len) != ESP_OK) {
    ESP_LOGW(TAG, "client proof refused");
    return hap_write_error(SETUP_STATE_M4, TLV_ERROR_AUTHENTICATION, output,
                           output_capacity, output_len);
  }

  tlv8_encoder_init(&enc, output, output_capacity);
  if (!tlv8_encode_byte(&enc, TLV_TYPE_STATE, SETUP_STATE_M4) ||
      !tlv8_encode(&enc, TLV_TYPE_PROOF, srp_get_proof(session->srp),
                   SRP_PROOF_BYTES)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = SETUP_STATE_M4;

  /* Transient pairing ends here: the SRP key is the session's secret. */
  if (session->pair_setup_transient) {
    size_t key_len = 0;
    const uint8_t *key = srp_get_session_key(session->srp, &key_len);
    esp_err_t err = hap_set_control_keys(session, key, key_len, false);
    if (err != ESP_OK) {
      return err;
    }
    memcpy(session->shared_secret, key, sizeof(session->shared_secret));
    session->session_established = true;
  }
  return ESP_OK;
}

/* Checks the client's M5 and fills M6. ESP_OK or an authentication failure. */
static esp_err_t exchange_identities(hap_session_t *session, const uint8_t *key,
                                     size_t key_len, const uint8_t *enc_key,
                                     const uint8_t *cipher, size_t cipher_len,
                                     uint8_t *output, size_t output_capacity,
                                     size_t *output_len) {
  uint8_t plain[MAX_M5_PLAIN], signing_salt[32];
  uint8_t info[32 + MAX_ID_LEN + HAP_ED25519_PUBLIC_KEY_SIZE];
  uint8_t client_id[MAX_ID_LEN], client_pk[HAP_ED25519_PUBLIC_KEY_SIZE];
  uint8_t client_sig[crypto_sign_BYTES], own_sig[crypto_sign_BYTES];
  uint8_t reply[MAX_M6_PLAIN + HAP_POLY1305_TAG_SIZE];
  size_t id_len = 0, pk_len = 0, sig_len = 0;
  tlv8_encoder_t enc;

  if (cipher_len < HAP_POLY1305_TAG_SIZE ||
      cipher_len - HAP_POLY1305_TAG_SIZE > sizeof(plain) ||
      hap_open(enc_key, "PS-Msg05", cipher, cipher_len, plain) != ESP_OK) {
    return ESP_FAIL;
  }
  size_t plain_len = cipher_len - HAP_POLY1305_TAG_SIZE;
  if (!tlv8_decode_concat(plain, plain_len, TLV_TYPE_IDENTIFIER, client_id,
                          sizeof(client_id), &id_len) ||
      !tlv8_decode_concat(plain, plain_len, TLV_TYPE_PUBLIC_KEY, client_pk,
                          sizeof(client_pk), &pk_len) ||
      !tlv8_decode_concat(plain, plain_len, TLV_TYPE_SIGNATURE, client_sig,
                          sizeof(client_sig), &sig_len) ||
      pk_len != sizeof(client_pk) || sig_len != sizeof(client_sig)) {
    return ESP_FAIL;
  }

  /* The client signs (derived X | its id | its public key). */
  if (hap_kdf("Pair-Setup-Controller-Sign-Salt",
              "Pair-Setup-Controller-Sign-Info", key, key_len, signing_salt,
              sizeof(signing_salt)) != ESP_OK) {
    return ESP_FAIL;
  }
  memcpy(info, signing_salt, 32);
  memcpy(info + 32, client_id, id_len);
  memcpy(info + 32 + id_len, client_pk, pk_len);
  if (crypto_sign_verify_detached(client_sig, info, 32 + id_len + pk_len,
                                  client_pk) != 0) {
    return ESP_FAIL;
  }

  /* We answer in the same form. */
  const char *own_id = hap_identity_id();
  size_t own_id_len = strlen(own_id);
  if (hap_kdf("Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info",
              key, key_len, signing_salt, sizeof(signing_salt)) != ESP_OK) {
    return ESP_FAIL;
  }
  memcpy(info, signing_salt, 32);
  memcpy(info + 32, own_id, own_id_len);
  memcpy(info + 32 + own_id_len, hap_get_public_key(),
         HAP_ED25519_PUBLIC_KEY_SIZE);
  crypto_sign_detached(own_sig, NULL, info,
                       32 + own_id_len + HAP_ED25519_PUBLIC_KEY_SIZE,
                       hap_identity_secret_key());

  tlv8_encoder_init(&enc, plain, sizeof(plain));
  if (!tlv8_encode(&enc, TLV_TYPE_IDENTIFIER, (const uint8_t *)own_id,
                   own_id_len) ||
      !tlv8_encode(&enc, TLV_TYPE_PUBLIC_KEY, hap_get_public_key(),
                   HAP_ED25519_PUBLIC_KEY_SIZE) ||
      !tlv8_encode(&enc, TLV_TYPE_SIGNATURE, own_sig, sizeof(own_sig))) {
    return ESP_FAIL;
  }
  size_t reply_plain = tlv8_encoder_size(&enc);
  if (hap_seal(enc_key, "PS-Msg06", plain, reply_plain, reply) != ESP_OK) {
    return ESP_FAIL;
  }

  tlv8_encoder_init(&enc, output, output_capacity);
  if (!tlv8_encode_byte(&enc, TLV_TYPE_STATE, SETUP_STATE_M6) ||
      !tlv8_encode(&enc, TLV_TYPE_ENCRYPTED_DATA, reply,
                   reply_plain + HAP_POLY1305_TAG_SIZE)) {
    return ESP_FAIL;
  }
  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = SETUP_STATE_M6;
  return ESP_OK;
}

esp_err_t hap_pair_setup_m5(hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!session->srp || session->pair_setup_state != SETUP_STATE_M4) {
    return ESP_ERR_INVALID_STATE;
  }

  size_t key_len = 0, cipher_len = 0;
  const uint8_t *key = srp_get_session_key(session->srp, &key_len);
  uint8_t enc_key[HAP_CHACHA20_KEY_SIZE];
  size_t cipher_cap = MAX_M5_PLAIN + HAP_POLY1305_TAG_SIZE;
  uint8_t *cipher = malloc(cipher_cap);
  esp_err_t err;

  if (!cipher) {
    return ESP_ERR_NO_MEM;
  }
  if (!key ||
      !tlv8_decode_concat(input, input_len, TLV_TYPE_ENCRYPTED_DATA, cipher,
                          cipher_cap, &cipher_len)) {
    free(cipher);
    return ESP_ERR_INVALID_ARG;
  }

  err = hap_kdf("Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", key,
                key_len, enc_key, sizeof(enc_key));
  if (err == ESP_OK) {
    err = exchange_identities(session, key, key_len, enc_key, cipher, cipher_len,
                              output, output_capacity, output_len);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "client identity refused");
      err = hap_write_error(SETUP_STATE_M6, TLV_ERROR_AUTHENTICATION, output,
                            output_capacity, output_len);
    }
  }
  sodium_memzero(enc_key, sizeof(enc_key));
  free(cipher);
  return err;
}
