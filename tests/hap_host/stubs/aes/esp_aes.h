#pragma once
#include <stddef.h>
#include "mbedtls/aes.h"
typedef mbedtls_aes_context esp_aes_context;
static inline void esp_aes_init(esp_aes_context *c) { mbedtls_aes_init(c); }
static inline void esp_aes_free(esp_aes_context *c) { mbedtls_aes_free(c); }
static inline int esp_aes_setkey(esp_aes_context *c, const unsigned char *k, unsigned int bits) { return mbedtls_aes_setkey_enc(c, k, bits); }
static inline int esp_aes_crypt_ctr(esp_aes_context *c, size_t len, size_t *off, unsigned char nonce[16], unsigned char block[16], const unsigned char *in, unsigned char *out) { return mbedtls_aes_crypt_ctr(c, len, off, nonce, block, in, out); }
