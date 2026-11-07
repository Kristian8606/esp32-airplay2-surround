/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* Bound IPv4 sockets. Uses only the BSD socket calls that lwIP provides, so
 * the same file builds on a computer for the host tests. */

#include "socket_utils.h"

#include <string.h>

#if defined(ESP_PLATFORM)
#include "lwip/sockets.h"
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* Binds sock to INADDR_ANY:port and reads back the port bound. */
static int bind_any(int sock, uint16_t port, uint16_t *actual) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    return -1;
  }

  struct sockaddr_in bound;
  socklen_t len = sizeof(bound);
  memset(&bound, 0, sizeof(bound));
  if (getsockname(sock, (struct sockaddr *)&bound, &len) != 0 ||
      bound.sin_family != AF_INET) {
    return -1;
  }
  *actual = ntohs(bound.sin_port);
  return 0;
}

int socket_utils_bind_udp(uint16_t port, int recv_timeout_sec, int recvbuf_size,
                          uint16_t *bound_port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    return -1;
  }

  if (recv_timeout_sec > 0) {
    struct timeval tv = {.tv_sec = recv_timeout_sec, .tv_usec = 0};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
      goto fail;
    }
  }
  if (recvbuf_size > 0) {
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbuf_size,
                   sizeof(recvbuf_size)) != 0) {
      goto fail;
    }
  }

  uint16_t actual;
  if (bind_any(sock, port, &actual) != 0) {
    goto fail;
  }
  if (bound_port != NULL) {
    *bound_port = actual;
  }
  return sock;

fail:
  close(sock);
  return -1;
}

int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblocking,
                                   uint16_t *bound_port) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    return -1;
  }

  int one = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
    goto fail;
  }

  uint16_t actual;
  if (bind_any(sock, port, &actual) != 0) {
    goto fail;
  }
  if (listen(sock, backlog) != 0) {
    goto fail;
  }

  if (nonblocking) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
      goto fail;
    }
  }

  if (bound_port != NULL) {
    *bound_port = actual;
  }
  return sock;

fail:
  close(sock);
  return -1;
}
