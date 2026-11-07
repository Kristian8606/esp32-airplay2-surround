/* SPDX-License-Identifier: Apache-2.0 */
/* Host fake with the Mbed TLS signature; strict RFC 4648 decoding. */
#pragma once
#include <stddef.h>

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A
#define MBEDTLS_ERR_BASE64_INVALID_CHARACTER -0x002C

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);
