#include "gw_relay.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
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
#include "ptp_clock.h"
#include "socket_utils.h"

static const char *TAG = "gw_relay";

/* Same placement as the speaker's buffered TCP reader (aac_fifo_rx). */
#define GW_RELAY_CORE        1
#define GW_RELAY_PRIORITY    4
#define GW_RELAY_STACK       5120U
#define GW_RELAY_CHUNK       4096U
#define GW_RELAY_POLL_MS     200
/* While the stream is held for a speaker, poll often to resume promptly. */
#define GW_RELAY_HOLD_POLL_MS 20
#define GW_RELAY_STATS_MS    5000
#define GW_RELAY_STOP_WAIT_MS 1000
/* Same bound as ap2_buffered_fifo: length bytes + 12-byte RTP header. */
#define GW_RELAY_MIN_WIRE_LEN 14U
/* The stream waits this long for a speaker that is starting its listener or
 * being connected, so it does not miss the start. The sender is far ahead of
 * playback, so a short hold costs nothing. */
#define GW_RELAY_HOLD_MS     1000
#define GW_RELAY_CONNECT_TIMEOUT_MS 3000
#define GW_RELAY_RETRY_MS    1000
#define GW_RELAY_STALL_LOG_MS 5000

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

typedef enum {
  TGT_IDLE = 0,   /* no connection; connects while the sender is connected */
  TGT_CONNECTING, /* non-blocking connect in progress */
  TGT_SYNC,       /* connected: its stream starts at the next frame */
  TGT_LIVE,       /* receives every byte from a frame boundary on */
} tgt_state_t;

/* One speaker's copy of the stream. Its connection mirrors the sender's:
 * opened when the sender connects, closed when the sender closes, so the
 * speaker's FIFO sees the same connection epochs as with a direct sender. */
typedef struct {
  gw_audio_target_t want;
  tgt_state_t state;
  int fd;
  bool hold;           /* first connect for this sender connection */
  int64_t since_us;    /* CONNECTING: connect start; IDLE: earliest retry */
  size_t off;          /* LIVE: bytes of the current chunk already sent */
  int64_t blocked_since_us;
  bool stall_logged;
  uint64_t total_bytes;
  uint32_t win_bytes;
} relay_target_t;

static struct {
  int listen_sock;
  int client_sock;
  uint16_t port;
  volatile bool running;
  TaskHandle_t task;
  relay_stream_t stream;
  relay_target_t tgt[SURROUND_MAX_SPEAKERS];
  uint32_t targets_gen;
  /* The chunk read from the sender, until every live speaker has it. */
  uint8_t chunk[GW_RELAY_CHUNK];
  size_t chunk_len;
  bool chunk_pending;
} s = {.listen_sock = -1, .client_sock = -1};

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static const char *tgt_state_name(tgt_state_t st) {
  switch (st) {
    case TGT_CONNECTING: return "connecting";
    case TGT_SYNC: return "sync";
    case TGT_LIVE: return "live";
    default: return "idle";
  }
}

static void stream_reset(void) {
  memset(&s.stream, 0, sizeof(s.stream));
  s.stream.win_start_us = esp_timer_get_time();
}

static void log_targets(void) {
  static char line[8U * 32U];
  size_t pos = 0;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    relay_target_t *t = &s.tgt[i];
    if (!t->want.port && t->fd < 0) continue;
    const int n = snprintf(line + pos, sizeof(line) - pos, " %.12s:%s %" PRIu32 "KiB",
                           t->want.name, tgt_state_name(t->state),
                           t->win_bytes / 1024U);
    t->win_bytes = 0;
    if (n < 0 || (size_t)n >= sizeof(line) - pos) break;
    pos += (size_t)n;
  }
  if (pos) ESP_LOGI(TAG, "RELAY tx%s", line);
  else ESP_LOGI(TAG, "RELAY tx: no speaker has a buffered listener");
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
    log_targets();
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
 * aborts the connection exactly like the speaker's FIFO does. *first_start is
 * the offset of the first frame that begins in this chunk, or -1. */
