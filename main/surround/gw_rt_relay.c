#include "gw_rt_relay.h"

#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_speakers.h"
#include "socket_utils.h"

static const char *TAG = "gw_rt_relay";

/* Realtime audio has only ~2 s of slack: above the buffered relay. */
#define RT_RELAY_CORE         1
#define RT_RELAY_PRIORITY     6
#define RT_RELAY_STACK        4096U
#define RT_RELAY_POLL_MS      200
#define RT_RELAY_STATS_MS     5000
#define RT_RELAY_STOP_WAIT_MS 1000
#define RT_RELAY_PACKET_MAX   2048U
#define RT_RELAY_RCVBUF       (128 * 1024)
/* NACK: 0x80, 0x80|0x55, request seq, first missing seq, count. Several
 * speakers usually miss the same packet: one request reaches the sender. */
#define RT_NACK_LEN           8U
#define RT_NACK_PT            0x55U
#define RT_NACK_DEDUP_US      30000LL
#define RT_NACK_HISTORY       16U

typedef struct {
  uint16_t first;
  uint16_t count;
  int64_t at_us;
} nack_seen_t;

typedef struct {
  uint32_t data_in, control_in, nack_in, nack_out, send_failed;
} rt_stats_t;

static struct {
  int data_sock;
  int control_sock;
  uint16_t data_port;
  uint16_t control_port;
  volatile bool running;
  TaskHandle_t task;
  portMUX_TYPE mux;
  struct sockaddr_in sender_control; /* guarded by mux */
  bool sender_control_valid;
  uint32_t targets_gen;
  gw_audio_target_t targets[SURROUND_MAX_SPEAKERS];
  nack_seen_t nacks[RT_NACK_HISTORY];
  uint32_t nack_head;
  rt_stats_t win;
  int64_t win_start_us;
} s = {.data_sock = -1,
       .control_sock = -1,
       .mux = portMUX_INITIALIZER_UNLOCKED};

static void refresh_targets(void) {
  const uint32_t gen = gw_speakers_audio_generation();
  if (gen == s.targets_gen) return;
  s.targets_gen = gen;
  gw_speakers_audio_targets(s.targets);
}

/* Copy one datagram to every speaker's data (or control) port. */
static void fan_out(const uint8_t *pkt, size_t len, bool control) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const gw_audio_target_t *t = &s.targets[i];
    const uint16_t port = control ? t->rt_control_port : t->rt_data_port;
    if (!port || !t->ip) continue;
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = t->ip,
    };
    /* Sent from the matching socket, so a speaker sees the usual ports. */
    const int sock = control ? s.control_sock : s.data_sock;
    if (sendto(sock, pkt, len, 0, (struct sockaddr *)&to, sizeof(to)) !=
        (ssize_t)len) {
      s.win.send_failed++;
    }
  }
}

static bool from_speaker(uint32_t ip) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    if (s.targets[i].ip == ip && (s.targets[i].rt_control_port ||
                                  s.targets[i].rt_data_port))
      return true;
  }
  return false;
}

static void forward_nack(const uint8_t *pkt, size_t len, int64_t now_us) {
  s.win.nack_in++;
  if (len != RT_NACK_LEN || (pkt[1] & 0x7fU) != RT_NACK_PT) return;
  const uint16_t first = (uint16_t)((pkt[4] << 8) | pkt[5]);
  const uint16_t count = (uint16_t)((pkt[6] << 8) | pkt[7]);
  for (size_t i = 0; i < RT_NACK_HISTORY; ++i) {
    const nack_seen_t *n = &s.nacks[i];
    if (n->at_us && n->first == first && n->count == count &&
        now_us - n->at_us < RT_NACK_DEDUP_US)
      return;
  }
  s.nacks[s.nack_head] = (nack_seen_t){first, count, now_us};
  s.nack_head = (s.nack_head + 1U) % RT_NACK_HISTORY;

  struct sockaddr_in to;
  bool valid;
  taskENTER_CRITICAL(&s.mux);
  to = s.sender_control;
  valid = s.sender_control_valid;
  taskEXIT_CRITICAL(&s.mux);
  if (!valid) return;
  if (sendto(s.control_sock, pkt, len, 0, (struct sockaddr *)&to, sizeof(to)) ==
      (ssize_t)len) {
    s.win.nack_out++;
  } else {
    s.win.send_failed++;
  }
}

static void log_window(int64_t now_us) {
  if (now_us - s.win_start_us < RT_RELAY_STATS_MS * 1000LL) return;
  if (s.win.data_in || s.win.control_in || s.win.nack_in) {
    unsigned speakers = 0;
    for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
      if (s.targets[i].rt_data_port) speakers++;
    }
    ESP_LOGI(TAG,
             "RT RELAY data %" PRIu32 " control %" PRIu32 " pkts -> %u speakers"
             " | NACK %" PRIu32 " in, %" PRIu32 " to sender | send failed %"
             PRIu32,
             s.win.data_in, s.win.control_in, speakers, s.win.nack_in,
             s.win.nack_out, s.win.send_failed);
  }
  memset(&s.win, 0, sizeof(s.win));
  s.win_start_us = now_us;
}

