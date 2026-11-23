#include "gw_airplay_artwork.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_airplay_artwork_t *s_current;
static uint32_t s_id;

void gw_airplay_artwork_release(gw_airplay_artwork_t *artwork) {
  if (!artwork) return;
  taskENTER_CRITICAL(&s_lock);
  const bool done = --artwork->refs == 0;
  taskEXIT_CRITICAL(&s_lock);
  if (done) {
    free(artwork->storage);
    free(artwork);
  }
}

static void replace(gw_airplay_artwork_t *artwork) {
  taskENTER_CRITICAL(&s_lock);
  gw_airplay_artwork_t *previous = s_current;
  if (artwork) {
    if (++s_id == 0) ++s_id;
    artwork->id = s_id;
  }
  s_current = artwork;
  taskEXIT_CRITICAL(&s_lock);
  gw_airplay_artwork_release(previous);
}

void gw_airplay_artwork_clear(void) { replace(NULL); }

bool gw_airplay_artwork_receive(uint8_t *storage, const uint8_t *data, size_t size) {
  if (size == 0) {
    gw_airplay_artwork_clear();
    return false;
  }
  if (!storage || !data) return false;

  static const uint8_t png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  const char *mime;
  if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
    mime = "image/jpeg";
  } else if (size >= sizeof(png) && memcmp(data, png, sizeof(png)) == 0) {
    mime = "image/png";
  } else {
    gw_airplay_artwork_clear();
    return false;
  }

  gw_airplay_artwork_t *artwork = malloc(sizeof(*artwork));
  if (!artwork) {
    gw_airplay_artwork_clear();
    return false;
  }
  *artwork = (gw_airplay_artwork_t){.data = data, .size = size, .mime = mime, .storage = storage, .refs = 1};
  replace(artwork);
  return true;
}

uint32_t gw_airplay_artwork_id(void) {
  taskENTER_CRITICAL(&s_lock);
  const uint32_t id = s_current ? s_current->id : 0;
  taskEXIT_CRITICAL(&s_lock);
  return id;
}

gw_airplay_artwork_t *gw_airplay_artwork_retain(uint32_t id) {
  taskENTER_CRITICAL(&s_lock);
  gw_airplay_artwork_t *artwork = s_current;
  if (artwork && artwork->id == id) {
    ++artwork->refs;
  } else {
    artwork = NULL;
  }
  taskEXIT_CRITICAL(&s_lock);
  return artwork;
}
