#include "gw_speakers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "ptp_clock.h"
#include "ptp_clock_engine.h"
#include "settings.h"
#include "socket_utils.h"
#include "tsf_fit.h"

static const char *TAG = "gw_speakers";

#define GW_SPEAKERS_CORE      1
#define GW_SPEAKERS_PRIORITY  5
#define GW_SPEAKERS_STACK     5120U
#define GW_SPEAKERS_POLL_MS   200
#define GW_FRAME_MAX          (3U + SURROUND_MAX_PAYLOAD)
#define GW_BSSID_REFRESH_MS   2000
#define GW_TIME_LOG_MS        10000

typedef enum {
  SPK_FREE = 0,
  SPK_CONNECTED, /* TCP up, no HELLO yet */
  SPK_WAITING,   /* joined during a stream: waits for the sender's FLUSH */
  SPK_ACTIVE,    /* receives every forwarded call */
} spk_state_t;

typedef struct {
  int fd;
  spk_state_t state;
  bool dead; /* a send failed: shut down, the task closes it */
  char ip[16];
  uint32_t ip_addr; /* network byte order */
  surround_hello_t hello;
  surround_config_t config; /* last CONFIG from the speaker */
  bool config_valid;
  surround_status_t status; /* last STATUS */
  int64_t status_us;        /* when it came, 0 = never */
  /* Audio target (gw_audio_target_t), from START_RESULT; 0 while none. */
  uint16_t audio_port;      /* buffered TCP */
  uint16_t rt_data_port;    /* realtime UDP */
  uint16_t rt_control_port;
  uint32_t audio_id;
  bool audio_pending;
  int64_t audio_pending_since_us;
  int64_t last_rx_us;
  uint8_t rx[GW_FRAME_MAX];
  size_t rx_have;
} gw_speaker_t;

/* The session state the active speakers already have, replayed to a waiting
 * speaker when it joins at a FLUSH. */
typedef enum {
  REC_STREAM_TYPE,
  REC_FORMAT,
  REC_ENCRYPTION,
  REC_START,
  REC_CLIENT_CONTROL,
  REC_LATENCY,
  REC_VOLUME,
  REC_ANCHOR, /* SET_ANCHOR or REALTIME_ANCHOR */
  REC_PLAY, /* last SET_PLAYING or PAUSE */
  REC_COUNT
} rec_kind_t;

typedef struct {
  bool valid;
  uint8_t type;
  uint8_t len;
  uint8_t payload[SURROUND_MAX_PAYLOAD];
} rec_t;

static SemaphoreHandle_t s_lock;
static int s_listen = -1;
static gw_speaker_t s_spk[SURROUND_MAX_SPEAKERS];
static rec_t s_rec[REC_COUNT];
static uint32_t s_audio_gen;
static uint32_t s_next_audio_id;
/* Sender time (TIME messages); used by the speakers task only. */
static tsf_fit_t s_tsf;
static uint8_t s_bssid[6];
static int64_t s_bssid_read_us;
static int64_t s_time_log_us;
/* gw_speakers_post_volume() -> speakers task. */
static int32_t s_volume_next;
static bool s_volume_dirty;

/* Caller holds s_lock. */
static void audio_changed(void) {
  __atomic_add_fetch(&s_audio_gen, 1U, __ATOMIC_RELEASE);
}

static const char *state_name(spk_state_t st) {
  switch (st) {
    case SPK_CONNECTED: return "connected";
    case SPK_WAITING: return "waiting for FLUSH";
    case SPK_ACTIVE: return "active";
    default: return "free";
  }
}

static const char *spk_label(const gw_speaker_t *sp) {
  return sp->hello.name[0] ? sp->hello.name : sp->ip;
}

static bool has_audio(const gw_speaker_t *sp) {
  return sp->audio_port || sp->rt_data_port || sp->audio_pending;
}

/* Caller holds s_lock. */
static void clear_audio(gw_speaker_t *sp) {
  if (!has_audio(sp)) return;
  sp->audio_port = 0;
  sp->rt_data_port = 0;
  sp->rt_control_port = 0;
  sp->audio_pending = false;
  audio_changed();
}

