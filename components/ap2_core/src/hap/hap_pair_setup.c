#include <stdlib.h>
#include <string.h>

#include "airplay_identity.h"
#include "ap2_pairings.h"
#include "esp_log.h"
#include "ap2_hap.h"
#include "hap_private.h"
#include "sodium.h"
#include "srp.h"
#include "ap2_tlv8.h"

static const char *TAG = "hap_setup";

#define SETUP_USERNAME "Pair-Setup"
/* Transient pairing always uses the fixed AirPlay code. */
#define SETUP_PASSWORD "3939"
#ifdef CONFIG_AIRPLAY_HOMEKIT
#define SETUP_PASSWORD_FULL CONFIG_AIRPLAY_HOMEKIT_SETUP_CODE
#else
#define SETUP_PASSWORD_FULL SETUP_PASSWORD
#endif

#define SETUP_STATE_IDLE 0
#define SETUP_STATE_M2   2
#define SETUP_STATE_M4   4
#define SETUP_STATE_M6   6

#define MAX_ID_LEN      64
#define MAX_M5_PLAIN    (2u * (MAX_ID_LEN + 32 + 64) + 16)
#define MAX_M6_PLAIN    (MAX_ID_LEN + 32 + 64 + 6)

static bool wants_transient(const uint8_t *input, size_t input_len) {
  size_t len = 0;
  const uint8_t *flags = ap2_tlv8_find(input, input_len, AP2_TLV_FLAGS, &len);
  uint32_t value = 0;

  if (!flags || len > sizeof(value)) {
    return false;
  }
  for (size_t i = 0; i < len; i++) { /* little-endian */
    value |= (uint32_t)flags[i] << (8u * i);
  }
  return (value & AP2_TLV_FLAG_TRANSIENT) != 0;
}

esp_err_t ap2_hap_setup_m1(ap2_hap_session_t *session, const uint8_t *input,
                           size_t input_len, uint8_t *output,
                           size_t output_capacity, size_t *output_len) {
  size_t pub_len = 0;
  ap2_tlv8_writer_t enc;
  esp_err_t err;

  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }

#ifdef CONFIG_AIRPLAY_HOMEKIT
  if (!wants_transient(input, input_len) && ap2_pairings_has_admin()) {
    ESP_LOGW(TAG, "HomeKit pair-setup unavailable: an administrator is still paired");
    return ap2_hap_write_error(SETUP_STATE_M2, AP2_TLV_ERROR_UNAVAILABLE,
                               output, output_capacity, output_len);
  }
#endif
  session->controller_verified = false;
  session->session_established = false;
  /* A new attempt replaces whatever this connection did before. */
  ap2_srp_delete(session->srp);
  session->srp = ap2_srp_new();
  session->pair_setup_state = SETUP_STATE_IDLE;
  session->session_established = false;
  if (!session->srp) {
    return ESP_ERR_NO_MEM;
  }
  session->pair_setup_transient = wants_transient(input, input_len);
  ESP_LOGI(TAG, "pair-setup M1: %s pairing",
           session->pair_setup_transient ? "transient" : "full (HomeKit)");

  err = ap2_srp_start(session->srp, SETUP_USERNAME,
                      session->pair_setup_transient ? SETUP_PASSWORD
                                                    : SETUP_PASSWORD_FULL);
  if (err != ESP_OK) {
    return err;
  }

  const uint8_t *pub = ap2_srp_public_key(session->srp, &pub_len);
  ap2_tlv8_writer_init(&enc, output, output_capacity);
  if (!ap2_tlv8_put_byte(&enc, AP2_TLV_STATE, SETUP_STATE_M2) ||
      !ap2_tlv8_put(&enc, AP2_TLV_SALT, ap2_srp_salt(session->srp),
                    AP2_SRP_SALT_BYTES) ||
      !ap2_tlv8_put(&enc, AP2_TLV_PUBLIC_KEY, pub, pub_len)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = ap2_tlv8_writer_size(&enc);
  session->pair_setup_state = SETUP_STATE_M2;
  return ESP_OK;
}

