/* SPDX-License-Identifier: Apache-2.0 */
/* Base64 encoding (RFC 4648) for building test headers. */
#pragma once
#include <stdlib.h>

/* Returns a malloc'd NUL-terminated string. */
static inline char *b64_encode(const unsigned char *in, size_t len) {
  static const char A[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *out = malloc((len + 2) / 3 * 4 + 1);
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < len) v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < len) v |= in[i + 2];
    out[o++] = A[(v >> 18) & 63];
    out[o++] = A[(v >> 12) & 63];
    out[o++] = i + 1 < len ? A[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < len ? A[v & 63] : '=';
  }
  out[o] = '\0';
  return out;
}