static bool consume(const uint8_t *data, size_t len, int *first_start) {
  relay_stream_t *st = &s.stream;
  const uint8_t *const base = data;
  *first_start = -1;
  st->total_bytes += len;
  st->win_bytes += (uint32_t)len;
  while (len > 0) {
    if (st->len_have < 2U) {
      if (st->len_have == 0U && *first_start < 0) {
        *first_start = (int)(data - base);
      }
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

/* retry: connect again after GW_RELAY_RETRY_MS while the sender stays. */
static void close_target(relay_target_t *t, const char *why, bool retry) {
  if (t->fd < 0) return;
  AP_LOGI(TAG, "speaker %s audio closed (%s): %" PRIu64 " KiB sent",
          t->want.name, why, t->total_bytes / 1024U);
  close(t->fd);
  t->fd = -1;
  t->state = TGT_IDLE;
  t->hold = false;
  t->off = 0;
  t->since_us = retry ? esp_timer_get_time() + GW_RELAY_RETRY_MS * 1000LL : 0;
}

static void open_target(relay_target_t *t, int64_t now_us) {
  t->total_bytes = 0;
  t->stall_logged = false;
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (fd < 0) {
    AP_LOGW(TAG, "speaker %s: no socket for audio (errno=%d)", t->want.name,
            errno);
    t->hold = false;
    t->since_us = now_us + GW_RELAY_RETRY_MS * 1000LL;
    return;
  }
  const int flags = fcntl(fd, F_GETFL, 0);
  (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(t->want.port),
      .sin_addr.s_addr = t->want.ip,
  };
  t->fd = fd;
  t->since_us = now_us;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
    t->state = TGT_SYNC;
    t->hold = false;
    AP_LOGI(TAG, "speaker %s audio connected to port %u", t->want.name,
            (unsigned)t->want.port);
  } else if (errno == EINPROGRESS) {
    t->state = TGT_CONNECTING;
  } else {
    AP_LOGW(TAG, "speaker %s audio connect to port %u failed (errno=%d)",
            t->want.name, (unsigned)t->want.port, errno);
    close_target(t, "connect failed", true);
  }
}

/* Follow the registry: a speaker that left, stopped or restarted its stream
 * loses its connection; a new listener is connected by start_connects(). */
static void refresh_targets(void) {
  const uint32_t gen = gw_speakers_audio_generation();
  if (gen == s.targets_gen) return;
  s.targets_gen = gen;
  gw_audio_target_t now[SURROUND_MAX_SPEAKERS];
  gw_speakers_audio_targets(now);
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    relay_target_t *t = &s.tgt[i];
    if (now[i].id != t->want.id) {
      close_target(t, now[i].id ? "speaker restarted its stream"
                                : "speaker left the stream",
                   false);
      t->state = TGT_IDLE;
      t->since_us = 0;
      t->hold = true; /* a new listener: do not let it miss the start */
    }
    t->want = now[i];
  }
}

static void start_connects(int64_t now_us) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    relay_target_t *t = &s.tgt[i];
    if (t->state == TGT_IDLE && t->fd < 0 && t->want.id && t->want.port &&
        now_us >= t->since_us) {
      open_target(t, now_us);
    }
  }
}

/* True while a speaker is about to join: the sender is not read meanwhile. */
static bool should_hold(int64_t now_us) {
  const int64_t hold_us = GW_RELAY_HOLD_MS * 1000LL;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const relay_target_t *t = &s.tgt[i];
    if (t->want.pending && now_us - t->want.pending_since_us < hold_us)
      return true;
    if (t->state == TGT_CONNECTING && t->hold && now_us - t->since_us < hold_us)
      return true;
  }
  return false;
}

