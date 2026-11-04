#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TLV8, the message format of pair-setup and pair-verify: a type byte, a
 * length byte and the value. A value longer than 255 bytes goes on in the
 * next items of the same type. */

/* Item types. */
enum {
  AP2_TLV_METHOD = 0x00,
  AP2_TLV_IDENTIFIER = 0x01,
  AP2_TLV_SALT = 0x02,
  AP2_TLV_PUBLIC_KEY = 0x03,
  AP2_TLV_PROOF = 0x04,
  AP2_TLV_ENCRYPTED_DATA = 0x05,
  AP2_TLV_STATE = 0x06,
  AP2_TLV_ERROR = 0x07,
  AP2_TLV_SIGNATURE = 0x0A,
  AP2_TLV_FLAGS = 0x13,
};

/* Values of an AP2_TLV_ERROR item. */
enum {
  AP2_TLV_ERROR_UNKNOWN = 0x01,
  AP2_TLV_ERROR_AUTHENTICATION = 0x02,
  AP2_TLV_ERROR_BACKOFF = 0x03,
  AP2_TLV_ERROR_MAX_PEERS = 0x04,
  AP2_TLV_ERROR_MAX_TRIES = 0x05,
  AP2_TLV_ERROR_UNAVAILABLE = 0x06,
  AP2_TLV_ERROR_BUSY = 0x07,
};

/* Bit of an AP2_TLV_FLAGS item: pair-setup without a stored pairing. */
#define AP2_TLV_FLAG_TRANSIENT 0x10u

/* Writes into a caller's buffer. The fields are private. */
typedef struct {
  uint8_t *buffer;
  size_t size;
  size_t capacity;
} ap2_tlv8_writer_t;

void ap2_tlv8_writer_init(ap2_tlv8_writer_t *writer, uint8_t *buffer,
                          size_t capacity);
/* Appends one value, split into items when needed. False if it does not
 * fit. */
bool ap2_tlv8_put(ap2_tlv8_writer_t *writer, uint8_t type,
                  const uint8_t *value, size_t len);
bool ap2_tlv8_put_byte(ap2_tlv8_writer_t *writer, uint8_t type,
                       uint8_t value);
/* Bytes written so far. */
size_t ap2_tlv8_writer_size(const ap2_tlv8_writer_t *writer);

/* The first item of this type: for a split value only its first part (see
 * ap2_tlv8_get). NULL if absent or if the data is malformed before it. */
const uint8_t *ap2_tlv8_find(const uint8_t *data, size_t data_len,
                             uint8_t type, size_t *value_len);

/* Copies the whole value of this type. False if absent, malformed or too
 * big for the buffer. */
bool ap2_tlv8_get(const uint8_t *data, size_t data_len, uint8_t type,
                  uint8_t *out, size_t out_capacity, size_t *out_len);
