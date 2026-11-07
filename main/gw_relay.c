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
#include "sound_engine.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_speakers.h"
#include "apple_clock.h"
#include "socket_utils.h"

static const char *TAG = "gw_relay";

/* Same placement as the speaker's buffered TCP reader (aac_fifo_rx). */
#define GW_RELAY_CORE        1
#define GW_RELAY_PRIORITY    4
#define GW_RELAY_STACK       5120U
#define GW_RELAY_CHUNK       4096U
#define GW_RELAY_POLL_MS     200
/* While a speaker is about to join or pacing holds the sending back, poll
 * often to go on promptly. */
#define GW_RELAY_HOLD_POLL_MS 20
#define GW_RELAY_STATS_MS    5000
#define GW_RELAY_STOP_WAIT_MS 1000
/* Also bounds lwIP's FIN-allocation retries on close; sends stay nonblocking. */
#define GW_RELAY_CLOSE_TIMEOUT_MS 200
/* Same bound as ap2_buffered_fifo: length bytes + 12-byte RTP header. */
#define GW_RELAY_MIN_WIRE_LEN 14U
/* The sender's audio waits here, in PSRAM, between reading and sending.
 * The sender is read whenever there is room, so at a FLUSH the old track is
 * here, dropped at once, instead of queued in TCP: read and sent at paced
 * speed only to be thrown away, that took 2-4 s per track change from an
 * iPhone (board, 11-02). A power of two: 64 s of 256 kbit/s AAC. */
#define GW_RELAY_RING_BYTES  (4U * 1024U * 1024U)
#define GW_RELAY_RING_MASK   (GW_RELAY_RING_BYTES - 1U)
/* Where each frame starts, with its sequence number and RTP time. */
#define GW_RELAY_INDEX       65536U
#define GW_RELAY_INDEX_MASK  (GW_RELAY_INDEX - 1U)
/* The stream waits this long for a speaker that is starting its listener or
 * being connected, so it does not miss the start: what it would miss is kept
 * in the buffer meanwhile. */
#define GW_RELAY_HOLD_MS     1000
#define GW_RELAY_CONNECT_TIMEOUT_MS 3000
#define GW_RELAY_RETRY_MS    1000
#define GW_RELAY_STALL_LOG_MS 5000
/* A playing speaker takes audio at least at real time: one that took none
 * for this long while another went further is stuck (gw_relay.h). */
#define GW_RELAY_STALL_DROP_MS  10000
/* ...and is connected again after this. */
#define GW_RELAY_STALL_RETRY_MS 30000
/* Send pacing. The sender sends a new track's audio as fast as the network
 * takes it (~300 KB/s per speaker seen), and the Gateway sends it again to
 * every speaker: with five speakers the Wi-Fi was busy enough that the
 * sender's PTP Syncs arrived late. The speakers keep up to ~90 s of audio,
 * so the relay sends at most GW_RELAY_PACE_FACTOR times real time (plus a
 * start of GW_RELAY_PACE_BURST_S at full speed); the buffer above takes
 * the sender's audio meanwhile. Real time comes from the frames' RTP
 * timestamps. */
#define GW_RELAY_PACE_FACTOR  2.0
#define GW_RELAY_PACE_BURST_S 2.0
/* RTP jumps more than this (a seek, a new track): pacing starts over. */
#define GW_RELAY_PACE_JUMP_S  1.0
/* Buffered frames carry a 23-bit sequence number (sound_engine.c). */
#define GW_RELAY_SEQ_MASK    0x007fffffU

typedef struct {
  uint64_t off; /* absolute byte offset of the frame in the stream */
  uint32_t seq;
  uint32_t rtp;
} frame_ix_t;

typedef struct {
  /* Frame walker over the TCP byte stream. */
  uint8_t len_bytes[2];
  uint8_t len_have;
  uint16_t wire_len;
  uint16_t body_have;
  uint8_t header[12];
  uint64_t frame_start;
  /* Per-connection totals and the current stats window. */
  uint64_t total_bytes;
  uint64_t total_packets;
  uint32_t win_bytes;
  uint32_t win_packets;
  uint32_t win_first_seq, win_last_seq;
  uint32_t win_first_rtp, win_last_rtp;
  int64_t win_start_us;
  uint32_t win_paced_ms;
  uint32_t win_dropped;
} relay_stream_t;

