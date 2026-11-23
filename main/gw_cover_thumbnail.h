#pragma once

#include <stddef.h>
#include <stdint.h>

/* The JPEG bitstream must be DMA aligned. The caller frees the result. */
uint8_t *gw_cover_thumbnail(const uint8_t *jpeg, uint32_t length,
                            uint32_t output_limit, uint32_t *output_size);
