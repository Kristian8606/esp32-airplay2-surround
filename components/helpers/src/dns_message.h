/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* The captive DNS message rules, without any ESP-IDF part. Private to the
 * component; the host tests include it directly. See spec/spec.md,
 * "Which queries get a reply" and "The reply". */

#include <stddef.h>
#include <stdint.h>

/* Largest datagram accepted, and the size of the reply buffer. The receive
 * buffer is one byte larger, so that a longer datagram is seen and
 * dropped. */
#define DNS_MESSAGE_MAX 512

/* Builds the reply to the query of query_len bytes into reply (reply_size
 * bytes). redirect_ip is an IPv4 address in network byte order. Returns the
 * length of the reply, or 0 when the query gets no reply (the datagram is
 * dropped). Never reads outside query[0, query_len) nor writes outside
 * reply[0, reply_size). */
size_t dns_message_reply(const uint8_t *query, size_t query_len,
                         uint32_t redirect_ip, uint8_t *reply,
                         size_t reply_size);