static void rt_relay_task(void *arg) {
  (void)arg;
  static uint8_t pkt[RT_RELAY_PACKET_MAX];
  while (s.running) {
    refresh_targets();
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s.data_sock, &rd);
    FD_SET(s.control_sock, &rd);
    const int maxfd = s.data_sock > s.control_sock ? s.data_sock : s.control_sock;
    struct timeval tv = {.tv_sec = 0, .tv_usec = RT_RELAY_POLL_MS * 1000};
    const int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
    if (!s.running) break;
    if (n < 0) {
      if (errno != EINTR) {
        ESP_LOGE(TAG, "select errno=%d", errno);
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      continue;
    }
    const int64_t now_us = esp_timer_get_time();
    if (n > 0 && FD_ISSET(s.data_sock, &rd)) {
      const int r = recv(s.data_sock, pkt, sizeof(pkt), MSG_DONTWAIT);
      if (r > 0) {
        s.win.data_in++;
        fan_out(pkt, (size_t)r, false);
      }
    }
    if (n > 0 && FD_ISSET(s.control_sock, &rd)) {
      struct sockaddr_in src;
      socklen_t slen = sizeof(src);
      const int r = recvfrom(s.control_sock, pkt, sizeof(pkt), MSG_DONTWAIT,
                             (struct sockaddr *)&src, &slen);
      if (r > 0) {
        if (from_speaker(src.sin_addr.s_addr)) {
          forward_nack(pkt, (size_t)r, now_us);
        } else {
          s.win.control_in++;
          fan_out(pkt, (size_t)r, true);
        }
      }
    }
    log_window(now_us);
  }
  close(s.data_sock);
  close(s.control_sock);
  s.data_sock = s.control_sock = -1;
  s.task = NULL;
  vTaskDelete(NULL);
}

esp_err_t gw_rt_relay_start(uint16_t data_port, uint16_t control_port) {
  if (s.running) {
    return s.data_port == data_port && s.control_port == control_port
               ? ESP_OK : ESP_ERR_INVALID_STATE;
  }
  if (s.task) return ESP_ERR_INVALID_STATE;
  uint16_t bound_data = 0, bound_control = 0;
  s.data_sock = socket_utils_bind_udp(data_port, 0, RT_RELAY_RCVBUF, &bound_data);
  s.control_sock =
      socket_utils_bind_udp(control_port, 0, RT_RELAY_RCVBUF / 2, &bound_control);
  if (s.data_sock < 0 || s.control_sock < 0 || bound_data != data_port ||
      bound_control != control_port) {
    ESP_LOGE(TAG, "binding realtime ports %u/%u failed", (unsigned)data_port,
             (unsigned)control_port);
    if (s.data_sock >= 0) close(s.data_sock);
    if (s.control_sock >= 0) close(s.control_sock);
    s.data_sock = s.control_sock = -1;
    return ESP_FAIL;
  }
  s.data_port = data_port;
  s.control_port = control_port;
  s.targets_gen = gw_speakers_audio_generation() - 1U;
  memset(s.nacks, 0, sizeof(s.nacks));
  memset(&s.win, 0, sizeof(s.win));
  s.win_start_us = esp_timer_get_time();
  s.running = true;
  if (xTaskCreatePinnedToCore(rt_relay_task, "gw_rt_relay", RT_RELAY_STACK,
                              NULL, RT_RELAY_PRIORITY, &s.task,
                              RT_RELAY_CORE) != pdPASS) {
    s.running = false;
    close(s.data_sock);
    close(s.control_sock);
    s.data_sock = s.control_sock = -1;
    return ESP_FAIL;
  }
  AP_LOGI(TAG, "realtime relay on data port %u, control port %u",
          (unsigned)data_port, (unsigned)control_port);
  return ESP_OK;
}

void gw_rt_relay_stop(void) {
  s.running = false;
  for (int i = 0; s.task && i < RT_RELAY_STOP_WAIT_MS / 10; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s.task) {
    ESP_LOGE(TAG, "realtime relay task did not stop within %d ms",
             RT_RELAY_STOP_WAIT_MS);
    return;
  }
  s.data_port = s.control_port = 0;
  taskENTER_CRITICAL(&s.mux);
  s.sender_control_valid = false;
  taskEXIT_CRITICAL(&s.mux);
}

bool gw_rt_relay_is_idle(void) { return !s.running && !s.task; }

uint16_t gw_rt_relay_control_port(void) { return s.control_port; }

void gw_rt_relay_set_sender_control(uint32_t ip, uint16_t port) {
  struct sockaddr_in a = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = ip,
  };
  taskENTER_CRITICAL(&s.mux);
  s.sender_control = a;
  s.sender_control_valid = ip != 0 && port != 0;
  taskEXIT_CRITICAL(&s.mux);
}