/* Caller holds s_lock. */
static void send_frame(gw_speaker_t *sp, uint8_t type, const void *payload,
                       size_t len) {
  if (sp->fd < 0 || sp->dead || len > SURROUND_MAX_PAYLOAD) return;
  uint8_t buf[GW_FRAME_MAX];
  const size_t body = 1U + len;
  buf[0] = (uint8_t)(body >> 8);
  buf[1] = (uint8_t)body;
  buf[2] = type;
  if (len) memcpy(buf + 3, payload, len);
  const int n = send(sp->fd, buf, 2U + body, MSG_DONTWAIT);
  if (n != (int)(2U + body)) {
    /* A partial frame would break the framing: drop the speaker. */
    AP_LOGW(TAG, "speaker %s dropped: send of message 0x%02x failed (%d, errno=%d)",
            spk_label(sp), type, n, errno);
    sp->dead = true;
    shutdown(sp->fd, SHUT_RDWR);
    clear_audio(sp);
    return;
  }
  /* The speaker opens (START_*) or closes (STOP) its audio ports: the
   * relays follow. */
  if (type == SURROUND_MSG_START_BUFFERED) {
    clear_audio(sp);
    sp->audio_pending = true;
    sp->audio_pending_since_us = esp_timer_get_time();
    audio_changed();
  } else if (type == SURROUND_MSG_START_STREAM || type == SURROUND_MSG_STOP) {
    clear_audio(sp);
  }
}

/* Caller holds s_lock. Only the speakers task closes sockets, so the fd set it
 * selects on never contains a closed descriptor. */
static void close_speaker(gw_speaker_t *sp, const char *why) {
  if (sp->fd < 0) return;
  AP_LOGI(TAG, "speaker %s disconnected (%s)", spk_label(sp), why);
  if (has_audio(sp)) audio_changed();
  close(sp->fd);
  memset(sp, 0, sizeof(*sp));
  sp->fd = -1;
}

static bool session_active(void) {
  return s_rec[REC_FORMAT].valid || s_rec[REC_START].valid;
}

/* Realtime audio is near-live (about 2 s ahead) and carries its timing in
 * every second's sync packet: a speaker can join at once. */
static bool realtime_session(void) {
  return s_rec[REC_STREAM_TYPE].valid &&
         s_rec[REC_STREAM_TYPE].payload[0] == SURROUND_STREAM_REALTIME;
}

/* Caller holds s_lock. Bring one speaker to the state the others have. */
static void replay(gw_speaker_t *sp) {
  for (int k = 0; k < REC_COUNT; ++k) {
    if (s_rec[k].valid)
      send_frame(sp, s_rec[k].type, s_rec[k].payload, s_rec[k].len);
  }
}

static int rec_kind(uint8_t type) {
  switch (type) {
    case SURROUND_MSG_SET_STREAM_TYPE: return REC_STREAM_TYPE;
    case SURROUND_MSG_SET_FORMAT: return REC_FORMAT;
    case SURROUND_MSG_SET_ENCRYPTION: return REC_ENCRYPTION;
    case SURROUND_MSG_START_BUFFERED:
    case SURROUND_MSG_START_STREAM: return REC_START;
    case SURROUND_MSG_SET_CLIENT_CONTROL: return REC_CLIENT_CONTROL;
    case SURROUND_MSG_SET_PLAYOUT_LATENCY: return REC_LATENCY;
    case SURROUND_MSG_SET_VOLUME: return REC_VOLUME;
    case SURROUND_MSG_SET_ANCHOR:
    case SURROUND_MSG_REALTIME_ANCHOR: return REC_ANCHOR;
    case SURROUND_MSG_SET_PLAYING:
    case SURROUND_MSG_PAUSE: return REC_PLAY;
    default: return -1;
  }
}

static bool is_flush(uint8_t type) {
  return type == SURROUND_MSG_SEEK_FLUSH ||
         type == SURROUND_MSG_DEFERRED_FLUSH ||
         type == SURROUND_MSG_IMMEDIATE_FLUSH;
}

