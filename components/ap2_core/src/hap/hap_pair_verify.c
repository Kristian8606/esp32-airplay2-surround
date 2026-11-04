#include <string.h>

#include "aes/esp_aes.h"
#include "esp_log.h"
#include "ap2_hap.h"
#include "hap_private.h"
#include "sodium.h"
#include "ap2_tlv8.h"

static const char *TAG = "hap_verify";

#define RAW_HEADER_LEN  4u /* flags before the keys of a raw message */
#define RAW_SIG_LEN     crypto_sign_BYTES
#define MAX_M3_CIPHER   512u

/* New ephemeral key and the shared secret with the client's key. */
static esp_err_t agree_on_secret(ap2_hap_session_t *session,
                                 const uint8_t client_public[32]) {
  memcpy(session->client_public_key, client_public, AP2_HAP_X25519_KEY_SIZE);
  if (crypto_box_keypair(session->session_public_key,
                         session->session_secret_key) != 0) {
    return ESP_FAIL;
  }
  /* Fails for a low-order client key, which would make the secret known. */
  if (crypto_scalarmult(session->shared_secret, session->session_secret_key,
                        session->client_public_key) != 0) {
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t ap2_hap_verify_m1(ap2_hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  uint8_t client_public[AP2_HAP_X25519_KEY_SIZE];
  size_t client_public_len = 0;
  uint8_t signed_part[2 * AP2_HAP_X25519_KEY_SIZE + 17];
  uint8_t signature[crypto_sign_BYTES];
  uint8_t inner[64 + crypto_sign_BYTES];
  uint8_t cipher[sizeof(inner) + AP2_HAP_POLY1305_TAG_SIZE];
  ap2_tlv8_writer_t enc;
  esp_err_t err;

  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!ap2_tlv8_get(input, input_len, AP2_TLV_PUBLIC_KEY, client_public,
                    sizeof(client_public), &client_public_len) ||
      client_public_len != sizeof(client_public)) {
    return ESP_ERR_INVALID_ARG;
  }

  err = agree_on_secret(session, client_public);
  if (err != ESP_OK) {
    return err;
  }

  /* We sign (our key | our id | client's key). */
  const char *id = ap2_hap_identity_id();
  size_t id_len = strlen(id);
  memcpy(signed_part, session->session_public_key, AP2_HAP_X25519_KEY_SIZE);
  memcpy(signed_part + AP2_HAP_X25519_KEY_SIZE, id, id_len);
  memcpy(signed_part + AP2_HAP_X25519_KEY_SIZE + id_len, session->client_public_key,
         AP2_HAP_X25519_KEY_SIZE);
  crypto_sign_detached(signature, NULL, signed_part,
                       2 * AP2_HAP_X25519_KEY_SIZE + id_len,
                       ap2_hap_identity_secret_key());

  ap2_tlv8_writer_init(&enc, inner, sizeof(inner));
  if (!ap2_tlv8_put(&enc, AP2_TLV_IDENTIFIER, (const uint8_t *)id, id_len) ||
      !ap2_tlv8_put(&enc, AP2_TLV_SIGNATURE, signature, sizeof(signature))) {
    return ESP_ERR_INVALID_SIZE;
  }
  size_t inner_len = ap2_tlv8_writer_size(&enc);

  err = ap2_hap_kdf("Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info",
                    session->shared_secret, sizeof(session->shared_secret),
                    session->verify_key, sizeof(session->verify_key));
  if (err != ESP_OK) {
    return err;
  }
  err = ap2_hap_seal(session->verify_key, "PV-Msg02", inner, inner_len, cipher);
  if (err != ESP_OK) {
    return err;
  }

  ap2_tlv8_writer_init(&enc, output, output_capacity);
  if (!ap2_tlv8_put_byte(&enc, AP2_TLV_STATE, AP2_HAP_VERIFY_M2) ||
      !ap2_tlv8_put(&enc, AP2_TLV_PUBLIC_KEY, session->session_public_key,
                    AP2_HAP_X25519_KEY_SIZE) ||
      !ap2_tlv8_put(&enc, AP2_TLV_ENCRYPTED_DATA, cipher,
                    inner_len + AP2_HAP_POLY1305_TAG_SIZE)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = ap2_tlv8_writer_size(&enc);
  session->pair_verify_state = AP2_HAP_VERIFY_M2;
  return ESP_OK;
}

esp_err_t ap2_hap_verify_m3(ap2_hap_session_t *session, const uint8_t *input,
                            size_t input_len, uint8_t *output,
                            size_t output_capacity, size_t *output_len) {
  uint8_t cipher[MAX_M3_CIPHER], plain[MAX_M3_CIPHER];
  size_t cipher_len = 0;
  ap2_tlv8_writer_t enc;

  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }

  /* The message is authenticated by the key from M2. Its signature is not
   * checked: no client keys are stored, so there is nothing to check it
   * against. Pairing does not limit who may connect. */
  if (session->pair_verify_state != AP2_HAP_VERIFY_M2 ||
      !ap2_tlv8_get(input, input_len, AP2_TLV_ENCRYPTED_DATA, cipher,
                    sizeof(cipher), &cipher_len) ||
      cipher_len < AP2_HAP_POLY1305_TAG_SIZE ||
      ap2_hap_open(session->verify_key, "PV-Msg03", cipher, cipher_len, plain) !=
      ESP_OK) {
    ap2_hap_write_error(AP2_HAP_VERIFY_M4, AP2_TLV_ERROR_AUTHENTICATION, output,
                        output_capacity, output_len);
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = ap2_hap_set_control_keys(session, session->shared_secret,
                                           sizeof(session->shared_secret), false);
  if (err != ESP_OK) {
    return err;
  }

  ap2_tlv8_writer_init(&enc, output, output_capacity);
  if (!ap2_tlv8_put_byte(&enc, AP2_TLV_STATE, AP2_HAP_VERIFY_M4)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = ap2_tlv8_writer_size(&enc);
  session->pair_verify_state = AP2_HAP_VERIFY_M4;
  session->session_established = true;
  return ESP_OK;
}

/* AES-128-CTR key and IV of the raw form: SHA-512 of a label and the secret,
 * first 16 bytes. */
static void raw_aes_part(const char *label, const uint8_t *secret,
                         uint8_t out[16]) {
  uint8_t digest[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state st;

  crypto_hash_sha512_init(&st);
  crypto_hash_sha512_update(&st, (const uint8_t *)label, strlen(label));
  crypto_hash_sha512_update(&st, secret, AP2_HAP_X25519_KEY_SIZE);
  crypto_hash_sha512_final(&st, digest);
  memcpy(out, digest, 16);
  sodium_memzero(digest, sizeof(digest));
}

esp_err_t ap2_hap_verify_raw_m1(ap2_hap_session_t *session, const uint8_t *input,
                                size_t input_len, uint8_t *output,
                                size_t output_capacity, size_t *output_len) {
  uint8_t signed_part[2 * AP2_HAP_X25519_KEY_SIZE], signature[RAW_SIG_LEN];
  uint8_t aes_key[16], aes_iv[16], stream_block[16];
  size_t stream_offset = 0;
  esp_aes_context aes;
  esp_err_t err;

  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }
  if (input_len < RAW_HEADER_LEN + AP2_HAP_X25519_KEY_SIZE ||
      output_capacity < AP2_HAP_X25519_KEY_SIZE + RAW_SIG_LEN) {
    return ESP_ERR_INVALID_SIZE;
  }

  err = agree_on_secret(session, input + RAW_HEADER_LEN);
  if (err != ESP_OK) {
    return err;
  }

  memcpy(signed_part, session->session_public_key, AP2_HAP_X25519_KEY_SIZE);
  memcpy(signed_part + AP2_HAP_X25519_KEY_SIZE, session->client_public_key,
         AP2_HAP_X25519_KEY_SIZE);
  crypto_sign_detached(signature, NULL, signed_part, sizeof(signed_part),
                       ap2_hap_identity_secret_key());

  raw_aes_part("Pair-Verify-AES-Key", session->shared_secret, aes_key);
  raw_aes_part("Pair-Verify-AES-IV", session->shared_secret, aes_iv);

  memcpy(output, session->session_public_key, AP2_HAP_X25519_KEY_SIZE);
  esp_aes_init(&aes);
  int rc = esp_aes_setkey(&aes, aes_key, 128);
  if (rc == 0) {
    rc = esp_aes_crypt_ctr(&aes, RAW_SIG_LEN, &stream_offset, aes_iv,
                           stream_block, signature,
                           output + AP2_HAP_X25519_KEY_SIZE);
  }
  esp_aes_free(&aes);
  sodium_memzero(aes_key, sizeof(aes_key));
  if (rc != 0) {
    ESP_LOGE(TAG, "AES error %d", rc);
    return ESP_FAIL;
  }

  *output_len = AP2_HAP_X25519_KEY_SIZE + RAW_SIG_LEN;
  session->pair_verify_state = AP2_HAP_VERIFY_M2;
  return ESP_OK;
}

esp_err_t ap2_hap_verify_raw_m3(ap2_hap_session_t *session, const uint8_t *input,
                                size_t input_len, uint8_t *output,
                                size_t output_capacity, size_t *output_len) {
  (void)output;
  (void)output_capacity;

  if (!session || !input || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }
  if (session->pair_verify_state != AP2_HAP_VERIFY_M2) {
    return ESP_ERR_INVALID_STATE;
  }
  if (input_len < RAW_HEADER_LEN + RAW_SIG_LEN) {
    return ESP_ERR_INVALID_ARG;
  }

  /* As in ap2_hap_verify_m3, the client's signature is not checked. This
   * form leaves RTSP in the clear, so these keys stay unused; the shared
   * secret is what SETUP needs. */
  esp_err_t err = ap2_hap_set_control_keys(session, session->shared_secret,
                                           sizeof(session->shared_secret), true);
  if (err != ESP_OK) {
    return err;
  }
  *output_len = 0;
  session->pair_verify_state = AP2_HAP_VERIFY_M4;
  session->session_established = true;
  return ESP_OK;
}