typedef enum {
  TGT_IDLE = 0,   /* no connection; connects while the sender is connected */
  TGT_CONNECTING, /* non-blocking connect in progress */
  TGT_SYNC,       /* connected: its stream starts at a frame boundary */
  TGT_LIVE,       /* receives the stream from a frame boundary on */
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
  uint64_t pos;        /* LIVE: next byte of the stream to send */
  uint64_t ix;         /* LIVE: the frame pos lies in (or the next one) */
  uint32_t flush_gen;  /* the last FLUSH this speaker's stream skipped */
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
  /* The buffer: bytes [tail, head) of the stream, frames [ix_tail, ix_head)
   * complete up to frames_end. */
  uint8_t *ring;
  frame_ix_t *ix;
  uint64_t head, tail, frames_end;
  uint64_t ix_head, ix_tail;
  /* Frames before release_ix may be sent (pacing); release_end is where
   * the first one not released starts. */
  uint64_t release_ix, release_end;
  bool pace_valid;
  uint32_t pace_rtp0, pace_last_rtp;
  int64_t pace_t0_us;
  bool paced;
  /* A FLUSH: frames before flush_seq are skipped. release_flush_gen catches
   * up once the first frame from flush_seq on has been read. Written by
   * gw_relay_flush() under flush_mux, copied by the relay task. */
  uint32_t flush_gen, flush_seq, release_flush_gen;
  /* A speaker about to join: the buffer keeps from hold_ix on for it. */
  bool hold_active;
  uint64_t hold_ix;
  /* The sender plays (gw_relay_set_playing), since when (relay task). */
  bool playing;
  bool was_playing;
  int64_t playing_since_us;
} s = {.listen_sock = -1, .client_sock = -1};

static portMUX_TYPE s_flush_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_flush_req_gen, s_flush_req_seq;

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* a is at or after b, modulo the 23-bit sequence. */
static bool seq_ge(uint32_t a, uint32_t b) {
  return ((a - b) & GW_RELAY_SEQ_MASK) < (GW_RELAY_SEQ_MASK + 1U) / 2U;
}

static frame_ix_t *fx(uint64_t i) { return &s.ix[i & GW_RELAY_INDEX_MASK]; }

static uint64_t frame_start(uint64_t i) {
  return i < s.ix_head ? fx(i)->off : s.frames_end;
}

static uint64_t frame_end(uint64_t i) {
  return i + 1U < s.ix_head ? fx(i + 1U)->off : s.frames_end;
}

static const char *tgt_state_name(tgt_state_t st) {
  switch (st) {
    case TGT_CONNECTING: return "connecting";
    case TGT_SYNC: return "sync";
    case TGT_LIVE: return "live";
    default: return "idle";
  }
}

static void take_flush(void);

/* A new sender connection: an empty buffer, pacing and FLUSH from scratch
 * (a FLUSH posted for the previous connection does not touch the new one:
 * its sequence numbers are unrelated). */