void gw_speakers_send(surround_msg_type_t type, const void *payload,
                      size_t len) {
  if (!s_lock || len > SURROUND_MAX_PAYLOAD) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);

  if (is_flush(type)) {
    for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
      gw_speaker_t *sp = &s_spk[i];
      if (sp->fd < 0 || sp->dead || sp->state != SPK_WAITING) continue;
      replay(sp);
      if (!sp->dead) {
        sp->state = SPK_ACTIVE;
        AP_LOGI(TAG, "speaker %s joins at this FLUSH", spk_label(sp));
      }
    }
  }

  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    if (s_spk[i].state == SPK_ACTIVE) send_frame(&s_spk[i], type, payload, len);
  }

  if (type == SURROUND_MSG_STOP) {
    /* The stream ended: nothing left to join, so waiting speakers are now in
     * step with the rest. The volume is kept across streams. */
    for (int k = 0; k < REC_COUNT; ++k) {
      if (k != REC_VOLUME) s_rec[k].valid = false;
    }
    for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
      if (s_spk[i].state == SPK_WAITING) s_spk[i].state = SPK_ACTIVE;
    }
  } else {
    const int k = rec_kind(type);
    if (k >= 0) {
      s_rec[k].valid = true;
      s_rec[k].type = (uint8_t)type;
      s_rec[k].len = (uint8_t)len;
      if (len) memcpy(s_rec[k].payload, payload, len);
    }
  }

  xSemaphoreGive(s_lock);
}

/* Caller holds s_lock. */
static void handle_hello(gw_speaker_t *sp, const uint8_t *p, size_t len) {
  surround_hello_t h;
  if (len != sizeof(h)) {
    AP_LOGW(TAG, "%s: HELLO of %u bytes, expected %u", sp->ip, (unsigned)len,
            (unsigned)sizeof(h));
    close_speaker(sp, "bad HELLO");
    return;
  }
  memcpy(&h, p, sizeof(h));
  h.name[sizeof(h.name) - 1] = '\0';
  h.firmware[sizeof(h.firmware) - 1] = '\0';
  if (h.magic != SURROUND_HELLO_MAGIC || h.version != SURROUND_PROTO_VERSION) {
    AP_LOGW(TAG, "%s: protocol version %u, Gateway speaks %u", sp->ip,
            (unsigned)h.version, (unsigned)SURROUND_PROTO_VERSION);
    close_speaker(sp, "protocol version mismatch");
    return;
  }
  /* A rebooted speaker reconnects before its old link times out. */
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    gw_speaker_t *other = &s_spk[i];
    if (other != sp && other->fd >= 0 && other->state != SPK_CONNECTED &&
        memcmp(other->hello.mac, h.mac, sizeof(h.mac)) == 0) {
      close_speaker(other, "replaced by a new connection from the same speaker");
    }
  }
  sp->hello = h;
  const bool join_now = session_active() && realtime_session();
  sp->state = session_active() && !join_now ? SPK_WAITING : SPK_ACTIVE;
  AP_LOGI(TAG,
          "speaker %s %02x:%02x:%02x:%02x:%02x:%02x at %s, channel %d, fw %s: %s%s",
          h.name, h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5],
          sp->ip, (int)h.channel, h.firmware, state_name(sp->state),
          join_now ? ", joins the realtime stream now" : "");
  if (join_now) {
    replay(sp);
  } else if (s_rec[REC_VOLUME].valid) {
    send_frame(sp, s_rec[REC_VOLUME].type, s_rec[REC_VOLUME].payload,
               s_rec[REC_VOLUME].len);
  }
}

