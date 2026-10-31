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
#include "gw_calib.h"
#include "gw_fw.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "ptp_clock.h"
#include "ptp_clock_engine.h"
#include "settings.h"
#include "socket_utils.h"
#include "gw_ptp_probe.h"
#include "gw_timeline.h"
#include "tsf_fit.h"

static const char *TAG = "gw_speakers";

#define GW_SPEAKERS_CORE      1
#define GW_SPEAKERS_PRIORITY  5
#define GW_SPEAKERS_STACK     5120U
#define GW_SPEAKERS_POLL_MS   200
#define GW_FRAME_MAX          (3U + SURROUND_MAX_PAYLOAD)
#define GW_BSSID_REFRESH_MS   2000
#define GW_TIME_LOG_MS        10000
/* A speaker playing calibration chirps answers within this time. */
#define GW_CAL_LINK_GRACE_US  (15LL * 1000000LL)
/* Firmware from the Gateway: tries per speaker and image, how long one
 * install may take (counted only while nothing plays: the speaker restarts
 * only between streams), and how long nothing must have played (a track
 * change is a STOP and a new SETUP 50 ms apart). A speaker that went away
 * and is not back with the image by then may not start with it: the rollout
 * stops there (gw_fw_halt) instead of taking the next speaker. The speaker
 * fetches the image from the web server (main.c). */
#define GW_FW_ATTEMPTS        3
#define GW_FW_TIMEOUT_US      (180LL * 1000000LL)
#define GW_FW_HTTP_PORT       80

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
  /* CAL_PLAY sent: the speaker plays chirps and is silent meanwhile, so the
   * link timeout waits until this time. 0 = none. */
  int64_t cal_until_us;
  /* FW_UPDATE sent to a speaker of another protocol version: it stays
   * connected (without joining) until it restarts. */
  bool fw_only;
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
/* A copy for other tasks (room calibration), under s_lock. */
static tsf_fit_t s_tsf_shared;
static uint8_t s_bssid[6];
static int64_t s_bssid_read_us;
static int64_t s_time_log_us;
/* The last TIME with sender time: its identity and flags go on while the
 * line holds over a gap (gw_timeline_hold). Speakers task only. */
static surround_time_t s_time_last;
static int64_t s_time_last_us;
static uint32_t s_ptp_cursor; /* ptp_clock_read_samples() */
/* Firmware from the Gateway, under s_lock: tries per speaker for the
 * current image, and the one install under way. */
typedef struct {
  uint8_t mac[6];
  uint32_t generation;
  uint8_t attempts;
} fw_try_t;
static fw_try_t s_fw_tries[2 * SURROUND_MAX_SPEAKERS];
static uint32_t s_fw_tries_next;
static struct {
  bool active;
  uint8_t mac[6];
  char name[24];
  char ip[16];
  int64_t since_us; /* FW_UPDATE sent; GW_FW_TIMEOUT_US from here */
  bool left; /* its connection closed since FW_UPDATE (it restarted) */
} s_fw_busy;
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
  if (s_fw_busy.active && memcmp(s_fw_busy.mac, sp->hello.mac, 6) == 0) {
    s_fw_busy.left = true;
  }
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

/* A speaker in a stream may be silent this long (a Wi-Fi dropout) before it
 * is dropped; it plays on its buffer meanwhile. Caller holds s_lock. */
static int64_t link_timeout_us(const gw_speaker_t *sp) {
  const bool stream = session_active() && sp->state != SPK_CONNECTED;
  return (int64_t)(stream ? SURROUND_LINK_TIMEOUT_STREAM_MS
                          : SURROUND_LINK_TIMEOUT_MS) * 1000LL;
}

/* ---- Firmware from the Gateway (gw_fw.c); caller holds s_lock ---- */

static fw_try_t *fw_tries(const uint8_t mac[6], uint32_t generation) {
  fw_try_t *t = NULL;
  for (size_t i = 0; i < sizeof(s_fw_tries) / sizeof(s_fw_tries[0]); ++i) {
    if (memcmp(s_fw_tries[i].mac, mac, 6) == 0) t = &s_fw_tries[i];
  }
  if (!t) {
    t = &s_fw_tries[s_fw_tries_next];
    s_fw_tries_next = (s_fw_tries_next + 1U) % (sizeof(s_fw_tries) / sizeof(s_fw_tries[0]));
    memcpy(t->mac, mac, 6);
    t->generation = generation;
    t->attempts = 0;
  }
  if (t->generation != generation) {
    t->generation = generation; /* a new image: new tries */
    t->attempts = 0;
  }
  return t;
}

