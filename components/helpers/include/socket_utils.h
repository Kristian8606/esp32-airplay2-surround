/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* IPv4 sockets bound to every local address. See spec/spec.md,
 * "socket_utils". Both return the socket, or -1. */

/* port 0: any free port. recv_timeout_sec and recvbuf_size are set only
 * when above 0. *bound_port (may be NULL) gets the port bound. */
int socket_utils_bind_udp(uint16_t port, int recv_timeout_sec, int recvbuf_size,
                          uint16_t *bound_port);

/* A listening TCP socket. */
int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblocking,
                                   uint16_t *bound_port);