/* Caller holds s_lock. */
static void handle_message(gw_speaker_t *sp, uint8_t type, const uint8_t *p,
                           size_t len) {
  if (type == SURROUND_MSG_HELLO) {
    handle_hello(sp, p, len);
    return;
  }
  if (sp->state == SPK_CONNECTED) {
    AP_LOGW(TAG, "%s: message 0x%02x before HELLO", sp->ip, type);
    return;
  }
  switch (type) {
    case SURROUND_MSG_PONG:
      break;
    case SURROUND_MSG_STATUS:
      if (len != sizeof(sp->status)) break;
      memcpy(&sp->status, p, sizeof(sp->status));
      sp->status.codec[sizeof(sp->status.codec) - 1] = '\0';
      sp->status.label[sizeof(sp->status.label) - 1] = '\0';
      sp->status_us = esp_timer_get_time();
      break;
    case SURROUND_MSG_CONFIG: {
      if (len != sizeof(sp->config)) break;
      memcpy(&sp->config, p, sizeof(sp->config));
      sp->config_valid = true;
      AP_LOGI(TAG, "speaker %s: channel %d, trim %.1f dB, delay %" PRId32 " us",
              spk_label(sp), (int)sp->config.channel,
              sp->config.trim_db10 / 10.0, sp->config.delay_us);
      break;
    }
    case SURROUND_MSG_START_RESULT: {
      surround_start_result_t r;
      if (len != sizeof(r)) break;
      memcpy(&r, p, sizeof(r));
      const bool ok = r.err == ESP_OK;
      sp->audio_port = ok ? r.port : 0;
      sp->rt_data_port = ok ? r.rt_data_port : 0;
      sp->rt_control_port = ok ? r.rt_control_port : 0;
      sp->audio_id = 0;
      if (sp->audio_port || sp->rt_data_port) {
        if (++s_next_audio_id == 0U) ++s_next_audio_id; /* 0 means none */
        sp->audio_id = s_next_audio_id;
      }
      sp->audio_pending = false;
      audio_changed();
      if (r.rt_data_port || r.rt_control_port) {
        AP_LOGI(TAG, "speaker %s start_stream: %s, realtime ports %u/%u",
                spk_label(sp), esp_err_to_name(r.err),
                (unsigned)r.rt_data_port, (unsigned)r.rt_control_port);
      } else {
        AP_LOGI(TAG, "speaker %s start: %s, audio port %u", spk_label(sp),
                esp_err_to_name(r.err), (unsigned)r.port);
      }
      break;
    }
    default:
      AP_LOGW(TAG, "speaker %s: unknown message 0x%02x", spk_label(sp), type);
      break;
  }
}

/* Caller holds s_lock. Feeds received bytes through the frame parser. */
static void receive(gw_speaker_t *sp, const uint8_t *data, size_t n) {
  while (n > 0 && sp->fd >= 0) {
    size_t take = sizeof(sp->rx) - sp->rx_have;
    if (take > n) take = n;
    memcpy(sp->rx + sp->rx_have, data, take);
    sp->rx_have += take;
    data += take;
    n -= take;
    while (sp->fd >= 0 && sp->rx_have >= 2U) {
      const size_t body = ((size_t)sp->rx[0] << 8) | sp->rx[1];
      if (body < 1U || body > 1U + SURROUND_MAX_PAYLOAD) {
        close_speaker(sp, "invalid frame length");
        return;
      }
      if (sp->rx_have < 2U + body) break;
      handle_message(sp, sp->rx[2], sp->rx + 3, body - 1U);
      if (sp->fd < 0) return;
      sp->rx_have -= 2U + body;
      memmove(sp->rx, sp->rx + 2U + body, sp->rx_have);
    }
  }
}

static void accept_speaker(void) {
  struct sockaddr_in addr;
  socklen_t alen = sizeof(addr);
  const int c = accept(s_listen, (struct sockaddr *)&addr, &alen);
  if (c < 0) return;
  const int one = 1;
  (void)setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  xSemaphoreTake(s_lock, portMAX_DELAY);
  gw_speaker_t *sp = NULL;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS && !sp; ++i) {
    if (s_spk[i].fd < 0) sp = &s_spk[i];
  }
  if (!sp) {
    xSemaphoreGive(s_lock);
    AP_LOGW(TAG, "speaker from %s refused: all %u slots in use",
            inet_ntoa(addr.sin_addr), (unsigned)SURROUND_MAX_SPEAKERS);
    close(c);
    return;
  }
  memset(sp, 0, sizeof(*sp));
  sp->fd = c;
  sp->state = SPK_CONNECTED;
  sp->last_rx_us = esp_timer_get_time();
  strlcpy(sp->ip, inet_ntoa(addr.sin_addr), sizeof(sp->ip));
  sp->ip_addr = addr.sin_addr.s_addr;
  xSemaphoreGive(s_lock);
  AP_LOGI(TAG, "control connection from %s", sp->ip);
}

/* Speakers task: read the TSF once, extend the fit and tell every speaker
 * which sender PTP time the fitted TSF of this instant is. */