static void stream_reset(void) {
  take_flush();
  memset(&s.stream, 0, sizeof(s.stream));
  s.stream.win_start_us = esp_timer_get_time();
  s.head = s.tail = s.frames_end = 0;
  s.ix_head = s.ix_tail = 0;
  s.release_ix = s.release_end = 0;
  s.pace_valid = false;
  s.paced = false;
  s.hold_active = false;
  s.release_flush_gen = s.flush_gen;
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
    apple_clock_line_t ps = {0};
    apple_clock_get(&ps);
    const double kbit_s =
        span_us > 0 ? (double)st->win_bytes * 8000.0 / (double)span_us : 0.0;
    ESP_LOGI(TAG,
             "RELAY rx %" PRIu32 " pkts %" PRIu32 " B (%.0f kbit/s) seq %" PRIu32
             "..%" PRIu32 " rtp %" PRIu32 "..%" PRIu32 " | total %" PRIu64
             " pkts %" PRIu64 " KiB | buffered %" PRIu32 " KiB, flushed %" PRIu32
             " pkts, paced %" PRIu32 " ms | apple_clock ready=%d gm=%016" PRIx64
             " offset=%" PRId64 " ns",
             st->win_packets, st->win_bytes, kbit_s, st->win_first_seq,
             st->win_last_seq, st->win_first_rtp, st->win_last_rtp,
             st->total_packets, st->total_bytes / 1024U,
             (uint32_t)((s.head - s.tail) / 1024U), st->win_dropped,
             st->win_paced_ms, ps.ready, ps.grandmaster_clock_id,
             ps.ready ? apple_clock_at(&ps, now_us * 1000LL) - now_us * 1000LL : 0LL);
    log_targets();
  }
  st->win_bytes = 0;
  st->win_packets = 0;
  st->win_paced_ms = 0;
  st->win_dropped = 0;
  st->win_start_us = now_us;
}

static void frame_done(void) {
  relay_stream_t *st = &s.stream;
  const uint32_t seq = read_be32(st->header) & GW_RELAY_SEQ_MASK;
  const uint32_t rtp = read_be32(st->header + 4);
  if (st->win_packets == 0U) {
    st->win_first_seq = seq;
    st->win_first_rtp = rtp;
  }
  st->win_last_seq = seq;
  st->win_last_rtp = rtp;
  st->win_packets++;
  st->total_packets++;
  *fx(s.ix_head) = (frame_ix_t){st->frame_start, seq, rtp};
  s.ix_head++;
  s.frames_end = st->frame_start + st->wire_len;
}

/* Walk the frames in bytes just read into the buffer at stream offset abs.
 * False on invalid framing, which aborts the connection exactly like the
 * speaker's FIFO does. */
