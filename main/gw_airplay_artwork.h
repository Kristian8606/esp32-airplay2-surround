#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const uint8_t *data;
  size_t size;
  const char *mime;
  uint32_t id;
  uint8_t *storage; /* Owned RTSP body, which can contain a plist around the image. */
  unsigned refs; /* Managed by retain/release. */
} gw_airplay_artwork_t;

/* On true the Gateway owns storage; data can point inside it. An empty
 * image clears the artwork and leaves storage owned by the RTSP reader. */
bool gw_airplay_artwork_receive(uint8_t *storage, const uint8_t *data, size_t size);

/* Read an AirPlay 2 Now Playing update without copying its artwork data. */
bool gw_airplay_artwork_plist(const uint8_t *plist, size_t len,
                             const uint8_t **data, size_t *size, bool *replace);
void gw_airplay_artwork_clear(void);

/* A retained image remains valid until the HTTP transfer has finished. */
uint32_t gw_airplay_artwork_id(void);
gw_airplay_artwork_t *gw_airplay_artwork_retain(uint32_t id);
void gw_airplay_artwork_release(gw_airplay_artwork_t *artwork);
