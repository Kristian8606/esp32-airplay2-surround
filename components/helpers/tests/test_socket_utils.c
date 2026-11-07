/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* socket_utils on POSIX sockets: port 0, an exact port, a port already
 * taken, the options set, and nothing left open after a failure. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "socket_utils.h"
#include "test.h"

#define UNCHANGED 0xbeef

/* The descriptor the next socket would get; equal before and after a call
 * means the call left nothing open. */
static int next_fd(void) {
  int fd = open("/dev/null", O_RDONLY);
  close(fd);
  return fd;
}

static uint16_t local_port(int sock) {
  struct sockaddr_in a;
  socklen_t len = sizeof(a);
  getsockname(sock, (struct sockaddr *)&a, &len);
  return ntohs(a.sin_port);
}

static void check_ipv4_any(int sock, int type) {
  struct sockaddr_in a;
  socklen_t len = sizeof(a);
  CHECK(getsockname(sock, (struct sockaddr *)&a, &len) == 0);
  CHECK(a.sin_family == AF_INET);
  CHECK(a.sin_addr.s_addr == htonl(INADDR_ANY));
  int t = 0;
  len = sizeof(t);
  CHECK(getsockopt(sock, SOL_SOCKET, SO_TYPE, &t, &len) == 0 && t == type);
}

static int reuseaddr(int sock) {
  int v = 0;
  socklen_t len = sizeof(v);
  getsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &v, &len);
  return v != 0;
}

static void test_udp_port0(void) {
  uint16_t p1 = 0, p2 = 0;
  int s1 = socket_utils_bind_udp(0, 0, 0, &p1);
  int s2 = socket_utils_bind_udp(0, 0, 0, &p2);
  CHECK(s1 >= 0 && s2 >= 0);
  CHECK(p1 != 0 && p2 != 0 && p1 != p2);
  CHECK(p1 == local_port(s1) && p2 == local_port(s2));
  check_ipv4_any(s1, SOCK_DGRAM);
  CHECK(!reuseaddr(s1));

  /* No options asked: the system defaults stay. */
  struct timeval tv = {1, 1};
  socklen_t len = sizeof(tv);
  CHECK(getsockopt(s1, SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0);
  CHECK(tv.tv_sec == 0 && tv.tv_usec == 0);

  /* Negative values are not set either. */
  int s3 = socket_utils_bind_udp(0, -1, -5, NULL);
  CHECK(s3 >= 0);
  close(s3);

  /* NULL bound_port. */
  int s4 = socket_utils_bind_udp(0, 0, 0, NULL);
  CHECK(s4 >= 0);
  close(s4);

  close(s1);
  close(s2);
}

static void test_udp_exact_and_taken(void) {
  /* Find a free port, free it, then ask for it exactly. */
  uint16_t free_port = 0;
  int probe = socket_utils_bind_udp(0, 0, 0, &free_port);
  close(probe);

  uint16_t b = 0;
  int s = socket_utils_bind_udp(free_port, 0, 128 * 1024, &b);
  CHECK(s >= 0);
  CHECK(b == free_port);

  /* The same port again fails, closes what it opened, keeps *bound_port. */
  int before = next_fd();
  uint16_t b2 = UNCHANGED;
  CHECK(socket_utils_bind_udp(free_port, 0, 0, &b2) == -1);
  CHECK(b2 == UNCHANGED);
  CHECK(next_fd() == before);

  /* Also with options set. */
  CHECK(socket_utils_bind_udp(free_port, 3, 64 * 1024, &b2) == -1);
  CHECK(b2 == UNCHANGED);
  CHECK(next_fd() == before);
  close(s);
}

static void test_udp_options(void) {
  uint16_t b = 0;
  int s = socket_utils_bind_udp(0, 5, 64 * 1024, &b);
  CHECK(s >= 0);

  struct timeval tv = {0, 0};
  socklen_t len = sizeof(tv);
  CHECK(getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0);
  CHECK(tv.tv_sec == 5 && tv.tv_usec == 0);

  int rcvbuf = 0;
  len = sizeof(rcvbuf);
  CHECK(getsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len) == 0);
  /* Linux reports twice the value set; others report it as set. */
  CHECK(rcvbuf >= 64 * 1024);
  close(s);
}

/* An option the system refuses makes the call fail. Only checked where
 * this system refuses a huge SO_RCVBUF (Linux clamps it instead). */
static void test_udp_option_refused(void) {
  int raw = socket(AF_INET, SOCK_DGRAM, 0);
  int huge = INT_MAX;
  int refused = setsockopt(raw, SOL_SOCKET, SO_RCVBUF, &huge, sizeof(huge));
  close(raw);
  if (refused == 0) {
    printf("test_udp_option_refused: skipped, SO_RCVBUF not refused here\n");
    return;
  }
  int before = next_fd();
  uint16_t b = UNCHANGED;
  CHECK(socket_utils_bind_udp(0, 0, INT_MAX, &b) == -1);
  CHECK(b == UNCHANGED);
  CHECK(next_fd() == before);
}

static void test_tcp(void) {
  uint16_t b = 0;
  int s = socket_utils_bind_tcp_listener(0, 8, true, &b);
  CHECK(s >= 0);
  CHECK(b != 0 && b == local_port(s));
  check_ipv4_any(s, SOCK_STREAM);
  CHECK(reuseaddr(s));
  CHECK((fcntl(s, F_GETFL, 0) & O_NONBLOCK) != 0);

  /* Nonblocking: accept with nothing waiting returns at once. */
  errno = 0;
  CHECK(accept(s, NULL, NULL) == -1);
  CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

  /* A client can connect. */
  int c = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(b);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0);

  /* Blocking listener. */
  uint16_t b2 = 0;
  int s2 = socket_utils_bind_tcp_listener(0, 1, false, &b2);
  CHECK(s2 >= 0 && b2 != 0 && b2 != b);
  CHECK((fcntl(s2, F_GETFL, 0) & O_NONBLOCK) == 0);
  close(s2);

  /* An exact port that another listener holds fails. */
  int before = next_fd();
  uint16_t b3 = UNCHANGED;
  CHECK(socket_utils_bind_tcp_listener(b, 1, true, &b3) == -1);
  CHECK(b3 == UNCHANGED);
  CHECK(next_fd() == before);

  /* After the listener and its connection closed, the same port can be
   * listened on again right away (SO_REUSEADDR). */
  int conn = -1;
  for (int i = 0; i < 100 && conn < 0; i++) {
    conn = accept(s, NULL, NULL);
    if (conn < 0) {
      usleep(10000);
    }
  }
  CHECK(conn >= 0);
  close(conn); /* server side closes first: TIME_WAIT on this port */
  close(c);
  close(s);
  uint16_t b4 = 0;
  int s4 = socket_utils_bind_tcp_listener(b, 8, true, &b4);
  CHECK(s4 >= 0);
  CHECK(b4 == b);
  close(s4);

  /* NULL bound_port. */
  int s5 = socket_utils_bind_tcp_listener(0, 1, true, NULL);
  CHECK(s5 >= 0);
  close(s5);
}

int main(void) {
  test_udp_port0();
  test_udp_exact_and_taken();
  test_udp_options();
  test_udp_option_refused();
  test_tcp();
  return TEST_RESULT();
}
