#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TLV8 as used by pair-setup and pair-verify: one type byte, one length byte,
 * then the value. A value over 255 bytes continues in adjacent items of the
 * same type. */

enum {
  TLV_TYPE_METHOD = 0x00,
  TLV_TYPE_IDENTIFIER = 0x01,
  TLV_TYPE_SALT = 0x02,
  TLV_TYPE_PUBLIC_KEY = 0x03,
  TLV_TYPE_PROOF = 0x04,
  TLV_TYPE_ENCRYPTED_DATA = 0x05,
  TLV_TYPE_STATE = 0x06,
  TLV_TYPE_ERROR = 0x07,
  TLV_TYPE_SIGNATURE = 0x0A,
  TLV_TYPE_FLAGS = 0x13,
};

enum {
  TLV_ERROR_UNKNOWN = 0x01,
  TLV_ERROR_AUTHENTICATION = 0x02,
  TLV_ERROR_BACKOFF = 0x03,
  TLV_ERROR_MAX_PEERS = 0x04,
  TLV_ERROR_MAX_TRIES = 0x05,
  TLV_ERROR_UNAVAILABLE = 0x06,
  TLV_ERROR_BUSY = 0x07,
};

#define TLV_FLAG_TRANSIENT 0x10u

#define PAIR_VERIFY_STATE_M1 1
#define PAIR_VERIFY_STATE_M2 2
#define PAIR_VERIFY_STATE_M3 3
#define PAIR_VERIFY_STATE_M4 4

typedef struct {
  uint8_t *buffer;
  size_t size;
  size_t capacity;
} tlv8_encoder_t;

void tlv8_encoder_init(tlv8_encoder_t *enc, uint8_t *buffer, size_t capacity);

/* Appends one value, fragmenting it when needed. False if it does not fit. */
bool tlv8_encode(tlv8_encoder_t *enc, uint8_t type, const uint8_t *value,
                 size_t len);
bool tlv8_encode_byte(tlv8_encoder_t *enc, uint8_t type, uint8_t value);
size_t tlv8_encoder_size(const tlv8_encoder_t *enc);

/* First item of this type. For a fragmented value this is only the first
 * fragment; tlv8_decode_concat returns the whole value. NULL if absent or if
 * the data is malformed before that item. */
const uint8_t *tlv8_find(const uint8_t *data, size_t data_len, uint8_t type,
                         size_t *value_len);

/* Copies the whole value of this type. False if absent, malformed or too big
 * for the buffer. */
bool tlv8_decode_concat(const uint8_t *data, size_t data_len, uint8_t type,
                        uint8_t *out_buffer, size_t out_capacity,
                        size_t *out_len);
