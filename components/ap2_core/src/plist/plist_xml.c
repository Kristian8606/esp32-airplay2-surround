#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ap2_plist.h"
#include "base64.h"

/* Appends what fits and keeps the text terminated. */
static void put_text(ap2_xml_t *p, const char *text, size_t len) {
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

static void put(ap2_xml_t *p, const char *text) {
  put_text(p, text, strlen(text));
}

static void put_escaped(ap2_xml_t *p, const char *text) {
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

static void put_key(ap2_xml_t *p, const char *key) {
  put(p, "<key>");
  put_escaped(p, key);
  put(p, "</key>\n");
}

void ap2_xml_init(ap2_xml_t *p, char *buffer, size_t capacity) {
  p->buffer = buffer;
  p->size = 0;
  p->capacity = capacity;
  if (capacity) {
    buffer[0] = '\0';
  }
}

void ap2_xml_begin(ap2_xml_t *p) {
  put(p, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n");
}

void ap2_xml_dict_begin(ap2_xml_t *p) {
  put(p, "<dict>\n");
}

void ap2_xml_dict_end(ap2_xml_t *p) {
  put(p, "</dict>\n");
}

void ap2_xml_string(ap2_xml_t *p, const char *key, const char *value) {
  put_key(p, key);
  put(p, "<string>");
  put_escaped(p, value);
  put(p, "</string>\n");
}

void ap2_xml_int(ap2_xml_t *p, const char *key, int64_t value) {
  char number[24];
  snprintf(number, sizeof(number), "%" PRId64, value);
  put_key(p, key);
  put(p, "<integer>");
  put(p, number);
  put(p, "</integer>\n");
}

void ap2_xml_uint(ap2_xml_t *p, const char *key, uint64_t value) {
  char number[24];
  snprintf(number, sizeof(number), "%" PRIu64, value);
  put_key(p, key);
  put(p, "<integer>");
  put(p, number);
  put(p, "</integer>\n");
}

void ap2_xml_data(ap2_xml_t *p, const char *key, const uint8_t *data,
                  size_t len) {
  put_key(p, key);
  put(p, "<data>");
  /* Encode directly into the buffer, if it fits. */
  if (p->capacity && p->size < p->capacity) {
    int written = ap2_base64_encode(data, len, p->buffer + p->size,
                                    p->capacity - 1 - p->size);
    if (written > 0) {
      p->size += (size_t)written;
      p->buffer[p->size] = '\0';
    }
  }
  put(p, "</data>\n");
}

void ap2_xml_array_begin(ap2_xml_t *p, const char *key) {
  put_key(p, key);
  put(p, "<array>\n");
}

void ap2_xml_child_dict_begin(ap2_xml_t *p, const char *key) {
  put_key(p, key);
  put(p, "<dict>\n");
}

void ap2_xml_array_end(ap2_xml_t *p) {
  put(p, "</array>\n");
}

size_t ap2_xml_end(ap2_xml_t *p) {
  put(p, "</plist>\n");
  return p->size;
}
