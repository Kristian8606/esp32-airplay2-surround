#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plist.h"

/* Appends what fits and keeps the text terminated. */
static void put_text(plist_t *p, const char *text, size_t len) {
  if (p->capacity == 0) {
    return;
  }
  size_t room = p->capacity - 1 - p->size;
  if (len > room) {
    len = room;
  }
  memcpy(p->buffer + p->size, text, len);
  p->size += len;
  p->buffer[p->size] = '\0';
}

static void put(plist_t *p, const char *text) {
  put_text(p, text, strlen(text));
}

static void put_escaped(plist_t *p, const char *text) {
  for (; *text; text++) {
    switch (*text) {
    case '&':
      put(p, "&amp;");
      break;
    case '<':
      put(p, "&lt;");
      break;
    case '>':
      put(p, "&gt;");
      break;
    default:
      put_text(p, text, 1);
      break;
    }
  }
}

static void put_key(plist_t *p, const char *key) {
  put(p, "<key>");
  put_escaped(p, key);
  put(p, "</key>\n");
}

void plist_init(plist_t *p, char *buffer, size_t capacity) {
  p->buffer = buffer;
  p->size = 0;
  p->capacity = capacity;
  if (capacity) {
    buffer[0] = '\0';
  }
}

void plist_begin(plist_t *p) {
  put(p, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\">\n");
}

void plist_dict_begin(plist_t *p) {
  put(p, "<dict>\n");
}

void plist_dict_end(plist_t *p) {
  put(p, "</dict>\n");
}

void plist_dict_string(plist_t *p, const char *key, const char *value) {
  put_key(p, key);
  put(p, "<string>");
  put_escaped(p, value);
  put(p, "</string>\n");
}

void plist_dict_int(plist_t *p, const char *key, int64_t value) {
  char number[24];
  snprintf(number, sizeof(number), "%" PRId64, value);
  put_key(p, key);
  put(p, "<integer>");
  put(p, number);
  put(p, "</integer>\n");
}

void plist_dict_uint(plist_t *p, const char *key, uint64_t value) {
  char number[24];
  snprintf(number, sizeof(number), "%" PRIu64, value);
  put_key(p, key);
  put(p, "<integer>");
  put(p, number);
  put(p, "</integer>\n");
}

void plist_dict_data(plist_t *p, const char *key, const uint8_t *data,
                     size_t len) {
  put_key(p, key);
  put(p, "<data>");
  /* Encode directly into the buffer, if it fits. */
  if (p->capacity && p->size < p->capacity) {
    int written = base64_encode(data, len, p->buffer + p->size,
                                p->capacity - 1 - p->size);
    if (written > 0) {
      p->size += (size_t)written;
      p->buffer[p->size] = '\0';
    }
  }
  put(p, "</data>\n");
}

void plist_dict_array_begin(plist_t *p, const char *key) {
  put_key(p, key);
  put(p, "<array>\n");
}

void plist_dict_dict_begin(plist_t *p, const char *key) {
  put_key(p, key);
  put(p, "<dict>\n");
}

void plist_array_end(plist_t *p) {
  put(p, "</array>\n");
}

size_t plist_end(plist_t *p) {
  put(p, "</plist>\n");
  return p->size;
}
