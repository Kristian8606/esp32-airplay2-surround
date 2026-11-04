#include "socket_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "socket_utils";

/* Binds to every interface and reports the port actually given. */
static bool bind_any(int fd, uint16_t port, uint16_t *bound_port) {
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind to port %u failed: errno %d", port, errno);
    return false;
  }
  if (bound_port) {
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
      ESP_LOGE(TAG, "getsockname failed: errno %d", errno);
      return false;
    }
    *bound_port = ntohs(addr.sin_port);
  }
  return true;
}

int socket_utils_bind_udp(uint16_t port, int recv_timeout_sec, int recvbuf_size,
                          uint16_t *bound_port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  if (fd < 0) {
    ESP_LOGE(TAG, "UDP socket failed: errno %d", errno);
    return -1;
  }
  if (recvbuf_size > 0) {
    /* Best effort: the system may give less. */
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recvbuf_size,
                     sizeof(recvbuf_size));
  }
  if (recv_timeout_sec > 0) {
    struct timeval timeout = {.tv_sec = recv_timeout_sec, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  if (!bind_any(fd, port, bound_port)) {
    close(fd);
    return -1;
  }
  return fd;
}

int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblocking,
                                   uint16_t *bound_port) {
  const int enable = 1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  if (fd < 0) {
    ESP_LOGE(TAG, "TCP socket failed: errno %d", errno);
    return -1;
  }
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (!bind_any(fd, port, bound_port)) {
    close(fd);
    return -1;
  }
  if (listen(fd, backlog) != 0) {
    ESP_LOGE(TAG, "listen failed: errno %d", errno);
    close(fd);
    return -1;
  }
  if (nonblocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      ESP_LOGE(TAG, "cannot make the listener non-blocking: errno %d", errno);
      close(fd);
      return -1;
    }
  }
  return fd;
}