static void time_tick(int64_t now_us) {
  int64_t local_ns = 0, tsf_ns = 0;
  if (!tsf_fit_read_sample(&local_ns, &tsf_ns)) return;
  if (tsf_fit_add(&s_tsf, local_ns, tsf_ns) == TSF_FIT_RESTARTED) {
    AP_LOGW(TAG, "TSF stepped (access point restarted or changed): time fit "
                 "restarted");
  }
  surround_time_t m = {0};
  int64_t tsf_now_ns = 0;
  if (!tsf_fit_tsf_at(&s_tsf, local_ns, &tsf_now_ns)) return;
  m.tsf_ns = tsf_now_ns; /* packed member: no pointer to it */

  if (s_bssid_read_us == 0 ||
      now_us - s_bssid_read_us > GW_BSSID_REFRESH_MS * 1000LL) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      memcpy(s_bssid, ap.bssid, sizeof(s_bssid));
    }
    s_bssid_read_us = now_us;
  }
  memcpy(m.bssid, s_bssid, sizeof(m.bssid));

  ptp_clock_snapshot_t ps;
  ptp_clock_get_snapshot(&ps);
  uint64_t remote_ns = 0;
  if (ps.realtime_mode) {
    /* Realtime: the estimate RTSP and the realtime receiver use. */
    ptp_realtime_snapshot_t rs;
    ptp_clock_get_realtime_snapshot(&rs);
    if (rs.master_ready && rs.master_clock_id != 0 &&
        ptp_clock_engine_local_to_remote((uint64_t)local_ns, rs.master_offset_ns,
                                         &remote_ns) &&
        remote_ns <= (uint64_t)INT64_MAX) {
      m.ptp_ns = (int64_t)remote_ns;
      m.source_clock_id = rs.source_clock_id;
      m.grandmaster_clock_id = rs.master_clock_id;
      m.mastership_age_ms = rs.mastership_age_ms;
      m.sample_count = rs.sample_count;
      m.flags = SURROUND_TIME_PTP_VALID | SURROUND_TIME_PTP_LOCKED |
                SURROUND_TIME_REALTIME;
    }
  } else if (ps.valid && ps.source_clock_id != 0 &&
             ptp_clock_engine_local_to_remote((uint64_t)local_ns,
                                              ps.filtered_offset_ns, &remote_ns) &&
             remote_ns <= (uint64_t)INT64_MAX) {
    m.ptp_ns = (int64_t)remote_ns;
    m.source_clock_id = ps.source_clock_id;
    m.grandmaster_clock_id = ps.grandmaster_clock_id;
    m.mastership_age_ms = ps.mastership_age_ms;
    m.sample_count = ps.sample_count;
    m.flags = SURROUND_TIME_PTP_VALID | (ps.locked ? SURROUND_TIME_PTP_LOCKED : 0U);
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    gw_speaker_t *sp = &s_spk[i];
    if (sp->fd >= 0 && sp->state != SPK_CONNECTED) {
      send_frame(sp, SURROUND_MSG_TIME, &m, sizeof(m));
    }
  }
  xSemaphoreGive(s_lock);

  if (now_us - s_time_log_us >= GW_TIME_LOG_MS * 1000LL) {
    s_time_log_us = now_us;
    ESP_LOGI(TAG,
             "TIME tsf fit %u samples %+.3f ppm rms %.2f us restarts %u | PTP "
             "valid=%d locked=%d realtime=%d gm=%016" PRIx64 " offset=%" PRId64
             " ns",
             (unsigned)s_tsf.count, s_tsf.slope * 1e6, s_tsf.rms_ns / 1000.0,
             (unsigned)s_tsf.restarts, (m.flags & SURROUND_TIME_PTP_VALID) != 0,
             (m.flags & SURROUND_TIME_PTP_LOCKED) != 0, ps.realtime_mode,
             m.grandmaster_clock_id, ps.filtered_offset_ns);
  }
}

void gw_speakers_post_volume(int32_t volume_q15) {
  __atomic_store_n(&s_volume_next, volume_q15, __ATOMIC_RELEASE);
  __atomic_store_n(&s_volume_dirty, true, __ATOMIC_RELEASE);
}