static void send_pending(relay_target_t *t, int64_t now_us) {
  if (t->state != TGT_LIVE || t->off >= s.chunk_len) return;
  const int n = send(t->fd, s.chunk + t->off, s.chunk_len - t->off,
                     MSG_DONTWAIT);
  if (n > 0) {
    t->off += (size_t)n;
    t->total_bytes += (uint64_t)n;
    t->win_bytes += (uint32_t)n;
    if (t->stall_logged) {
      AP_LOGI(TAG, "speaker %s takes audio again after %" PRId64 " ms",
              t->want.name, (now_us - t->blocked_since_us) / 1000LL);
      t->stall_logged = false;
    }
    t->blocked_since_us = now_us;
  } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR)) {
    /* Its FIFO or the network is full: the stream waits for it. */
  } else {
    close_target(t, "send failed", true);
  }
}

/* A new chunk from the sender: live speakers get all of it, a speaker that
 * just connected starts at the first frame that begins in it. */
static void dispatch_chunk(int first_start, int64_t now_us) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    relay_target_t *t = &s.tgt[i];
    if (t->state == TGT_SYNC && first_start >= 0) {
      t->state = TGT_LIVE;
      t->off = (size_t)first_start;
      AP_LOGI(TAG, "speaker %s audio starts at the next frame boundary",
              t->want.name);
    } else if (t->state == TGT_LIVE) {
      t->off = 0;
    } else {
      continue;
    }
    t->blocked_since_us = now_us;
    send_pending(t, now_us);
  }
}

static bool chunk_delivered(void) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const relay_target_t *t = &s.tgt[i];
    if (t->state == TGT_LIVE && t->off < s.chunk_len) return false;
  }
  return true;
}

static void check_targets(int64_t now_us) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    relay_target_t *t = &s.tgt[i];
    if (t->state == TGT_CONNECTING &&
        now_us - t->since_us > GW_RELAY_CONNECT_TIMEOUT_MS * 1000LL) {
      close_target(t, "connect timed out", true);
    } else if (t->state == TGT_LIVE && s.chunk_pending &&
               t->off < s.chunk_len && !t->stall_logged &&
               now_us - t->blocked_since_us >
                   GW_RELAY_STALL_LOG_MS * 1000LL) {
      /* Normal while paused with full FIFOs; otherwise this speaker holds
       * back the others. */
      AP_LOGW(TAG, "speaker %s has not taken audio for %d s; the stream waits",
              t->want.name, GW_RELAY_STALL_LOG_MS / 1000);
      t->stall_logged = true;
    }
  }
}

static void close_client(const char *why) {
  if (s.client_sock < 0) return;
  log_window(true);
  AP_LOGI(TAG, "sender audio connection closed (%s): %" PRIu64 " pkts, %" PRIu64
               " KiB",
          why, s.stream.total_packets, s.stream.total_bytes / 1024U);
  close(s.client_sock);
  s.client_sock = -1;
  s.chunk_pending = false;
  /* Mirror the sender: each speaker's connection ends with it. */
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    close_target(&s.tgt[i], "sender closed its connection", false);
    s.tgt[i].hold = true;
  }
}

static void accept_client(void) {
  const int c = accept(s.listen_sock, NULL, NULL);
  if (c < 0) return;
  /* A newly accepted connection is a fresh sequential byte stream. */
  close_client("replaced by a new connection");
  s.client_sock = c;
  stream_reset();
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    s.tgt[i].hold = true;
    s.tgt[i].since_us = 0;
  }
  AP_LOGI(TAG, "sender audio connected on port %u", (unsigned)s.port);
}

static void receive_chunk(int64_t now_us) {
  const int r = recv(s.client_sock, s.chunk, sizeof(s.chunk), 0);
  if (r > 0) {
    int first_start = -1;
    if (!consume(s.chunk, (size_t)r, &first_start)) {
      close_client("invalid framing");
      return;
    }
    s.chunk_len = (size_t)r;
    dispatch_chunk(first_start, now_us);
    s.chunk_pending = !chunk_delivered();
  } else if (r == 0) {
    close_client("sender closed");
  } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    close_client("recv error");
  }
}

