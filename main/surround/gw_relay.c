#include "gw_relay.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ptp_clock.h"
#include "socket_utils.h"

static const char *TAG = "gw_relay";

/* Same placement as the speaker's buffered TCP reader (aac_fifo_rx). */
#define GW_RELAY_CORE        1
#define GW_RELAY_PRIORITY    4
#define GW_RELAY_STACK       4096U
#define GW_RELAY_CHUNK       1460U
#define GW_RELAY_POLL_MS     200
#define GW_RELAY_STATS_MS    5000
#define GW_RELAY_STOP_WAIT_MS 1000
/* Same bound as ap2_buffered_fifo: length bytes + 12-byte RTP header. */
#define GW_RELAY_MIN_WIRE_LEN 14U

typedef struct {
  /* Frame walker over the TCP byte stream. */
  uint8_t len_bytes[2];
  uint8_t len_have;
  uint16_t wire_len;
  uint16_t body_have;
  uint8_t header[12];
  /* Per-connection totals and the current stats window. */
  uint64_t total_bytes;
  uint64_t total_packets;
  uint32_t win_bytes;
  uint32_t win_packets;
  uint32_t win_first_seq, win_last_seq;
  uint32_t win_first_rtp, win_last_rtp;
  int64_t win_start_us;
} relay_stream_t;

static struct {
  int listen_sock;
  int client_sock;
  uint16_t port;
  volatile bool running;
  TaskHandle_t task;
  relay_stream_t stream;
} s = {.listen_sock = -1, .client_sock = -1};

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void stream_reset(void) {
  memset(&s.stream, 0, sizeof(s.stream));
  s.stream.win_start_us = esp_timer_get_time();
}

static void log_window(bool force) {
  relay_stream_t *st = &s.stream;
  const int64_t now_us = esp_timer_get_time();
  const int64_t span_us = now_us - st->win_start_us;
  if (!force && span_us < (int64_t)GW_RELAY_STATS_MS * 1000LL) return;
  if (st->win_packets > 0U || force) {
    ptp_clock_snapshot_t ps = {0};
    ptp_clock_get_snapshot(&ps);
    const double kbit_s =
        span_us > 0 ? (double)st->win_bytes * 8000.0 / (double)span_us : 0.0;
    ESP_LOGI(TAG,
             "RELAY rx %" PRIu32 " pkts %" PRIu32 " B (%.0f kbit/s) seq %" PRIu32
             "..%" PRIu32 " rtp %" PRIu32 "..%" PRIu32 " | total %" PRIu64
             " pkts %" PRIu64 " KiB | PTP valid=%d locked=%d gm=%016" PRIx64
             " offset=%" PRId64 " ns",
             st->win_packets, st->win_bytes, kbit_s, st->win_first_seq,
             st->win_last_seq, st->win_first_rtp, st->win_last_rtp,
             st->total_packets, st->total_bytes / 1024U, ps.valid, ps.locked,
             ps.grandmaster_clock_id, ps.filtered_offset_ns);
  }
  st->win_bytes = 0;
  st->win_packets = 0;
  st->win_start_us = now_us;
}

static void frame_done(void) {
  relay_stream_t *st = &s.stream;
  /* Buffered frames carry a 23-bit sequence number (audio_receiver.c). */
  const uint32_t seq = read_be32(st->header) & 0x007fffffU;
  const uint32_t rtp = read_be32(st->header + 4);
  if (st->win_packets == 0U) {
    st->win_first_seq = seq;
    st->win_first_rtp = rtp;
  }
  st->win_last_seq = seq;
  st->win_last_rtp = rtp;
  st->win_packets++;
  st->total_packets++;
}

/* Walk the frames in one received chunk. False on invalid framing, which
 * aborts the connection exactly like the speaker's FIFO does. */