/* A speaker may update now: at once, even during a stream (that speaker
 * stops playing for it), only not during a room calibration. */
static bool fw_allowed(void) { return !gw_calib_running(); }

/* The speaker can take the image and runs another build. */
static bool fw_wanted(const surround_hello_t *h, const gw_fw_image_t *img) {
  return img->valid && !img->halted && h->version >= SURROUND_PROTO_FW_UPDATE &&
         strcmp(h->firmware, img->id) != 0 &&
         fw_tries(h->mac, img->generation)->attempts < GW_FW_ATTEMPTS;
}

static void fw_send(gw_speaker_t *sp, const surround_hello_t *h,
                    const gw_fw_image_t *img, int64_t now_us) {
  surround_fw_update_t m = {.size = img->size, .http_port = GW_FW_HTTP_PORT};
  memcpy(m.sha256, img->sha256, sizeof(m.sha256));
  send_frame(sp, SURROUND_MSG_FW_UPDATE, &m, sizeof(m));
  fw_tries(h->mac, img->generation)->attempts++;
  s_fw_busy.active = true;
  memcpy(s_fw_busy.mac, h->mac, sizeof(s_fw_busy.mac));
  strlcpy(s_fw_busy.name, h->name, sizeof(s_fw_busy.name));
  strlcpy(s_fw_busy.ip, sp->ip, sizeof(s_fw_busy.ip));
  s_fw_busy.since_us = now_us;
  s_fw_busy.left = false;
  AP_LOGI(TAG, "speaker %s (%s) runs %s: installing %s from the Gateway", h->name,
          sp->ip, h->firmware, img->id);
}