static void relay_task(void *arg) {
  (void)arg;
  uint8_t scratch[16];
  while (s.running) {
    int64_t now_us = esp_timer_get_time();
    refresh_targets();
    if (s.client_sock >= 0) start_connects(now_us);
    const bool hold = s.client_sock >= 0 && !s.chunk_pending &&
                      should_hold(now_us);
    const int sel_client =
        s.client_sock >= 0 && !s.chunk_pending && !hold ? s.client_sock : -1;

    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_SET(s.listen_sock, &rd);
    int maxfd = s.listen_sock;
    if (sel_client >= 0) {
      FD_SET(sel_client, &rd);
      if (sel_client > maxfd) maxfd = sel_client;
    }
    for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
      const relay_target_t *t = &s.tgt[i];
      if (t->fd < 0) continue;
      FD_SET(t->fd, &rd); /* speakers send nothing: readable means closed */
      if (t->state == TGT_CONNECTING ||
          (t->state == TGT_LIVE && s.chunk_pending && t->off < s.chunk_len)) {
        FD_SET(t->fd, &wr);
      }
      if (t->fd > maxfd) maxfd = t->fd;
    }
    const int poll_ms = hold ? GW_RELAY_HOLD_POLL_MS : GW_RELAY_POLL_MS;
    struct timeval tv = {.tv_sec = 0, .tv_usec = poll_ms * 1000};
    const int n = select(maxfd + 1, &rd, &wr, NULL, &tv);
    if (!s.running) break;
    if (n < 0) {
      if (errno != EINTR) {
        ESP_LOGE(TAG, "select errno=%d", errno);
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      continue;
    }
    now_us = esp_timer_get_time();

    /* Speakers first, then the sender, then a new sender: closing happens
     * only after every descriptor in the sets has been looked at. */
    for (size_t i = 0; n > 0 && i < SURROUND_MAX_SPEAKERS; ++i) {
      relay_target_t *t = &s.tgt[i];
      if (t->fd < 0) continue;
      if (FD_ISSET(t->fd, &rd)) {
        const int r = recv(t->fd, scratch, sizeof(scratch), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR)) {
          close_target(t, r == 0 ? "speaker closed" : "connection error", true);
          continue;
        }
      }
      if (!FD_ISSET(t->fd, &wr)) continue;
      if (t->state == TGT_CONNECTING) {
        int err = 0;
        socklen_t elen = sizeof(err);
        (void)getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err == 0) {
          t->state = TGT_SYNC;
          t->hold = false;
          AP_LOGI(TAG, "speaker %s audio connected to port %u in %" PRId64 " ms",
                  t->want.name, (unsigned)t->want.port,
                  (now_us - t->since_us) / 1000LL);
        } else {
          AP_LOGW(TAG, "speaker %s audio connect to port %u failed (errno=%d)",
                  t->want.name, (unsigned)t->want.port, err);
          close_target(t, "connect failed", true);
        }
      } else {
        send_pending(t, now_us);
      }
    }

    if (n > 0 && sel_client >= 0 && sel_client == s.client_sock &&
        FD_ISSET(sel_client, &rd)) {
      receive_chunk(now_us);
    }
    if (s.chunk_pending && chunk_delivered()) s.chunk_pending = false;
    check_targets(now_us);

    if (n > 0 && FD_ISSET(s.listen_sock, &rd)) accept_client();

    if (s.client_sock >= 0) log_window(false);
  }

  close_client("relay stopped");
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    close_target(&s.tgt[i], "relay stopped", false);
  }
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
  s.chunk_pending = false;
  /* Take the registry as it is now on the first pass. */
  s.targets_gen = gw_speakers_audio_generation() - 1U;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    memset(&s.tgt[i], 0, sizeof(s.tgt[i]));
    s.tgt[i].fd = -1;
  }
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
