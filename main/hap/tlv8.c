#include "tlv8.h"

#include <string.h>

#define TLV_HEADER_LEN   2u
#define TLV_FRAGMENT_MAX 255u

void tlv8_encoder_init(tlv8_encoder_t *enc, uint8_t *buffer, size_t capacity) {
  enc->buffer = buffer;
  enc->size = 0;
  enc->capacity = capacity;
}

static bool put_item(tlv8_encoder_t *enc, uint8_t type, const uint8_t *value,
                     size_t len) {
  if (enc->capacity - enc->size < TLV_HEADER_LEN + len) {
    return false;
  }
  enc->buffer[enc->size++] = type;
  enc->buffer[enc->size++] = (uint8_t)len;
  if (len) {
    memcpy(enc->buffer + enc->size, value, len);
    enc->size += len;
  }
  return true;
}

bool tlv8_encode(tlv8_encoder_t *enc, uint8_t type, const uint8_t *value,
                 size_t len) {
  if (len == 0) {
    return put_item(enc, type, NULL, 0);
  }
  while (len) {
    size_t chunk = len < TLV_FRAGMENT_MAX ? len : TLV_FRAGMENT_MAX;
    if (!put_item(enc, type, value, chunk)) {
      return false;
    }
    value += chunk;
    len -= chunk;
  }
  return true;
}

bool tlv8_encode_byte(tlv8_encoder_t *enc, uint8_t type, uint8_t value) {
  return put_item(enc, type, &value, 1);
}

size_t tlv8_encoder_size(const tlv8_encoder_t *enc) {
  return enc->size;
}

/* Reads the item at *pos and moves past it. False at the end or if the item
 * runs past the data. */
static bool next_item(const uint8_t *data, size_t data_len, size_t *pos,
                      uint8_t *type, const uint8_t **value, size_t *len) {
  if (data_len - *pos < TLV_HEADER_LEN) {
    return false;
  }
  size_t item_len = data[*pos + 1];
  if (data_len - *pos - TLV_HEADER_LEN < item_len) {
    return false;
  }
  *type = data[*pos];
  *value = data + *pos + TLV_HEADER_LEN;
  *len = item_len;
  *pos += TLV_HEADER_LEN + item_len;
  return true;
}

const uint8_t *tlv8_find(const uint8_t *data, size_t data_len, uint8_t type,
                         size_t *value_len) {
  size_t pos = 0, len = 0;
  uint8_t item_type = 0;
  const uint8_t *value = NULL;

  if (!data) {
    return NULL;
  }
  while (next_item(data, data_len, &pos, &item_type, &value, &len)) {
    if (item_type == type) {
      if (value_len) {
        *value_len = len;
      }
      return value;
    }
  }
  return NULL;
}

bool tlv8_decode_concat(const uint8_t *data, size_t data_len, uint8_t type,
                        uint8_t *out_buffer, size_t out_capacity,
                        size_t *out_len) {
  size_t pos = 0, len = 0, total = 0;
  uint8_t item_type = 0;
  const uint8_t *value = NULL;
  bool found = false;

  if (!data) {
    return false;
  }
  while (next_item(data, data_len, &pos, &item_type, &value, &len)) {
    if (item_type != type) {
      if (found) {
        break; /* a value's fragments are adjacent */
      }
      continue;
    }
    if (out_capacity - total < len) {
      return false;
    }
    if (len) {
      memcpy(out_buffer + total, value, len);
    }
    total += len;
    found = true;
    if (len < TLV_FRAGMENT_MAX) {
      break; /* only a full fragment continues */
    }
  }
  if (found) {
    *out_len = total;
  }
  return found;
}