static bool consume(const uint8_t *data, size_t len, uint64_t abs) {
  relay_stream_t *st = &s.stream;
  const uint8_t *const base = data;
  st->total_bytes += len;
  st->win_bytes += (uint32_t)len;
  while (len > 0) {
    if (st->len_have < 2U) {
      if (st->len_have == 0U) st->frame_start = abs + (uint64_t)(data - base);
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

/* Take a FLUSH posted by gw_relay_flush(). */
static void take_flush(void) {
  taskENTER_CRITICAL(&s_flush_mux);
  const uint32_t gen = s_flush_req_gen, seq = s_flush_req_seq;
  taskEXIT_CRITICAL(&s_flush_mux);
  if (gen == s.flush_gen) return;
  s.flush_gen = gen;
  s.flush_seq = seq;
}

/* Frames are released for sending as pacing allows. A FLUSH moves the
 * release on to the first frame from its sequence number at once, and
 * pacing starts over there (a new track). */
static void advance_release(int64_t now_us) {
  if (s.release_flush_gen != s.flush_gen) {
    uint64_t j = s.release_ix;
    while (j < s.ix_head && !seq_ge(fx(j)->seq, s.flush_seq)) j++;
    s.stream.win_dropped += (uint32_t)(j - s.release_ix);
    s.release_ix = j;
    if (j < s.ix_head) {
      s.release_flush_gen = s.flush_gen;
      s.pace_valid = false;
    }
  }
  const double rate = (double)sound_engine_get_sample_rate();
  s.paced = false;
  while (s.release_flush_gen == s.flush_gen && s.release_ix < s.ix_head) {
    const uint32_t rtp = fx(s.release_ix)->rtp;
    if (!s.pace_valid) {
      s.pace_valid = true;
      s.pace_rtp0 = rtp;
      s.pace_t0_us = now_us;
    } else {
      const int32_t step = (int32_t)(rtp - s.pace_last_rtp);
      if (step < 0 || (double)step > GW_RELAY_PACE_JUMP_S * rate) {
        s.pace_rtp0 = rtp; /* a seek or a new track */
        s.pace_t0_us = now_us;
      }
    }
    const double media_s = (double)(uint32_t)(rtp - s.pace_rtp0) / rate;
    const double elapsed_s = (double)(now_us - s.pace_t0_us) / 1e6;
    const double allowed_s = GW_RELAY_PACE_BURST_S + GW_RELAY_PACE_FACTOR * elapsed_s;
    if (allowed_s - media_s > GW_RELAY_PACE_BURST_S) {
      /* Held back by the speakers or the sender: no credit beyond one burst
       * piles up for later. */
      s.pace_t0_us = now_us - (int64_t)(media_s / GW_RELAY_PACE_FACTOR * 1e6);
    } else if (media_s > allowed_s) {
      s.paced = true;
      break;
    }
    s.pace_last_rtp = rtp;
    s.release_ix++;
  }
  s.release_end = frame_start(s.release_ix);
}

/* A speaker whose stream has not yet skipped the latest FLUSH: at a frame
 * boundary it moves on to the first frame from the FLUSH's sequence number
 * (or past every frame read so far, until that one comes). */
static void target_flush(relay_target_t *t) {
  if (t->flush_gen == s.flush_gen || t->pos != frame_start(t->ix)) return;
  uint64_t j = t->ix;
  while (j < s.ix_head && !seq_ge(fx(j)->seq, s.flush_seq)) j++;
  t->ix = j;
  t->pos = frame_start(j);
  if (j < s.ix_head) t->flush_gen = s.flush_gen;
}

/* How far the speaker's stream may be sent now. */
static uint64_t target_limit(const relay_target_t *t) {
  uint64_t limit = s.release_end;
  /* A FLUSH waits for the end of the frame being sent. */
  if (t->flush_gen != s.flush_gen && t->ix < s.ix_head) {
    const uint64_t end = frame_end(t->ix);
    if (end < limit) limit = end;
  }
  return limit;
}

/* The oldest frame a live speaker still needs: where a new one starts. */
static uint64_t live_start_ix(void) {
  uint64_t m = s.release_ix;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const relay_target_t *t = &s.tgt[i];
    if (t->state == TGT_LIVE && t->ix < m) m = t->ix;
  }
  return m < s.ix_tail ? s.ix_tail : m;
}

/* Free what no speaker needs any more. */
static void advance_tail(void) {
  uint64_t m = live_start_ix();
  if (s.hold_active && s.hold_ix < m) m = s.hold_ix;
  if (m > s.ix_tail) s.ix_tail = m;
  s.tail = frame_start(s.ix_tail);
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
  const struct timeval tx_tv = {
      .tv_sec = 0, .tv_usec = GW_RELAY_CLOSE_TIMEOUT_MS * 1000};
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tx_tv, sizeof(tx_tv));
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

/* True while a speaker is about to join. A connected one waits only for the
 * first frame: without the hold, a burst released before that frame
 * arrives would be freed, and the speaker would start that much late
 * (2.4 s on the board, v4.5.37). */
static bool should_hold(int64_t now_us) {
  const int64_t hold_us = GW_RELAY_HOLD_MS * 1000LL;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const relay_target_t *t = &s.tgt[i];
    if (t->state == TGT_SYNC) return true;
    if (t->want.pending && now_us - t->want.pending_since_us < hold_us)
      return true;
    if (t->state == TGT_CONNECTING && t->hold && now_us - t->since_us < hold_us)
      return true;
  }
  return false;
}

/* A connected speaker starts with the slowest live one (or, while one is
 * about to join, where the hold began), at a frame boundary. */
static void join_targets(int64_t now_us) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    relay_target_t *t = &s.tgt[i];
    if (t->state != TGT_SYNC) continue;
    uint64_t start = live_start_ix();
    if (s.hold_active && s.hold_ix < start) start = s.hold_ix;
    if (start >= s.ix_head) continue; /* no frame yet */
    t->state = TGT_LIVE;
    t->ix = start;
    t->pos = fx(start)->off;
    t->flush_gen = s.release_flush_gen == s.flush_gen ? s.flush_gen
                                                      : s.flush_gen - 1U;
    t->blocked_since_us = now_us;
    AP_LOGI(TAG, "speaker %s audio starts at the next frame boundary",
            t->want.name);
  }
}

