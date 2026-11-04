#include "base64.h"

#include <limits.h>

static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ap2_base64_length(size_t input_len) {
  return (input_len + 2) / 3 * 4;
}

int ap2_base64_encode(const uint8_t *input, size_t input_len, char *output,
                      size_t output_capacity) {
  size_t needed = ap2_base64_length(input_len);
  size_t in = 0, out = 0;

  if (needed > output_capacity || needed > INT_MAX) {
    return -1;
  }
  while (input_len - in >= 3) {
    uint32_t bits = ((uint32_t)input[in] << 16) |
                    ((uint32_t)input[in + 1] << 8) | input[in + 2];
    output[out++] = alphabet[(bits >> 18) & 63];
    output[out++] = alphabet[(bits >> 12) & 63];
    output[out++] = alphabet[(bits >> 6) & 63];
    output[out++] = alphabet[bits & 63];
    in += 3;
  }
  if (input_len - in == 1) {
    uint32_t bits = (uint32_t)input[in] << 16;
    output[out++] = alphabet[(bits >> 18) & 63];
    output[out++] = alphabet[(bits >> 12) & 63];
    output[out++] = '=';
    output[out++] = '=';
  } else if (input_len - in == 2) {
    uint32_t bits = ((uint32_t)input[in] << 16) | ((uint32_t)input[in + 1] << 8);
    output[out++] = alphabet[(bits >> 18) & 63];
    output[out++] = alphabet[(bits >> 12) & 63];
    output[out++] = alphabet[(bits >> 6) & 63];
    output[out++] = '=';
  }
  if (out < output_capacity) {
    output[out] = '\0';
  }
  return (int)out;
}