/* Once a second: the next speaker with another build. */
static void fw_tick(int64_t now_us) {
  if (s_fw_busy.active && now_us - s_fw_busy.since_us > GW_FW_TIMEOUT_US) {
    s_fw_busy.active = false;
    if (s_fw_busy.left) {
      /* It restarted and is not back with the image: it may not start with
       * it (or went back to its old one). No other speaker gets it. */
      ESP_LOGE(TAG, "speaker %s did not come back with the new firmware: the "
                   "update of the other speakers is stopped",
              s_fw_busy.name);
      gw_fw_halt(s_fw_busy.name);
    } else {
      /* Still on its first connection: the download or install failed and
       * it runs its old firmware. */
      AP_LOGW(TAG, "speaker %s did not install the new firmware: it keeps its old one",
              s_fw_busy.name);
    }
  }
  if (!fw_allowed() || s_fw_busy.active) return;
  gw_fw_image_t img;
  gw_fw_get(&img);
  if (!img.valid || img.halted) return;
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    gw_speaker_t *sp = &s_spk[i];
    if (sp->fd < 0 || sp->dead || sp->state == SPK_CONNECTED) continue;
    if (!fw_wanted(&sp->hello, &img)) continue;
    fw_send(sp, &sp->hello, &img, now_us);
    return;
  }
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
  if (sp->state != SPK_CONNECTED) {
    /* Once per connection: a second HELLO must not move a speaker that is
     * playing back to "waiting". */
    AP_LOGW(TAG, "speaker %s: repeated HELLO ignored", spk_label(sp));
    return;
  }
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
    /* It cannot join, but it can take the firmware that lets it. */
    const int64_t now_us = esp_timer_get_time();
    gw_fw_image_t img;
    gw_fw_get(&img);
    if (h.magic == SURROUND_HELLO_MAGIC && !s_fw_busy.active && fw_allowed() &&
        fw_wanted(&h, &img)) {
      sp->fw_only = true;
      sp->hello = h; /* its MAC: close_speaker() notes when it restarts */
      fw_send(sp, &h, &img, now_us);
      return;
    }
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
  if (s_fw_busy.active && memcmp(s_fw_busy.mac, h.mac, sizeof(h.mac)) == 0) {
    gw_fw_image_t img;
    gw_fw_get(&img);
    if (strcmp(h.firmware, img.id) == 0) {
      AP_LOGI(TAG, "speaker %s updated to %s", h.name, h.firmware);
      s_fw_busy.active = false;
    }
  }
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
    if (!sp->fw_only) AP_LOGW(TAG, "%s: message 0x%02x before HELLO", sp->ip, type);
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
    case SURROUND_MSG_CAL_RESULT: {
      surround_cal_result_t r;
      if (len != sizeof(r)) break;
      memcpy(&r, p, sizeof(r));
      sp->cal_until_us = 0;
      gw_calib_on_result(sp->hello.mac, &r);
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
 * which sender PTP time the fitted TSF of this instant is, on the smooth
 * line of gw_timeline.c. */
static void time_tick(int64_t now_us) {
  int64_t local_ns = 0, tsf_ns = 0;
  if (!tsf_fit_read_sample(&local_ns, &tsf_ns)) return;
  const bool tsf_restarted =
      tsf_fit_add(&s_tsf, local_ns, tsf_ns) == TSF_FIT_RESTARTED;
  if (tsf_restarted) {
    AP_LOGW(TAG, "TSF stepped (access point restarted or changed): time fit "
                 "restarted");
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_tsf_shared = s_tsf;
  xSemaphoreGive(s_lock);
  surround_time_t m = {0};
  int64_t tsf_now_ns = 0;
  if (!tsf_fit_tsf_at(&s_tsf, local_ns, &tsf_now_ns)) return;
  m.tsf_ns = tsf_now_ns; /* packed member: no pointer to it */

  /* The Syncs measured since the last tick, on the TSF: the line is fitted
   * to them (gw_timeline.c), not to the PTP filter. */
  ptp_clock_sample_t smp[16];
  uint32_t n_smp;
  while ((n_smp = ptp_clock_read_samples(&s_ptp_cursor, smp, 16)) > 0) {
    for (uint32_t i = 0; i < n_smp; ++i) {
      int64_t tsf_rx_ns = 0;
      if (tsf_fit_tsf_at(&s_tsf, smp[i].local_ns, &tsf_rx_ns)) {
        gw_timeline_add_sample(tsf_rx_ns, smp[i].remote_ns,
                               smp[i].source_clock_id ^
                                   (smp[i].grandmaster_clock_id * 31U));
      }
    }
  }

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

  gw_timeline_out_t line = {0};
  const int64_t filter_ns = m.ptp_ns; /* the PTP filter, for the log */
  if ((m.flags & SURROUND_TIME_PTP_VALID) &&
      gw_timeline_update(m.tsf_ns, tsf_restarted, &line)) {
    m.ptp_ns = line.ptp_ns;
    m.rate_ppb = line.rate_ppb;
    m.epoch = line.epoch;
    /* A speaker starts once this age passes its readiness (400 ms): count it
     * from when the line became ready, not from the PTP mastership. */
    if (m.mastership_age_ms > line.ready_ms) m.mastership_age_ms = line.ready_ms;
    s_time_last = m;
    s_time_last_us = now_us;
  } else if (!tsf_restarted && gw_timeline_hold(m.tsf_ns, &line)) {
    /* A gap in the sender's PTP (late Syncs, the realtime estimate not ready
     * for a moment): the speakers keep the same line instead of dropping
     * their time all at once. */
    const uint32_t gap_ms = (uint32_t)((now_us - s_time_last_us) / 1000LL);
    m.ptp_ns = line.ptp_ns;
    m.rate_ppb = line.rate_ppb;
    m.epoch = line.epoch;
    m.source_clock_id = s_time_last.source_clock_id;
    m.grandmaster_clock_id = s_time_last.grandmaster_clock_id;
    m.mastership_age_ms = s_time_last.mastership_age_ms + gap_ms;
    m.sample_count = s_time_last.sample_count;
    m.flags = s_time_last.flags;
  } else {
    gw_timeline_invalidate();
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
    gw_ptp_probe_log();
    int64_t best_ns = 0;
    uint32_t syncs = 0;
    if (gw_timeline_take_best(&best_ns, &syncs)) {
      /* Short, so the log keeps the whole line. */
      ESP_LOGI(TAG,
               "APPLE TV vs LINE: best Sync %+.3f ms (of %u) | line epoch %u "
               "%+.3f ppm%s, fit %+.3f ms over %u s | PTP filter %+.3f ms",
               (double)best_ns / 1e6, (unsigned)syncs, (unsigned)line.epoch,
               (double)line.rate_ppb / 1000.0, line.fitted ? " fitted" : "",
               (double)line.err_ns / 1e6, (unsigned)(line.span_ms / 1000U),
               (m.flags & SURROUND_TIME_PTP_VALID) && filter_ns != 0
                   ? (double)(filter_ns - m.ptp_ns) / 1e6 : 0.0);
    }
    ESP_LOGI(TAG,
             "TIME tsf fit %u samples %+.3f ppm rms %.2f us restarts %u | PTP "
             "valid=%d locked=%d realtime=%d gm=%016" PRIx64 " offset=%" PRId64
             " ns | line epoch %u rate %+.3f ppm%s, envelope %+.3f ms from it over "
             "%.0f s%s%s, PTP filter %+.3f ms from it",
             (unsigned)s_tsf.count, s_tsf.slope * 1e6, s_tsf.rms_ns / 1000.0,
             (unsigned)s_tsf.restarts, (m.flags & SURROUND_TIME_PTP_VALID) != 0,
             (m.flags & SURROUND_TIME_PTP_LOCKED) != 0, ps.realtime_mode,
             m.grandmaster_clock_id, ps.filtered_offset_ns, (unsigned)line.epoch,
             (double)line.rate_ppb / 1000.0, line.fitted ? " (fitted)" : "",
             (double)line.err_ns / 1e6, line.span_ms / 1000.0,
             line.settling ? " (settling)" : "",
             line.ready_ms == 0U && line.epoch != 0U ? " (not ready)" : "",
             (m.flags & SURROUND_TIME_PTP_VALID) && filter_ns != 0
                 ? (double)(filter_ns - m.ptp_ns) / 1e6 : 0.0);
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
  int64_t last_fw_us = 0;
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

    if (now_us - last_fw_us >= 1000000LL) {
      last_fw_us = now_us;
      xSemaphoreTake(s_lock, portMAX_DELAY);
      fw_tick(now_us);
      xSemaphoreGive(s_lock);
    }

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
      } else if (now_us - sp->last_rx_us > link_timeout_us(sp) &&
                 now_us >= sp->cal_until_us) {
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
  gw_fw_image_t img;
  gw_fw_get(&img);
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
    if (img.valid) {
      const surround_hello_t *h = &sp->hello;
      o->fw_update = strcmp(h->firmware, img.id) == 0 ? "current"
          : s_fw_busy.active && memcmp(s_fw_busy.mac, h->mac, 6) == 0 ? "updating"
          : h->version < SURROUND_PROTO_FW_UPDATE ? "manual"
          : img.halted ? "halted"
          : fw_tries(h->mac, img.generation)->attempts >= GW_FW_ATTEMPTS ? "failed"
                                                                         : "pending";
    }
  }
  xSemaphoreGive(s_lock);
  return n;
}

void gw_speakers_fw_busy(gw_fw_busy_t *out) {
  memset(out, 0, sizeof(*out));
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_fw_busy.active) {
    out->active = true;
    strlcpy(out->name, s_fw_busy.name, sizeof(out->name));
    strlcpy(out->ip, s_fw_busy.ip, sizeof(out->ip));
    out->seconds = (uint32_t)((esp_timer_get_time() - s_fw_busy.since_us) / 1000000LL);
  }
  xSemaphoreGive(s_lock);
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

esp_err_t gw_speakers_cal_play(const uint8_t mac[6],
                               const surround_cal_play_t *m) {
  if (!s_lock || !m) return ESP_ERR_INVALID_STATE;
  esp_err_t err = ESP_ERR_NOT_FOUND;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    gw_speaker_t *sp = &s_spk[i];
    if (sp->fd < 0 || sp->dead || sp->state == SPK_CONNECTED ||
        memcmp(sp->hello.mac, mac, sizeof(sp->hello.mac)) != 0)
      continue;
    send_frame(sp, SURROUND_MSG_CAL_PLAY, m, sizeof(*m));
    sp->cal_until_us = esp_timer_get_time() + GW_CAL_LINK_GRACE_US;
    err = sp->dead ? ESP_FAIL : ESP_OK;
    break;
  }
  xSemaphoreGive(s_lock);
  return err;
}

bool gw_speakers_tsf_to_local(int64_t tsf_ns, int64_t *local_ns) {
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool ok = tsf_fit_local_at(&s_tsf_shared, tsf_ns, local_ns);
  xSemaphoreGive(s_lock);
  return ok;
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