esp_err_t ap2_hap_setup_m3(ap2_hap_session_t *session, const uint8_t *input,
                           size_t input_len, uint8_t *output,
                           size_t output_capacity, size_t *output_len) {
  uint8_t client_pub[AP2_SRP_PRIME_BYTES], proof[AP2_SRP_PROOF_BYTES];
  size_t client_pub_len = 0, proof_len = 0;
  ap2_tlv8_writer_t enc;

  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!session->srp || session->pair_setup_state != SETUP_STATE_M2) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!ap2_tlv8_get(input, input_len, AP2_TLV_PUBLIC_KEY, client_pub,
                    sizeof(client_pub), &client_pub_len) ||
      !ap2_tlv8_get(input, input_len, AP2_TLV_PROOF, proof,
                    sizeof(proof), &proof_len) ||
      client_pub_len == 0 || proof_len != sizeof(proof)) {
    return ESP_ERR_INVALID_ARG;
  }

  if (ap2_srp_verify_client(session->srp, client_pub, client_pub_len, proof,
                            proof_len) != ESP_OK) {
    ESP_LOGW(TAG, "pair-setup M3: wrong code (%s pairing), client proof refused",
             session->pair_setup_transient ? "transient" : "full");
    return ap2_hap_write_error(SETUP_STATE_M4, AP2_TLV_ERROR_AUTHENTICATION, output,
                               output_capacity, output_len);
  }

  ap2_tlv8_writer_init(&enc, output, output_capacity);
  if (!ap2_tlv8_put_byte(&enc, AP2_TLV_STATE, SETUP_STATE_M4) ||
      !ap2_tlv8_put(&enc, AP2_TLV_PROOF, ap2_srp_proof(session->srp),
                    AP2_SRP_PROOF_BYTES)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = ap2_tlv8_writer_size(&enc);
  session->pair_setup_state = SETUP_STATE_M4;

  /* Transient pairing ends here: the SRP key is the session's secret. */
  if (session->pair_setup_transient) {
    size_t key_len = 0;
    const uint8_t *key = ap2_srp_session_key(session->srp, &key_len);
    esp_err_t err = ap2_hap_set_control_keys(session, key, key_len, false);
    if (err != ESP_OK) {
      return err;
    }
    memcpy(session->shared_secret, key, sizeof(session->shared_secret));
    session->session_established = true;
  }
  return ESP_OK;
}

