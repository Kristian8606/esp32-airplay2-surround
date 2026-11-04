#pragma once

#include <stddef.h>
#include <stdint.h>

/* Characters needed for len input bytes, without a terminating NUL. */
size_t base64_encoded_length(size_t input_len);

/* Standard alphabet with padding. Returns the length written, or -1 if it
 * does not fit. A NUL follows when there is room for it. */
int base64_encode(const uint8_t *input, size_t input_len, char *output,
                  size_t output_capacity);
