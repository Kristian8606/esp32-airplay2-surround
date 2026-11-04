#include <string.h>

#include "hap.h"
#include "hap_internal.h"
#include "sodium.h"

#define KDF_SALT_CONTROL "Control-Salt"
#define KDF_INFO_READ    "Control-Read-Encryption-Key"
#define KDF_INFO_WRITE   "Control-Write-Encryption-Key"

int hap_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                    size_t ikm_len, const uint8_t *info, size_t info_len,
                    uint8_t *okm, size_t okm_len) {
  uint8_t prk[crypto_auth_hmacsha512_BYTES];
  uint8_t block[crypto_auth_hmacsha512_BYTES];
  crypto_auth_hmacsha512_state st;
  size_t done = 0;
  uint8_t counter = 0;

  if (okm_len > 255u * sizeof(block)) {
    return -1;
  }

  /* Extract */
  crypto_auth_hmacsha512_init(&st, salt, salt_len);
  crypto_auth_hmacsha512_update(&st, ikm, ikm_len);
  crypto_auth_hmacsha512_final(&st, prk);

  /* Expand */
  while (done < okm_len) {
    size_t take = okm_len - done;
    if (take > sizeof(block)) {
      take = sizeof(block);
    }
    counter++;
    crypto_auth_hmacsha512_init(&st, prk, sizeof(prk));
    if (counter > 1) {
      crypto_auth_hmacsha512_update(&st, block, sizeof(block));
    }
    crypto_auth_hmacsha512_update(&st, info, info_len);
    crypto_auth_hmacsha512_update(&st, &counter, 1);
    crypto_auth_hmacsha512_final(&st, block);
    memcpy(okm + done, block, take);
    done += take;
  }

  sodium_memzero(prk, sizeof(prk));
  sodium_memzero(block, sizeof(block));
  sodium_memzero(&st, sizeof(st));
  return 0;
}

esp_err_t hap_kdf(const char *salt, const char *info, const uint8_t *ikm,
                  size_t ikm_len, uint8_t *out, size_t out_len) {
  return hap_hkdf_sha512((const uint8_t *)salt, strlen(salt), ikm, ikm_len,
                         (const uint8_t *)info, strlen(info), out, out_len) == 0
             ? ESP_OK
             : ESP_FAIL;
}

esp_err_t hap_set_control_keys(hap_session_t *session, const uint8_t *secret,
                               size_t secret_len, bool swapped) {
  /* We send with the client's read key and receive with its write key. */
  esp_err_t err = hap_kdf(KDF_SALT_CONTROL,
                          swapped ? KDF_INFO_WRITE : KDF_INFO_READ, secret,
                          secret_len, session->encrypt_key, HAP_CHACHA20_KEY_SIZE);
  if (err == ESP_OK) {
    err = hap_kdf(KDF_SALT_CONTROL, swapped ? KDF_INFO_READ : KDF_INFO_WRITE,
                  secret, secret_len, session->decrypt_key,
                  HAP_CHACHA20_KEY_SIZE);
  }
  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;
  return err;
}

esp_err_t hap_derive_audio_key(hap_session_t *session, uint8_t *audio_key,
                               size_t key_len) {
  if (!session || !audio_key) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!session->session_established) {
    return ESP_ERR_INVALID_STATE;
  }
  return hap_kdf(KDF_SALT_CONTROL, KDF_INFO_READ, session->shared_secret,
                 sizeof(session->shared_secret), audio_key, key_len);
}

static void label_nonce(const char *label, uint8_t nonce[12]) {
  memset(nonce, 0, 12);
  memcpy(nonce + 4, label, 8);
}

esp_err_t hap_seal(const uint8_t key[32], const char *label,
                   const uint8_t *plain, size_t plain_len, uint8_t *cipher) {
  uint8_t nonce[12];
  unsigned long long cipher_len = 0;

  label_nonce(label, nonce);
  return crypto_aead_chacha20poly1305_ietf_encrypt(cipher, &cipher_len, plain,
                                                   plain_len, NULL, 0, NULL,
                                                   nonce, key) == 0
             ? ESP_OK
             : ESP_FAIL;
}

esp_err_t hap_open(const uint8_t key[32], const char *label,
                   const uint8_t *cipher, size_t cipher_len, uint8_t *plain) {
  uint8_t nonce[12];
  unsigned long long plain_len = 0;

  label_nonce(label, nonce);
  return crypto_aead_chacha20poly1305_ietf_decrypt(plain, &plain_len, NULL,
                                                   cipher, cipher_len, NULL, 0,
                                                   nonce, key) == 0
             ? ESP_OK
             : ESP_FAIL;
}
