#include "rtsp_crypto.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "audio_diag.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <sys/select.h>
#include "sodium.h"

static const char *TAG = "rtsp_crypto";

/* A response has one wall-clock budget across every partial send/block.
 * Nonblocking sends keep the budget independent of peer socket behavior. */
int rtsp_socket_send_all(int socket, const uint8_t *data, size_t len,
                         int64_t deadline_us) {
  size_t sent = 0;
  while (sent < len) {
    int64_t remaining = deadline_us - esp_timer_get_time();
    if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(socket, &fds);
    struct timeval tv = {.tv_sec = remaining / 1000000,
                         .tv_usec = remaining % 1000000};
    int ready = select(socket + 1, NULL, &fds, NULL, &tv);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) return -1;
    ssize_t r = send(socket, data + sent, len - sent, MSG_DONTWAIT);
    if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (r <= 0) return -1;
    sent += (size_t)r;
  }
  return 0;
}

static uint8_t *crypto_scratch(rtsp_conn_t *conn) {
  if (!conn->crypto_scratch) {
    conn->crypto_scratch = heap_caps_malloc(RTSP_ENCRYPTED_BLOCK_MAX + 16,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!conn->crypto_scratch)
      conn->crypto_scratch = malloc(RTSP_ENCRYPTED_BLOCK_MAX + 16);
  }
  return conn->crypto_scratch;
}

int rtsp_crypto_read_block(int socket, rtsp_conn_t *conn, uint8_t *buffer,
                           size_t buffer_size) {
  if (!conn || !conn->hap_session || !conn->encrypted_mode) {
    // Expected during session teardown - not an error
    return -1;
  }

  // Read 2-byte length header (little-endian).  The socket has SO_RCVTIMEO
  // set for shutdown responsiveness; if it fires after a partial read we
  // must keep waiting rather than return — abandoning N consumed bytes here
  // permanently desyncs the encrypted stream framing and turns every later
  // recv into garbage interpreted as a fresh length prefix.  Cancellation
  // is handled by shutdown(SHUT_RDWR), which makes recv return a real error.
  uint8_t len_buf[2];
  size_t received = 0;
  while (received < 2) {
    ssize_t r = recv(socket, len_buf + received, 2 - received, 0);
    if (r > 0) {
      received += (size_t)r;
      continue;
    }
    if (r == 0) {
      return -1; // peer closed
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    return -1;
  }

  uint16_t block_len = (uint16_t)len_buf[0] | ((uint16_t)len_buf[1] << 8);

  if (block_len == 0 || block_len > RTSP_ENCRYPTED_BLOCK_MAX ||
      block_len > buffer_size) {
    ESP_LOGE(TAG, "Invalid encrypted block length: %d", block_len);
    return -1;
  }

  // Reuse the per-connection read/write scratch; dispatch runs after read.
  size_t encrypted_len = block_len + 16; // +16 for Poly1305 tag
  uint8_t *encrypted = crypto_scratch(conn);
  if (!encrypted) {
    ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
    return -1;
  }

  received = 0;
  while (received < encrypted_len) {
    ssize_t r = recv(socket, encrypted + received, encrypted_len - received, 0);
    if (r > 0) {
      received += (size_t)r;
      continue;
    }
    if (r == 0) {
      return -1; // peer closed
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    return -1;
  }

  // Decrypt using session keys
  uint8_t nonce[12] = {0};
  const uint64_t counter =
      ap2_hap_channel_counter(conn->hap_session, AP2_HAP_RECEIVE);
  memcpy(nonce + 4, &counter, 8);

  unsigned long long plaintext_len;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          buffer, &plaintext_len, NULL, encrypted, encrypted_len, len_buf,
          sizeof(len_buf), nonce,
          ap2_hap_channel_key(conn->hap_session, AP2_HAP_RECEIVE)) != 0) {
    ESP_LOGE(TAG, "Failed to decrypt frame");
    return -1;
  }

  ap2_hap_channel_count(conn->hap_session, AP2_HAP_RECEIVE);

  return (int)plaintext_len;
}

int rtsp_crypto_write_frame(int socket, rtsp_conn_t *conn, const uint8_t *data,
                            size_t data_len) {
  if (!conn || !conn->hap_session || !conn->encrypted_mode) {
    // Expected during session teardown - not an error
    return -1;
  }

  int64_t deadline_us = esp_timer_get_time() + RTSP_SEND_BUDGET_US;
  size_t offset = 0;
  while (offset < data_len) {
    uint16_t block_len = (data_len - offset) > RTSP_ENCRYPTED_BLOCK_MAX
                             ? RTSP_ENCRYPTED_BLOCK_MAX
                             : (uint16_t)(data_len - offset);

    uint8_t len_buf[2];
    len_buf[0] = block_len & 0xFF;
    len_buf[1] = (block_len >> 8) & 0xFF;

    uint8_t nonce[12] = {0};
    const uint64_t counter =
        ap2_hap_channel_counter(conn->hap_session, AP2_HAP_SEND);
    memcpy(nonce + 4, &counter, 8);

    size_t encrypted_len = block_len + 16; // +16 for Poly1305 tag
    uint8_t *encrypted = crypto_scratch(conn);
    if (!encrypted) {
      ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
      return -1;
    }

    unsigned long long ct_len;
    crypto_aead_chacha20poly1305_ietf_encrypt(
        encrypted, &ct_len, data + offset, block_len, len_buf, sizeof(len_buf),
        NULL, nonce, ap2_hap_channel_key(conn->hap_session, AP2_HAP_SEND));

    if (ct_len != encrypted_len) {
      ESP_LOGE(TAG, "Unexpected encrypted length: %llu", ct_len);
      return -1;
    }

    if (rtsp_socket_send_all(socket, len_buf, sizeof(len_buf), deadline_us) != 0 ||
        rtsp_socket_send_all(socket, encrypted, encrypted_len, deadline_us) != 0) {
      ESP_LOGE(TAG, "Failed to send encrypted block");
      conn->close_after_response = true;
      shutdown(socket, SHUT_RDWR);
      return -1;
    }

    ap2_hap_channel_count(conn->hap_session, AP2_HAP_SEND);
    offset += block_len;
  }

  return 0;
}