static bool consume(const uint8_t *data, size_t len) {
  relay_stream_t *st = &s.stream;
  st->total_bytes += len;
  st->win_bytes += (uint32_t)len;
  while (len > 0) {
    if (st->len_have < 2U) {
      st->len_bytes[st->len_have++] = *data++;
      len--;
      if (st->len_have == 2U) {
        st->wire_len = (uint16_t)((st->len_bytes[0] << 8) | st->len_bytes[1]);
        st->body_have = 0;
        if (st->wire_len < GW_RELAY_MIN_WIRE_LEN) {
          ESP_LOGE(TAG, "invalid frame length %u after %" PRIu64 " packets",
                   (unsigned)st->wire_len, st->total_packets);
          return false;
        }
      }
      continue;
    }
    const size_t body_len = (size_t)st->wire_len - 2U;
    size_t take = body_len - st->body_have;
    if (take > len) take = len;
    if (st->body_have < sizeof(st->header)) {
      size_t h = sizeof(st->header) - st->body_have;
      if (h > take) h = take;
      memcpy(st->header + st->body_have, data, h);
    }
    st->body_have += (uint16_t)take;
    data += take;
    len -= take;
    if (st->body_have == body_len) {
      frame_done();
      st->len_have = 0;
    }
  }
  return true;
}

static void close_client(const char *why) {
  if (s.client_sock < 0) return;
  log_window(true);
  AP_LOGI(TAG, "sender audio connection closed (%s): %" PRIu64 " pkts, %" PRIu64
               " KiB",
          why, s.stream.total_packets, s.stream.total_bytes / 1024U);
  close(s.client_sock);
  s.client_sock = -1;
}

static void relay_task(void *arg) {
  (void)arg;
  static uint8_t chunk[GW_RELAY_CHUNK];
  while (s.running) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s.listen_sock, &rd);
    int maxfd = s.listen_sock;
    if (s.client_sock >= 0) {
      FD_SET(s.client_sock, &rd);
      if (s.client_sock > maxfd) maxfd = s.client_sock;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = GW_RELAY_POLL_MS * 1000};
    const int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
    if (!s.running) break;
    if (n < 0) {
      if (errno != EINTR) {
        ESP_LOGE(TAG, "select errno=%d", errno);
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      continue;
    }

    if (n > 0 && FD_ISSET(s.listen_sock, &rd)) {
      const int c = accept(s.listen_sock, NULL, NULL);
      if (c >= 0) {
        /* A newly accepted connection is a fresh sequential byte stream. */
        close_client("replaced by a new connection");
        s.client_sock = c;
        stream_reset();
        AP_LOGI(TAG, "sender audio connected on port %u", (unsigned)s.port);
      }
    }

    if (n > 0 && s.client_sock >= 0 && FD_ISSET(s.client_sock, &rd)) {
      const int r = recv(s.client_sock, chunk, sizeof(chunk), 0);
      if (r > 0) {
        if (!consume(chunk, (size_t)r)) close_client("invalid framing");
      } else if (r == 0) {
        close_client("sender closed");
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_client("recv error");
      }
    }

    if (s.client_sock >= 0) log_window(false);
  }

  close_client("relay stopped");
  close(s.listen_sock);
  s.listen_sock = -1;
  s.task = NULL;
  vTaskDelete(NULL);
}

esp_err_t gw_relay_start(uint16_t requested_port, uint16_t *bound_port) {
  if (s.running) {
    if (bound_port) *bound_port = s.port;
    return ESP_OK;
  }
  if (s.task) return ESP_ERR_INVALID_STATE;

  uint16_t bound = requested_port;
  s.listen_sock = socket_utils_bind_tcp_listener(requested_port, 1, true, &bound);
  if (s.listen_sock < 0) return ESP_FAIL;
  s.port = bound;
  s.running = true;
  if (xTaskCreatePinnedToCore(relay_task, "gw_relay", GW_RELAY_STACK, NULL,
                              GW_RELAY_PRIORITY, &s.task,
                              GW_RELAY_CORE) != pdPASS) {
    s.running = false;
    close(s.listen_sock);
    s.listen_sock = -1;
    return ESP_FAIL;
  }
  AP_LOGI(TAG, "buffered audio listener port=%u", (unsigned)bound);
  if (bound_port) *bound_port = bound;
  return ESP_OK;
}

void gw_relay_stop(void) {
  s.running = false;
  for (int i = 0; s.task && i < GW_RELAY_STOP_WAIT_MS / 10; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s.task) ESP_LOGE(TAG, "relay task did not stop within %d ms",
                       GW_RELAY_STOP_WAIT_MS);
  else s.port = 0;
}

bool gw_relay_is_idle(void) { return !s.running && !s.task; }
