#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint8_t key[16];
  uint32_t length;
} gw_cover_thumb_t;

/* Use under the USB media lock. A zero length selects the original cover. */
void gw_cover_thumb_prepare(const char *path, int64_t offset, uint32_t length,
                            bool (*cancelled)(void *), void *arg,
                            gw_cover_thumb_t *thumb);

/* Returns a descriptor positioned at the JPEG bytes, or -1 on a miss. */
int gw_cover_thumb_open(const gw_cover_thumb_t *thumb);