/* Checks the client's M5 and fills M6. ESP_OK or an authentication failure. */
static esp_err_t exchange_identities(ap2_hap_session_t *session, const uint8_t *key,
                                     size_t key_len, const uint8_t *enc_key,
                                     const uint8_t *cipher, size_t cipher_len,
                                     uint8_t *output, size_t output_capacity,
                                     size_t *output_len) {
  uint8_t plain[MAX_M5_PLAIN], signing_salt[32];
  uint8_t info[32 + MAX_ID_LEN + AP2_HAP_ED25519_PUBLIC_KEY_SIZE];
  uint8_t client_id[MAX_ID_LEN], client_pk[AP2_HAP_ED25519_PUBLIC_KEY_SIZE];
  uint8_t client_sig[crypto_sign_BYTES], own_sig[crypto_sign_BYTES];
  uint8_t reply[MAX_M6_PLAIN + AP2_HAP_POLY1305_TAG_SIZE];
  size_t id_len = 0, pk_len = 0, sig_len = 0;
  ap2_tlv8_writer_t enc;

  if (cipher_len < AP2_HAP_POLY1305_TAG_SIZE ||
      cipher_len - AP2_HAP_POLY1305_TAG_SIZE > sizeof(plain) ||
      ap2_hap_open(enc_key, "PS-Msg05", cipher, cipher_len, plain) != ESP_OK) {
    return ESP_FAIL;
  }
  size_t plain_len = cipher_len - AP2_HAP_POLY1305_TAG_SIZE;
  if (!ap2_tlv8_get(plain, plain_len, AP2_TLV_IDENTIFIER, client_id,
                    sizeof(client_id), &id_len) ||
      !ap2_tlv8_get(plain, plain_len, AP2_TLV_PUBLIC_KEY, client_pk,
                    sizeof(client_pk), &pk_len) ||
      !ap2_tlv8_get(plain, plain_len, AP2_TLV_SIGNATURE, client_sig,
                    sizeof(client_sig), &sig_len) ||
      pk_len != sizeof(client_pk) || sig_len != sizeof(client_sig)) {
    return ESP_FAIL;
  }

  /* The client signs (derived X | its id | its public key). */
  if (ap2_hap_kdf("Pair-Setup-Controller-Sign-Salt",
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

#ifdef CONFIG_AIRPLAY_HOMEKIT
  /* The controller that adds the speaker to a home is its first admin. */
  esp_err_t stored = ap2_pairings_add(client_id, id_len, client_pk,
                                      AP2_PERM_ADMIN);
  if (stored != ESP_OK) {
    ESP_LOGE(TAG, "pair-setup M5: storing controller %.*s failed: %s",
             (int)id_len, (const char *)client_id, esp_err_to_name(stored));
    return ap2_hap_write_error(SETUP_STATE_M6,
                               stored == ESP_ERR_NO_MEM ? AP2_TLV_ERROR_MAX_PEERS
                                                        : AP2_TLV_ERROR_UNKNOWN,
                               output, output_capacity, output_len);
  }
  ap2_hap_set_controller(session, client_id, id_len, AP2_PERM_ADMIN);
  const char *own_id = ap2_hap_presented_id(true);
#else
  /* We answer in the same form. */
  const char *own_id = ap2_hap_identity_id();
#endif
  size_t own_id_len = strlen(own_id);
  if (ap2_hap_kdf("Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info",
                  key, key_len, signing_salt, sizeof(signing_salt)) != ESP_OK) {
    return ESP_FAIL;
  }
  memcpy(info, signing_salt, 32);
  memcpy(info + 32, own_id, own_id_len);
  memcpy(info + 32 + own_id_len, ap2_hap_identity_public_key(),
         AP2_HAP_ED25519_PUBLIC_KEY_SIZE);
  crypto_sign_detached(own_sig, NULL, info,
                       32 + own_id_len + AP2_HAP_ED25519_PUBLIC_KEY_SIZE,
                       ap2_hap_identity_secret_key());

  ap2_tlv8_writer_init(&enc, plain, sizeof(plain));
  if (!ap2_tlv8_put(&enc, AP2_TLV_IDENTIFIER, (const uint8_t *)own_id,
                    own_id_len) ||
      !ap2_tlv8_put(&enc, AP2_TLV_PUBLIC_KEY, ap2_hap_identity_public_key(),
                    AP2_HAP_ED25519_PUBLIC_KEY_SIZE) ||
      !ap2_tlv8_put(&enc, AP2_TLV_SIGNATURE, own_sig, sizeof(own_sig))) {
    return ESP_FAIL;
  }
  size_t reply_plain = ap2_tlv8_writer_size(&enc);
  if (ap2_hap_seal(enc_key, "PS-Msg06", plain, reply_plain, reply) != ESP_OK) {
    return ESP_FAIL;
  }

  ap2_tlv8_writer_init(&enc, output, output_capacity);
  if (!ap2_tlv8_put_byte(&enc, AP2_TLV_STATE, SETUP_STATE_M6) ||
      !ap2_tlv8_put(&enc, AP2_TLV_ENCRYPTED_DATA, reply,
                    reply_plain + AP2_HAP_POLY1305_TAG_SIZE)) {
    return ESP_FAIL;
  }
  *output_len = ap2_tlv8_writer_size(&enc);
  session->pair_setup_state = SETUP_STATE_M6;
#ifdef CONFIG_AIRPLAY_HOMEKIT
  ESP_LOGI(TAG, "pair-setup M6: paired with HomeKit controller %.*s (admin)",
           (int)id_len, (const char *)client_id);
  ap2_pairings_log("after pair-setup", true);
#endif
  return ESP_OK;
}

esp_err_t ap2_hap_setup_m5(ap2_hap_session_t *session, const uint8_t *input,
                           size_t input_len, uint8_t *output,
                           size_t output_capacity, size_t *output_len) {
  if (!session || !input || !output || !output_len) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!session->srp || session->pair_setup_state != SETUP_STATE_M4) {
    return ESP_ERR_INVALID_STATE;
  }

  if (session->pair_setup_transient) {
    return ap2_hap_write_error(SETUP_STATE_M6, AP2_TLV_ERROR_AUTHENTICATION,
                               output, output_capacity, output_len);
  }
#ifdef CONFIG_AIRPLAY_HOMEKIT
  /* Requests run under the RTSP server lock: recheck after M1/M3, since
   * another setup may have completed between those requests. */
  if (ap2_pairings_count() != 0) {
    return ap2_hap_write_error(SETUP_STATE_M6, AP2_TLV_ERROR_UNAVAILABLE,
                               output, output_capacity, output_len);
  }
#endif
  size_t key_len = 0, cipher_len = 0;
  const uint8_t *key = ap2_srp_session_key(session->srp, &key_len);
  uint8_t enc_key[AP2_HAP_CHACHA20_KEY_SIZE];
  size_t cipher_cap = MAX_M5_PLAIN + AP2_HAP_POLY1305_TAG_SIZE;
  uint8_t *cipher = malloc(cipher_cap);
  esp_err_t err;

  if (!cipher) {
    return ESP_ERR_NO_MEM;
  }
  if (!key ||
      !ap2_tlv8_get(input, input_len, AP2_TLV_ENCRYPTED_DATA, cipher,
                    cipher_cap, &cipher_len)) {
    free(cipher);
    return ESP_ERR_INVALID_ARG;
  }

  err = ap2_hap_kdf("Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", key,
                    key_len, enc_key, sizeof(enc_key));
  if (err == ESP_OK) {
    err = exchange_identities(session, key, key_len, enc_key, cipher, cipher_len,
                              output, output_capacity, output_len);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "client identity refused");
      err = ap2_hap_write_error(SETUP_STATE_M6, AP2_TLV_ERROR_AUTHENTICATION, output,
                                output_capacity, output_len);
    }
  }
  sodium_memzero(enc_key, sizeof(enc_key));
  free(cipher);
  return err;
}