/* Speakers task: send a volume posted by RTSP. */
static void forward_posted_volume(void) {
  if (!__atomic_exchange_n(&s_volume_dirty, false, __ATOMIC_ACQ_REL)) return;
  const int32_t v = __atomic_load_n(&s_volume_next, __ATOMIC_ACQUIRE);
  if (v == 0) {
    AP_LOGI(TAG, "FWD set_volume_q15 0 (mute)");
  } else {
    AP_LOGI(TAG, "FWD set_volume_q15 %" PRId32 " (%.1f dB)", v,
            20.0 * log10((double)v / 32768.0));
  }
  const surround_volume_t m = {.volume_q15 = v};
  gw_speakers_send(SURROUND_MSG_SET_VOLUME, &m, sizeof(m));
}

static void speakers_task(void *arg) {
  (void)arg;
  int64_t last_ping_us = 0;
  int64_t next_time_us = 0;
  uint8_t buf[256];
  for (;;) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s_listen, &rd);
    int maxfd = s_listen;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
      if (s_spk[i].fd >= 0) {
        FD_SET(s_spk[i].fd, &rd);
        if (s_spk[i].fd > maxfd) maxfd = s_spk[i].fd;
      }
    }
    xSemaphoreGive(s_lock);

    /* Wake for the next TIME at the latest. */
    int64_t wait_us = next_time_us - esp_timer_get_time();
    if (wait_us < 0) wait_us = 0;
    if (wait_us > GW_SPEAKERS_POLL_MS * 1000LL) wait_us = GW_SPEAKERS_POLL_MS * 1000LL;
    struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)wait_us};
    const int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
    if (n < 0 && errno != EINTR) {
      ESP_LOGE(TAG, "select errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    const int64_t now_us = esp_timer_get_time();

    if (n > 0 && FD_ISSET(s_listen, &rd)) accept_speaker();

    for (size_t i = 0; n > 0 && i < SURROUND_MAX_SPEAKERS; ++i) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      gw_speaker_t *sp = &s_spk[i];
      const int fd = sp->fd;
      xSemaphoreGive(s_lock);
      if (fd < 0 || !FD_ISSET(fd, &rd)) continue;
      const int r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
      xSemaphoreTake(s_lock, portMAX_DELAY);
      if (sp->fd == fd) {
        if (r > 0) {
          sp->last_rx_us = now_us;
          receive(sp, buf, (size_t)r);
        } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          close_speaker(sp, sp->dead ? "send failed" : "connection closed");
        }
      }
      xSemaphoreGive(s_lock);
    }

    forward_posted_volume();

    if (now_us >= next_time_us) {
      /* A fixed cadence, not drifting with the select wake-ups. */
      next_time_us = next_time_us == 0 || now_us - next_time_us >
                                              (int64_t)SURROUND_TIME_INTERVAL_MS * 1000LL
                         ? now_us + (int64_t)SURROUND_TIME_INTERVAL_MS * 1000LL
                         : next_time_us + (int64_t)SURROUND_TIME_INTERVAL_MS * 1000LL;
      time_tick(now_us);
    }

    const bool ping = now_us - last_ping_us >=
                      (int64_t)SURROUND_PING_INTERVAL_MS * 1000LL;
    if (ping) last_ping_us = now_us;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
      gw_speaker_t *sp = &s_spk[i];
      if (sp->fd < 0) continue;
      if (sp->dead) {
        close_speaker(sp, "send failed");
      } else if (now_us - sp->last_rx_us >
                 (int64_t)SURROUND_LINK_TIMEOUT_MS * 1000LL) {
        close_speaker(sp, sp->state == SPK_CONNECTED ? "no HELLO"
                                                     : "no answer to PING");
      } else if (ping && sp->state != SPK_CONNECTED) {
        send_frame(sp, SURROUND_MSG_PING, NULL, 0);
      }
    }
    xSemaphoreGive(s_lock);
  }
}

size_t gw_speakers_list(gw_speaker_info_t out[SURROUND_MAX_SPEAKERS]) {
  size_t n = 0;
  if (!s_lock) return 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const gw_speaker_t *sp = &s_spk[i];
    if (sp->fd < 0 || sp->dead || sp->state == SPK_CONNECTED) continue;
    gw_speaker_info_t *o = &out[n++];
    memset(o, 0, sizeof(*o));
    strlcpy(o->name, sp->hello.name, sizeof(o->name));
    strlcpy(o->ip, sp->ip, sizeof(o->ip));
    memcpy(o->mac, sp->hello.mac, sizeof(o->mac));
    strlcpy(o->firmware, sp->hello.firmware, sizeof(o->firmware));
    o->state = state_name(sp->state);
    o->config_valid = sp->config_valid;
    o->config = sp->config;
    o->audio = sp->audio_port != 0 || sp->rt_data_port != 0;
    /* A STATUS older than three periods is stale. */
    o->status_valid = sp->status_us != 0 &&
        esp_timer_get_time() - sp->status_us <
            3LL * SURROUND_STATUS_MS * 1000LL;
    o->status = sp->status;
  }
  xSemaphoreGive(s_lock);
  return n;
}