static void send_pending(relay_target_t *t, int64_t now_us) {
  if (t->state != TGT_LIVE) return;
  target_flush(t);
  const uint64_t limit = target_limit(t);
  if (t->pos >= limit) {
    t->blocked_since_us = now_us; /* nothing to send: not stalled */
    return;
  }
  const size_t at = (size_t)(t->pos & GW_RELAY_RING_MASK);
  size_t len = (size_t)(limit - t->pos);
  if (len > GW_RELAY_RING_BYTES - at) len = GW_RELAY_RING_BYTES - at;
  const int n = send(t->fd, s.ring + at, len, MSG_DONTWAIT);
  if (n > 0) {
    t->pos += (uint64_t)n;
    while (t->ix < s.ix_head && frame_end(t->ix) <= t->pos) t->ix++;
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
    /* Its FIFO or the network is full: it waits, the others go on. */
  } else {
    close_target(t, "send failed", true);
  }
}

static bool wants_write(relay_target_t *t) {
  if (t->state != TGT_LIVE) return false;
  target_flush(t);
  return t->pos < target_limit(t);
}

/* Another live speaker is further on: this one, not the sender, is slow. */
static bool other_ahead(size_t self) {
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const relay_target_t *t = &s.tgt[i];
    if (i != self && t->state == TGT_LIVE && t->pos > s.tgt[self].pos) return true;
  }
  return false;
}

static void check_targets(int64_t now_us) {
  const bool playing = __atomic_load_n(&s.playing, __ATOMIC_ACQUIRE);
  if (playing && !s.was_playing) s.playing_since_us = now_us;
  s.was_playing = playing;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    relay_target_t *t = &s.tgt[i];
    if (t->state == TGT_CONNECTING &&
        now_us - t->since_us > GW_RELAY_CONNECT_TIMEOUT_MS * 1000LL) {
      close_target(t, "connect timed out", true);
      continue;
    }
    if (t->state != TGT_LIVE) continue;
    if (t->pos >= target_limit(t)) {
      /* Nothing to send is not a stall: the sender sends in bursts (6 s
       * apart from the iPhone, 102 s at the end of a track on the board,
       * v4.5.38), and without this the first audio after such a gap read as
       * a stall that long. */
      t->blocked_since_us = now_us;
      continue;
    }
    if (!t->stall_logged &&
        now_us - t->blocked_since_us > GW_RELAY_STALL_LOG_MS * 1000LL) {
      /* Normal while paused with full FIFOs; otherwise this speaker keeps
       * old audio in the buffer. */
      AP_LOGW(TAG, "speaker %s has not taken audio for %d s",
              t->want.name, GW_RELAY_STALL_LOG_MS / 1000);
      t->stall_logged = true;
    }
    /* Stalled while the sender plays: counted from the later of its last
     * progress and the start of playing (a resume after a pause). */
    const int64_t since = t->blocked_since_us > s.playing_since_us
                              ? t->blocked_since_us : s.playing_since_us;
    if (playing && now_us - since > GW_RELAY_STALL_DROP_MS * 1000LL &&
        other_ahead(i)) {
      AP_LOGW(TAG, "speaker %s took no audio for %d s while the others did: "
                   "dropped from the stream so they keep playing, again in %d s",
              t->want.name, GW_RELAY_STALL_DROP_MS / 1000,
              GW_RELAY_STALL_RETRY_MS / 1000);
      close_target(t, "stalled", true);
      t->since_us = now_us + GW_RELAY_STALL_RETRY_MS * 1000LL;
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
  /* Mirror the sender: each speaker's connection ends with it. */
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    close_target(&s.tgt[i], "sender closed its connection", false);
    s.tgt[i].hold = true;
  }
  stream_reset();
}

