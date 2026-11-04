#include "ap2_hap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "hap_private.h"
#include "nvs.h"
#include "sodium.h"
#include "ap2_tlv8.h"
#include "srp.h"

static const char *TAG = "hap";

/* Existing boards keep their identity: these names must not change. */
#define IDENTITY_NAMESPACE "airplay"
#define IDENTITY_KEY_PUB   "ed25519_pub"
#define IDENTITY_KEY_SEC   "ed25519_sec"

static uint8_t identity_pub[AP2_HAP_ED25519_PUBLIC_KEY_SIZE];
static uint8_t identity_sec[AP2_HAP_ED25519_SECRET_KEY_SIZE];
static char identity_id[18]; /* AA:BB:CC:DD:EE:FF */

static bool load_identity(nvs_handle_t nvs) {
  size_t pub_len = sizeof(identity_pub), sec_len = sizeof(identity_sec);

  return nvs_get_blob(nvs, IDENTITY_KEY_PUB, identity_pub, &pub_len) == ESP_OK &&
         nvs_get_blob(nvs, IDENTITY_KEY_SEC, identity_sec, &sec_len) == ESP_OK &&
         pub_len == sizeof(identity_pub) && sec_len == sizeof(identity_sec);
}

static esp_err_t create_identity(nvs_handle_t nvs) {
  esp_err_t err;

  if (crypto_sign_keypair(identity_pub, identity_sec) != 0) {
    return ESP_FAIL;
  }
  err = nvs_set_blob(nvs, IDENTITY_KEY_PUB, identity_pub, sizeof(identity_pub));
  if (err == ESP_OK) {
    err = nvs_set_blob(nvs, IDENTITY_KEY_SEC, identity_sec, sizeof(identity_sec));
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  return err;
}

esp_err_t ap2_hap_identity_init(void) {
  nvs_handle_t nvs;
  uint8_t mac[6];
  esp_err_t err;

  if (sodium_init() < 0) {
    ESP_LOGE(TAG, "libsodium failed to start");
    return ESP_FAIL;
  }

  err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    return err;
  }
  snprintf(identity_id, sizeof(identity_id), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot open NVS: %s", esp_err_to_name(err));
    return err;
  }
  if (!load_identity(nvs)) {
    err = create_identity(nvs);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "cannot store identity: %s", esp_err_to_name(err));
    }
  }
  nvs_close(nvs);
  return err;
}

const uint8_t *ap2_hap_identity_public_key(void) {
  return identity_pub;
}

const uint8_t *ap2_hap_identity_secret_key(void) {
  return identity_sec;
}

const char *ap2_hap_identity_id(void) {
  return identity_id;
}

ap2_hap_session_t *ap2_hap_session_new(void) {
  return calloc(1, sizeof(ap2_hap_session_t));
}

void ap2_hap_session_delete(ap2_hap_session_t *session) {
  if (!session) {
    return;
  }
  ap2_srp_delete(session->srp);
  sodium_memzero(session, sizeof(*session));
  free(session);
}

int ap2_hap_setup_state(const ap2_hap_session_t *session) {
  return session->pair_setup_state;
}

int ap2_hap_verify_state(const ap2_hap_session_t *session) {
  return session->pair_verify_state;
}

bool ap2_hap_established(const ap2_hap_session_t *session) {
  return session->session_established;
}

const uint8_t *ap2_hap_shared_secret(const ap2_hap_session_t *session) {
  return session->shared_secret;
}

const uint8_t *ap2_hap_pairing_secret(const ap2_hap_session_t *session,
                                      size_t *len) {
  if (!session->session_established) {
    *len = 0;
    return NULL;
  }
  if (session->pair_setup_transient) {
    return ap2_srp_session_key(session->srp, len);
  }
  *len = sizeof(session->shared_secret);
  return session->shared_secret;
}

const uint8_t *ap2_hap_channel_key(const ap2_hap_session_t *session,
                                   ap2_hap_direction_t direction) {
  return direction == AP2_HAP_SEND ? session->encrypt_key
                                   : session->decrypt_key;
}

uint64_t ap2_hap_channel_counter(const ap2_hap_session_t *session,
                                 ap2_hap_direction_t direction) {
  return direction == AP2_HAP_SEND ? session->encrypt_nonce
                                   : session->decrypt_nonce;
}

void ap2_hap_channel_count(ap2_hap_session_t *session,
                           ap2_hap_direction_t direction) {
  if (direction == AP2_HAP_SEND) {
    session->encrypt_nonce++;
  } else {
    session->decrypt_nonce++;
  }
}

esp_err_t ap2_hap_write_error(uint8_t state, uint8_t code, uint8_t *output,
                              size_t output_capacity, size_t *output_len) {
  ap2_tlv8_writer_t enc;

  ap2_tlv8_writer_init(&enc, output, output_capacity);
  if (!ap2_tlv8_put_byte(&enc, AP2_TLV_STATE, state) ||
      !ap2_tlv8_put_byte(&enc, AP2_TLV_ERROR, code)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = ap2_tlv8_writer_size(&enc);
  return ESP_OK;
}