esp_err_t gw_speakers_set_config(const uint8_t mac[6],
                                 const surround_set_config_t *cfg) {
  if (!s_lock || !cfg) return ESP_ERR_INVALID_STATE;
  esp_err_t err = ESP_ERR_NOT_FOUND;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    gw_speaker_t *sp = &s_spk[i];
    if (sp->fd < 0 || sp->dead || sp->state == SPK_CONNECTED ||
        memcmp(sp->hello.mac, mac, sizeof(sp->hello.mac)) != 0)
      continue;
    send_frame(sp, SURROUND_MSG_SET_CONFIG, cfg, sizeof(*cfg));
    err = sp->dead ? ESP_FAIL : ESP_OK;
    break;
  }
  xSemaphoreGive(s_lock);
  return err;
}

uint32_t gw_speakers_audio_generation(void) {
  return __atomic_load_n(&s_audio_gen, __ATOMIC_ACQUIRE);
}

void gw_speakers_audio_targets(gw_audio_target_t out[SURROUND_MAX_SPEAKERS]) {
  memset(out, 0, sizeof(gw_audio_target_t) * SURROUND_MAX_SPEAKERS);
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    const gw_speaker_t *sp = &s_spk[i];
    if (sp->fd < 0 || sp->dead) continue;
    gw_audio_target_t *t = &out[i];
    if (sp->audio_port || sp->rt_data_port) {
      t->id = sp->audio_id;
      t->ip = sp->ip_addr;
      t->port = sp->audio_port;
      t->rt_data_port = sp->rt_data_port;
      t->rt_control_port = sp->rt_control_port;
    }
    t->pending = sp->audio_pending;
    t->pending_since_us = sp->audio_pending_since_us;
    strlcpy(t->name, spk_label(sp), sizeof(t->name));
  }
  xSemaphoreGive(s_lock);
}

esp_err_t gw_speakers_start(void) {
  if (s_lock) return ESP_OK;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) s_spk[i].fd = -1;
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;

  uint16_t bound = SURROUND_CONTROL_PORT;
  s_listen = socket_utils_bind_tcp_listener(SURROUND_CONTROL_PORT,
                                            SURROUND_MAX_SPEAKERS, true, &bound);
  if (s_listen < 0) {
    ESP_LOGE(TAG, "control listener on port %u failed",
             (unsigned)SURROUND_CONTROL_PORT);
    return ESP_FAIL;
  }
  if (xTaskCreatePinnedToCore(speakers_task, "gw_speakers", GW_SPEAKERS_STACK,
                              NULL, GW_SPEAKERS_PRIORITY, NULL,
                              GW_SPEAKERS_CORE) != pdPASS) {
    close(s_listen);
    s_listen = -1;
    return ESP_FAIL;
  }

  char name[65] = {0};
  if (settings_get_device_name(name, sizeof(name)) != ESP_OK || !name[0]) {
    strlcpy(name, "AirPlay Surround Gateway", sizeof(name));
  }
  mdns_txt_item_t txt[] = {{"ver", SURROUND_STR(SURROUND_PROTO_VERSION)}};
  esp_err_t err = mdns_service_add(name, SURROUND_MDNS_SERVICE,
                                   SURROUND_MDNS_PROTO, bound, txt, 1);
  if (err != ESP_OK) {
    /* Speakers can still reach the Gateway by a configured address. */
    ESP_LOGE(TAG, "mDNS %s.%s not advertised: %s", SURROUND_MDNS_SERVICE,
             SURROUND_MDNS_PROTO, esp_err_to_name(err));
  }
  ESP_LOGI(TAG, "speaker control listener on port %u (%s.%s)", (unsigned)bound,
           SURROUND_MDNS_SERVICE, SURROUND_MDNS_PROTO);
  return ESP_OK;
}
