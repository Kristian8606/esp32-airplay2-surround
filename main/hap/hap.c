#include "hap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "hap_internal.h"
#include "nvs.h"
#include "sodium.h"
#include "srp.h"
#include "tlv8.h"

static const char *TAG = "hap";

/* Existing boards keep their identity: these names must not change. */
#define IDENTITY_NAMESPACE "airplay"
#define IDENTITY_KEY_PUB   "ed25519_pub"
#define IDENTITY_KEY_SEC   "ed25519_sec"

static uint8_t identity_pub[HAP_ED25519_PUBLIC_KEY_SIZE];
static uint8_t identity_sec[HAP_ED25519_SECRET_KEY_SIZE];
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

esp_err_t hap_init(void) {
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

const uint8_t *hap_get_public_key(void) {
  return identity_pub;
}

const uint8_t *hap_identity_secret_key(void) {
  return identity_sec;
}

const char *hap_identity_id(void) {
  return identity_id;
}

hap_session_t *hap_session_create(void) {
  return calloc(1, sizeof(hap_session_t));
}

void hap_session_free(hap_session_t *session) {
  if (!session) {
    return;
  }
  srp_session_free(session->srp);
  sodium_memzero(session, sizeof(*session));
  free(session);
}

esp_err_t hap_write_error(uint8_t state, uint8_t code, uint8_t *output,
                          size_t output_capacity, size_t *output_len) {
  tlv8_encoder_t enc;

  tlv8_encoder_init(&enc, output, output_capacity);
  if (!tlv8_encode_byte(&enc, TLV_TYPE_STATE, state) ||
      !tlv8_encode_byte(&enc, TLV_TYPE_ERROR, code)) {
    return ESP_ERR_INVALID_SIZE;
  }
  *output_len = tlv8_encoder_size(&enc);
  return ESP_OK;
}
