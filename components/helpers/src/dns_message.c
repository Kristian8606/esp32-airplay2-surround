/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dns_message.h"

#include <string.h>

#define DNS_HEADER_LEN 12
#define DNS_NAME_MAX 255  /* wire form, length bytes and the final zero */
#define DNS_LABEL_MAX 63
#define DNS_ANSWER_LEN 16 /* C0 0C, TYPE, CLASS, TTL, RDLENGTH, 4 bytes */

#define DNS_FLAG_QR 0x80     /* in byte 2 */
#define DNS_OPCODE_MASK 0x78 /* in byte 2 */
#define DNS_FLAG_AA 0x04     /* in byte 2 */
#define DNS_FLAG_RD 0x01     /* in byte 2 */

#define DNS_TYPE_A 1
#define DNS_TYPE_ANY 255
#define DNS_CLASS_IN 1
#define DNS_TTL 60

static uint16_t get_u16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint8_t *put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
  return p + 2;
}

/* Returns the length of the question (name, QTYPE, QCLASS) that starts at
 * offset DNS_HEADER_LEN, or 0 when it does not parse inside len bytes. */
static size_t question_len(const uint8_t *msg, size_t len) {
  size_t pos = DNS_HEADER_LEN;
  size_t name_len = 0;

  for (;;) {
    if (pos >= len) {
      return 0;
    }
    uint8_t label = msg[pos];
    name_len++;
    pos++;
    if (label == 0) {
      break;
    }
    /* Covers compression pointers (top bits 11) and the reserved 01/10
     * forms: anything above 63 is not a plain label. */
    if (label > DNS_LABEL_MAX) {
      return 0;
    }
    name_len += label;
    if (name_len >= DNS_NAME_MAX) {
      /* Even an immediate zero byte would make it longer than 255. */
      return 0;
    }
    if (label > len - pos) {
      return 0;
    }
    pos += label;
  }

  if (len - pos < 4) {
    return 0;
  }
  return pos + 4 - DNS_HEADER_LEN;
}

size_t dns_message_reply(const uint8_t *query, size_t query_len,
                         uint32_t redirect_ip, uint8_t *reply,
                         size_t reply_size) {
  if (query == NULL || reply == NULL || query_len < DNS_HEADER_LEN ||
      query_len > DNS_MESSAGE_MAX) {
    return 0;
  }
  if ((query[2] & DNS_FLAG_QR) != 0 || (query[2] & DNS_OPCODE_MASK) != 0) {
    return 0;
  }
  if (get_u16(query + 4) != 1) {
    return 0;
  }

  size_t qlen = question_len(query, query_len);
  if (qlen == 0) {
    return 0;
  }

  const uint8_t *qtail = query + DNS_HEADER_LEN + qlen - 4;
  uint16_t qtype = get_u16(qtail);
  uint16_t qclass = get_u16(qtail + 2);
  int answer = qclass == DNS_CLASS_IN &&
               (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY);

  size_t total = DNS_HEADER_LEN + qlen + (answer ? DNS_ANSWER_LEN : 0);
  if (total > reply_size) {
    return 0;
  }

  uint8_t *p = reply;
  p[0] = query[0];
  p[1] = query[1];
  p[2] = (uint8_t)(DNS_FLAG_QR | DNS_FLAG_AA | (query[2] & DNS_FLAG_RD));
  p[3] = 0; /* RA = 0, Z = 0, RCODE = NOERROR */
  p += 4;
  p = put_u16(p, 1);                /* QDCOUNT */
  p = put_u16(p, answer ? 1 : 0);   /* ANCOUNT */
  p = put_u16(p, 0);                /* NSCOUNT */
  p = put_u16(p, 0);                /* ARCOUNT */

  memcpy(p, query + DNS_HEADER_LEN, qlen);
  p += qlen;

  if (answer) {
    p = put_u16(p, 0xC000 | DNS_HEADER_LEN); /* pointer to the question name */
    p = put_u16(p, DNS_TYPE_A);
    p = put_u16(p, DNS_CLASS_IN);
    p = put_u16(p, 0);
    p = put_u16(p, DNS_TTL);
    p = put_u16(p, 4);
    memcpy(p, &redirect_ip, 4);
    p += 4;
  }

  return (size_t)(p - reply);
}