static void accept_client(void) {
  const int c = accept(s.listen_sock, NULL, NULL);
  if (c < 0) return;
  const struct timeval tx_tv = {
      .tv_sec = 0, .tv_usec = GW_RELAY_CLOSE_TIMEOUT_MS * 1000};
  (void)setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tx_tv, sizeof(tx_tv));
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

/* Room in the buffer for another read: bytes, and frame entries for the
 * smallest frames it could hold. */
static bool can_receive(void) {
  return GW_RELAY_RING_BYTES - (s.head - s.tail) >= GW_RELAY_CHUNK &&
         GW_RELAY_INDEX - (s.ix_head - s.ix_tail) >
             GW_RELAY_CHUNK / GW_RELAY_MIN_WIRE_LEN + 1U;
}

static void receive(void) {
  const size_t at = (size_t)(s.head & GW_RELAY_RING_MASK);
  size_t room = GW_RELAY_RING_BYTES - (size_t)(s.head - s.tail);
  if (room > GW_RELAY_CHUNK) room = GW_RELAY_CHUNK;
  if (room > GW_RELAY_RING_BYTES - at) room = GW_RELAY_RING_BYTES - at;
  const int r = recv(s.client_sock, s.ring + at, room, 0);
  if (r > 0) {
    if (!consume(s.ring + at, (size_t)r, s.head)) {
      close_client("invalid framing");
      return;
    }
    s.head += (uint64_t)r;
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
    take_flush();
    const bool connected = s.client_sock >= 0;
    if (connected) start_connects(now_us);
    const bool hold = connected && should_hold(now_us);
    if (hold && !s.hold_active) {
      s.hold_active = true;
      s.hold_ix = live_start_ix();
    } else if (!hold) {
      s.hold_active = false;
    }
    advance_release(now_us);
    join_targets(now_us);
    advance_tail();
    const int sel_client = connected && can_receive() ? s.client_sock : -1;

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
      relay_target_t *t = &s.tgt[i];
      if (t->fd < 0) continue;
      FD_SET(t->fd, &rd); /* speakers send nothing: readable means closed */
      if (t->state == TGT_CONNECTING || wants_write(t)) FD_SET(t->fd, &wr);
      if (t->fd > maxfd) maxfd = t->fd;
    }
    const int poll_ms = hold || s.paced ? GW_RELAY_HOLD_POLL_MS : GW_RELAY_POLL_MS;
    if (s.paced) s.stream.win_paced_ms += (uint32_t)poll_ms;
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

    /* The sender first, so what it sent can go out in this pass; then the
     * speakers; then a new sender. Closing happens only after every
     * descriptor in the sets has been looked at. */
    if (n > 0 && sel_client >= 0 && sel_client == s.client_sock &&
        FD_ISSET(sel_client, &rd)) {
      receive();
      take_flush();
      advance_release(now_us);
    }
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
    check_targets(now_us); /* may drop a stalled speaker */

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
  /* Kept for the next stream once allocated. */
  if (!s.ring) {
    s.ring = heap_caps_malloc(GW_RELAY_RING_BYTES,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!s.ix) {
    s.ix = heap_caps_malloc(GW_RELAY_INDEX * sizeof(frame_ix_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!s.ring || !s.ix) {
    ESP_LOGE(TAG, "no PSRAM for the %u KiB audio buffer",
             (unsigned)(GW_RELAY_RING_BYTES / 1024U));
    return ESP_ERR_NO_MEM;
  }

  uint16_t bound = requested_port;
  s.listen_sock = socket_utils_bind_tcp_listener(requested_port, 1, true, &bound);
  if (s.listen_sock < 0) return ESP_FAIL;
  s.port = bound;
  stream_reset();
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

void gw_relay_set_playing(bool playing) {
  __atomic_store_n(&s.playing, playing, __ATOMIC_RELEASE);
}

void gw_relay_flush(uint32_t until_seq) {
  taskENTER_CRITICAL(&s_flush_mux);
  s_flush_req_seq = until_seq & GW_RELAY_SEQ_MASK;
  s_flush_req_gen++;
  taskEXIT_CRITICAL(&s_flush_mux);
}
