/* The Gateway's own player, phase 3a. See gw_player.h.
 *
 * One task (Core 1, away from Wi-Fi and lwIP on Core 0) does it all, as the
 * web radio does: it hands the speakers over from AirPlay, reads the file,
 * splits it into one lane per speaker role (common/surround/surround_route.h),
 * encodes each lane as one-channel ALAC with the light encoder, seals every
 * sub-frame like an AirPlay buffered packet (one key, one nonce counter
 * over all lanes) and writes it to the Gateway's relay over a loopback TCP
 * connection in the relay's lane mode (lane = role + 2 in the RTP header).
 *
 * Timing as the radio: the Gateway's clock is the line, the anchor goes
 * START_DELAY ahead, and the file is read as fast as the lead allows
 * (MAX_LEAD ahead of the play position). At the end of the file the task
 * waits until the speakers have played what they hold, then stops. */

#include "gw_player.h"

#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_bench_alac_lite.h"
#include "gw_calib.h"
#include "gw_player_decim.h"
#include "gw_player_m4a.h"
#if CONFIG_GATEWAY_USB_PLAYER_USB
#include "gw_usb.h"
#endif
#include "gw_player_src.h"
#include "gw_radio.h"
#include "gw_relay.h"
#include "gw_speakers.h"
#include "lwip/sockets.h"
#include "rtsp_server.h"
#include "sodium.h"
#include "sound_engine.h"
#include "surround_player_formats.h"
#include "surround_route.h"
#include "web_server_internal.h"

static const char *TAG = "gw_player";

#define PL_TASK_STACK    12288
#define PL_TASK_PRIORITY 3
#define PL_TASK_CORE     1 /* filters, encoders, the relay socket: next to lwIP; the decoder task has core 0 */
#define PL_FRAME         352U   /* samples per ALAC frame, like AirPlay */
#define PL_READ_CHUNK    8192U
#define PL_HEADER_MAX    65536U /* the WAV data chunk / the M4A codec must come before this */
#define PL_START_DELAY_NS 2000000000LL
#define PL_MAX_LEAD_NS    20000000000LL
#define PL_MAX_LEAD_BUSY_NS 8000000000LL /* lanes over PL_BUSY_KBPS in all */
#define PL_BUSY_KBPS      4000
#define PL_MIN_LEAD_NS    250000000LL
#define PL_RESUME_DELAY_NS 1000000000LL /* resume: the speakers start again after this */
#define PL_RESEEK_DELAY_US 1000000LL    /* a new lane: wait for more speakers first */
#define PL_SENDER_STOP_MS 8000
#define PL_STOP_WAIT_MS   6000
#define PL_STATUS_LOG_MS  10000
#define PL_SEQ_MASK       0x007fffffU
#define PL_TAG_LEN        16U
#define PL_NONCE_TAIL     8U
#define PL_ENC_ORDER      4
#define PL_LANES          SURROUND_ROLES /* one per role, index role + 1 */
/* Sub-frames go to the relay in batches: one send() per ~600-byte
 * sub-frame kept the relay's loop busy waking for each small segment, and
 * 60 % of the player's time went into send() (2026-11-18). */
#define PL_WBUF           16384U
#define PL_WBUF_FLUSH     8192U

typedef enum { P_IDLE = 0, P_STARTING, P_PLAYING, P_BUFFERING, P_FINISHING,
               P_STOPPING, P_FAILED, P_PAUSED } player_state_t;

static const char *state_name(player_state_t s) {
  switch (s) {
    case P_STARTING: return "starting";
    case P_PLAYING: return "playing";
    case P_BUFFERING: return "buffering";
    case P_FINISHING: return "finishing";
    case P_STOPPING: return "stopping";
    case P_FAILED: return "failed";
    case P_PAUSED: return "paused";
    default: return "idle";
  }
}

typedef struct {
  player_state_t state;
  char message[96];
  char url[256];
  char format[64];  /* "WAV 48000 Hz / 24 bit / 5.1" */
  int rate, bits, channels, layout;
  int volume_db;
  int lead_ms;
  uint32_t lane_bytes[PL_LANES]; /* sent per lane, for the status kbit/s */
  uint8_t lane_bits[PL_LANES];   /* 0 = no lane */
  double position_s, duration_s;
  /* the last status window (PL_STATUS_LOG_MS): where the time went */
  uint32_t pcm_crc10;  /* CRC-32 of the first 10 s of decoded PCM (0 = not yet) */
  double perf_speed;   /* frames sent / real time */
  int perf_read_ms, perf_enc_ms, perf_send_ms, perf_read_kib, perf_dec_ms;
  bool duration_estimated;
  int index;           /* the queue's track the speakers play */
} player_status_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static player_status_t s_st;
static SemaphoreHandle_t s_ctl;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;
static atomic_bool s_running;
static atomic_bool s_stop;
static atomic_bool s_owns;
static apple_clock_line_t s_line;
static int64_t s_owns_since_us;
static uint16_t s_epoch = 0x5000;
/* the queue (phase 3d.3): URLs in PSRAM under s_qlock */
#define PL_QUEUE_MAX 64
#define PL_URL_MAX 256
static char (*s_queue)[PL_URL_MAX];
static int s_queue_n;
static int s_queue_start;
static SemaphoreHandle_t s_qlock;
#define PL_GOTO_NONE (-100)
#define PL_GOTO_NEXT (-1)
#define PL_GOTO_PREV (-2)
static atomic_int s_goto = PL_GOTO_NONE; /* next, previous, or a queue index */
static atomic_int s_pause_req;   /* 1 pause, 2 resume */
static atomic_llong s_seek_ms = -1;
static int s_next_volume_db = -30;

static void st_set(player_state_t state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void st_set(player_state_t state, const char *fmt, ...) {
  char m[sizeof(s_st.message)] = "";
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
  }
  taskENTER_CRITICAL(&s_mux);
  s_st.state = state;
  strlcpy(s_st.message, m, sizeof(s_st.message));
  taskEXIT_CRITICAL(&s_mux);
  if (m[0]) ESP_LOGI(TAG, "%s: %s", state_name(state), m);
}

static int64_t now_ns(void) { return esp_timer_get_time() * 1000LL; }

static int32_t db_to_q15(int db) {
  return (int32_t)lroundf(32768.0f * powf(10.0f, (float)db / 20.0f));
}

static int clamp_db(int db) { return db > 0 ? 0 : db < -60 ? -60 : db; }

/* ---- the player's clock line (as the radio's, "PL" instead of "RD") ---- */

static uint64_t player_clock_id(void) {
  uint8_t mac[6] = {0};
  (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
  return ((uint64_t)mac[0] << 56) | ((uint64_t)mac[1] << 48) |
         ((uint64_t)mac[2] << 40) | (0x50ULL << 32) | (0x4CULL << 24) |
         ((uint64_t)mac[3] << 16) | ((uint64_t)mac[4] << 8) | mac[5];
}

bool gw_player_clock_line(apple_clock_line_t *out) {
  if (!atomic_load(&s_owns)) return false;
  taskENTER_CRITICAL(&s_mux);
  *out = s_line;
  taskEXIT_CRITICAL(&s_mux);
  return true;
}

bool gw_player_active(void) { return atomic_load(&s_owns); }

void gw_player_yield(void) {
  if (!atomic_load(&s_owns)) return;
  if (s_task && xTaskGetCurrentTaskHandle() == s_task) return;
  ESP_LOGI(TAG, "another sender takes the speakers: the player stops");
  gw_player_stop();
}

static void take_speakers(void) {
  const apple_clock_line_t l = {
      .ready = true,
      .realtime = false,
      .source_clock_id = player_clock_id(),
      .grandmaster_clock_id = player_clock_id(),
      .epoch = s_epoch++,
      .base_local_ns = 0,
      .base_apple_ns = 0,
      .rate_ppb = 0,
      .ready_since_ns = 0,
      .samples = 1000,
      .points = 100,
  };
  taskENTER_CRITICAL(&s_mux);
  s_line = l;
  taskEXIT_CRITICAL(&s_mux);
  s_owns_since_us = esp_timer_get_time();
  atomic_store(&s_owns, true);
  ESP_LOGI(TAG, "the speakers follow the Gateway's clock %016" PRIx64 " (epoch %u)",
           l.grandmaster_clock_id, (unsigned)l.epoch);
}

/* As the radio: AirPlay lets go, and listens again at once. */
static bool airplay_release(void) {
  st_set(P_STARTING, "Disconnecting the AirPlay sender");
  rtsp_server_stop();
  for (int i = 0; i < PL_SENDER_STOP_MS / 100; ++i) {
    if (rtsp_server_is_idle() && sound_engine_is_idle()) return true;
    if (atomic_load(&s_stop)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return rtsp_server_is_idle() && sound_engine_is_idle();
}

static bool airplay_listen(void) {
  const esp_err_t err = rtsp_server_start();
  if (err != ESP_OK)
    ESP_LOGE(TAG, "AirPlay listener did not start again: %s", esp_err_to_name(err));
  return err == ESP_OK;
}

/* ---- lanes ---- */

typedef struct {
  bool on;
  int role;
  surround_sig_t sig;
  int bits;
  uint32_t ssrc;
  alac_lite_t enc;
  uint32_t bytes;
} lane_t;

enum { KIND_WAV = 0, KIND_FLAC, KIND_MP3, KIND_AAC, KIND_M4A };

typedef struct {
  uint64_t sample, offset;
} flac_seekpt_t;

/* What the status shows of a track: the input's, published when the
 * speakers get to it. */
typedef struct {
  int ix;
  char url[PL_URL_MAX];
  char format[64];
  int rate, bits, channels, layout;
  double duration_s;
} track_info_t;

/* Gapless: where the next track starts in the stream (RTP time). */
typedef struct {
  uint32_t rtp;
  track_info_t ti;
} boundary_t;
#define PL_BOUNDARIES 4

typedef struct {
  /* source */
  player_source_t src;
  int kind;
  wav_info_t wav;     /* the format, for every kind (from the decoder for MP3, AAC, M4A) */
  uint64_t total_frames;
  char m4a_codec[5];  /* M4A: "alac", "mp4a", ".mp3" */
  double m4a_duration_s;
  /* M4A (phase 3d.2): its own index; each sample to esp_audio_dec */
  m4a_track_t m4a;
  m4a_iter_t m4a_it;
  esp_audio_dec_handle_t adec;
  uint8_t *afr;       /* an AAC sample behind its ADTS header */
  bool m4a_warned;
  /* MP3: the first frame (Xing TOC) for seeking */
  mp3_info_t mp3;
  bool mp3_ok;
  /* FLAC, MP3, AAC, M4A: esp_audio_simple_dec (the file from its start,
   * eos at the end) */
  esp_audio_simple_dec_handle_t dec;
  uint8_t *dpcm;
  uint32_t dpcm_cap;
  uint32_t dpcm_pending; /* the probe's first output, not sent yet */
  bool dec_done;
  bool force_eos;     /* the decoder takes no more of the last bytes: eos with them */
  uint32_t eos_empty, eos_calls;
  uint64_t eos_frames;
  bool probing;       /* open_input: decoding only to learn the format */
  int pcm_bytes, pcm_shift; /* decoded sample layout, found on the first frame */
  /* CRC-32 (zlib) of the first 10 s, file channel order, int32 LE samples:
   * compared with Apple's decoder on the Mac (bit for bit) */
  uint32_t crc;
  uint64_t crc_frames;
  bool crc_logged;
  double crc_from_s;  /* where the 10 s start: 0, or the place of the last seek */
  uint8_t *buf;
  size_t buf_len, buf_pos, buf_cap;
  int64_t buf_abs;    /* the file's byte at buf[0] */
  uint64_t frames_read;
  bool eof;
  /* frame being filled: internal slots */
  int32_t pcm[8][PL_FRAME];
  uint32_t fill;
  /* lanes */
  lane_t lane[PL_LANES];
  uint32_t speakers_gen;
  /* the roles last seen, by speaker name: a speaker that changes its role
   * holds the old lane for the lead, so the player seeks to where it is */
  char spk_name[SURROUND_MAX_SPEAKERS][24];
  int8_t spk_role[SURROUND_MAX_SPEAKERS];
  uint8_t *out;
  uint8_t *pkt;
  uint8_t *wbuf;
  size_t wlen;
  /* stream */
  int rate;
  uint8_t key[32];
  uint64_t nonce;
  uint32_t seq, rtp, rtp0;
  int sock;
  bool session, playing, need_anchor;
  uint64_t clock_id;
  int64_t anchor_ns;
  uint32_t anchor_rtp;
  int64_t max_lead_ns;
  int64_t log_us;
  /* where the time goes, per status window */
  int64_t t_read_us, t_enc_us, t_send_us, t_dec_us, t_hr_us;
  int64_t t_put_wait_us, t_get_wait_us; /* the queue full (decoder task) / empty (player) */
  /* MP3 / AAC length, estimated from what the decoder took so far */
  uint64_t dec_bytes, dec_frames;
  size_t id3_bytes;
  /* the queue and gapless (phase 3d.3) */
  int in_ix, play_ix;     /* the track being read; the one the speakers play */
  track_info_t ti;        /* the input's */
  boundary_t bnd[PL_BOUNDARIES];
  int n_bnd;
  int sess_rate, sess_bits, sess_layout;
  uint8_t sess_present;
  uint64_t out_left;      /* frames of music left in the input (UINT64_MAX: all) */
  uint64_t trim_start, trim_total; /* encoder delay to drop; music frames (0 = all) */
  uint64_t track_frames;  /* frames of the input that went out (the log: gapless) */
  /* hi-res (phase 3e): 88.2/96 kHz by 2, 176.4/192 by 4; pcm[] then holds
   * the filters' 24-bit output at the stream's rate */
  decim_t hr;
  int hr_in_rate, hr_bits;
  /* the decoder task (phase 3e): input, decoders and push_pcm run there;
   * the queue rb carries their PCM to this task */
  MessageBufferHandle_t rb;
  uint8_t *w_msg, *r_msg;  /* a record to send (decoder task), one received (player) */
  TaskHandle_t w_task;
  SemaphoreHandle_t w_resume, w_exit;
  atomic_bool park_req, w_parked;
  bool w_alive, w_quit;
  volatile bool in_done;  /* the input is read to its end (or closed): the worker waits */
  int park_depth;
  int pcm_bits;           /* the depth of what pcm[] holds: the lanes' */
  bool hr_worker;         /* the filters run in the decoder task (not for ALAC: its decoder is heavy) */
  int32_t *w_pack;        /* the decoder task's filtered block, packed by slot */
  /* pause and seek (phase 3d) */
  bool paused, finishing;
  uint32_t paused_rtp;
  uint32_t burst_rtp;     /* the first second from here goes out at once */
  uint64_t skip_frames;   /* decoded frames to drop: a seek lands before its place */
  int64_t reseek_at_us;   /* a lane was added: seek to where the speakers are */
  flac_stream_t flac;
  bool flac_scanned;
  int64_t flac_audio_start;
  int64_t flac_walk_at;   /* the next metadata block header (0 = not started) */
  size_t flac_scan;       /* where the search for the frame's end goes on (from buf_pos) */
  flac_seekpt_t *seekpts;
  size_t n_seekpts;
  uint64_t read_bytes;
  uint32_t frames_sent;
} player_t;

static int64_t lead_ns(const player_t *p) {
  if (!p->playing) return PL_START_DELAY_NS;
  const int64_t sent_ns =
      (int64_t)(uint32_t)(p->rtp - p->anchor_rtp) * 1000000000LL / p->rate;
  return p->anchor_ns + sent_ns - now_ns();
}

static void anchor_now(player_t *p) {
  p->anchor_ns = now_ns() + PL_START_DELAY_NS;
  p->anchor_rtp = p->rtp;
  sound_engine_set_anchor_time(p->clock_id, (uint64_t)p->anchor_ns, p->anchor_rtp);
  sound_engine_set_playing(true);
  p->playing = true;
  p->need_anchor = false;
}

/* The roles of the speakers connected now: their lanes. */
static void lanes_refresh(player_t *p) {
  const uint32_t gen = gw_speakers_audio_generation();
  if (gen == p->speakers_gen) return;
  p->speakers_gen = gen;
  gw_audio_target_t t[SURROUND_MAX_SPEAKERS];
  gw_speakers_audio_targets(t);
  bool want[PL_LANES] = {false};
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    if (!t[i].name[0]) continue;
    const int role = t[i].channel;
    if (role >= -1 && role < 8) want[role + 1] = true;
    for (size_t j = 0; j < SURROUND_MAX_SPEAKERS; ++j) {
      if (strcmp(p->spk_name[j], t[i].name) != 0 || p->spk_role[j] == role) continue;
      ESP_LOGI(TAG, "speaker %s: role %s -> %s", t[i].name, surround_role_name(p->spk_role[j]),
               surround_role_name(role));
      if (p->playing) p->reseek_at_us = esp_timer_get_time() + PL_RESEEK_DELAY_US;
    }
  }
  for (size_t i = 0; i < SURROUND_MAX_SPEAKERS; ++i) {
    strlcpy(p->spk_name[i], t[i].name, sizeof(p->spk_name[i]));
    p->spk_role[i] = t[i].channel;
  }
  const bool mc = p->wav.layout >= 6;
  int kbps = 0;
  for (int k = 0; k < PL_LANES; ++k) {
    lane_t *l = &p->lane[k];
    const int role = k - 1;
    const surround_sig_t sig = surround_role_signal(role, p->wav.layout, p->wav.present);
    const bool on = want[k] && sig.kind != SURROUND_SIG_NONE;
    if (on && !l->on) {
      memset(l, 0, sizeof(*l));
      l->role = role;
      l->sig = sig;
      l->bits = sig.kind == SURROUND_SIG_CHANNEL ? p->pcm_bits : 24;
      const airplay_audio_format_t *f = surround_player_lane_format(p->rate, l->bits, mc);
      if (!f) continue;
      l->ssrc = f->ssrc;
      alac_lite_init(&l->enc, l->bits, PL_ENC_ORDER);
      l->on = true;
      /* it would be heard only after the lead: start again from where the
       * speakers are, once the speakers settle */
      if (p->playing) p->reseek_at_us = esp_timer_get_time() + PL_RESEEK_DELAY_US;
      ESP_LOGI(TAG, "lane %s (%s): %s", surround_role_name(role),
               sig.kind == SURROUND_SIG_CHANNEL ? "its channel"
               : sig.kind == SURROUND_SIG_HALF_LR ? "0.5 (L + R)" : "mean without LFE",
               f->name);
    } else if (!on && l->on) {
      l->on = false;
      ESP_LOGI(TAG, "lane %s: no speaker with this role now", surround_role_name(role));
    }
    if (l->on) kbps += (int)((int64_t)p->rate * l->bits * 3 / 4 / 1000);
  }
  p->max_lead_ns = kbps > PL_BUSY_KBPS ? PL_MAX_LEAD_BUSY_NS : PL_MAX_LEAD_NS;
  taskENTER_CRITICAL(&s_mux);
  for (int k = 0; k < PL_LANES; ++k) s_st.lane_bits[k] = p->lane[k].on ? (uint8_t)p->lane[k].bits : 0;
  taskEXIT_CRITICAL(&s_mux);
}

static void put_be32(uint8_t *b, uint32_t v) {
  b[0] = (uint8_t)(v >> 24);
  b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);
  b[3] = (uint8_t)v;
}

static bool send_all(int sock, const uint8_t *b, size_t n) {
  while (n > 0) {
    const int w = send(sock, b, n, 0);
    if (w > 0) {
      b += w;
      n -= (size_t)w;
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      if (atomic_load(&s_stop)) return false;
      continue;
    }
    ESP_LOGW(TAG, "relay connection lost (errno %d)", errno);
    return false;
  }
  return true;
}

/* The batch so far to the relay. */
static bool flush_wbuf(player_t *p) {
  if (!p->wlen) return true;
  const int64_t t1 = esp_timer_get_time();
  const bool ok = send_all(p->sock, p->wbuf, p->wlen);
  p->t_send_us += esp_timer_get_time() - t1;
  p->wlen = 0;
  return ok;
}

/* The lane's samples of the frame in p->pcm, sign-extended to its bits. */
static void lane_samples(const player_t *p, const lane_t *l, int32_t *x) {
  switch (l->sig.kind) {
    case SURROUND_SIG_CHANNEL:
      memcpy(x, p->pcm[l->sig.channel], sizeof(int32_t) * PL_FRAME);
      break;
    case SURROUND_SIG_HALF_LR: {
      const int32_t *a = p->pcm[0], *b = p->pcm[1];
      if (p->pcm_bits == 16) {
        for (size_t i = 0; i < PL_FRAME; ++i) x[i] = (a[i] + b[i]) * 128; /* exact */
      } else {
        for (size_t i = 0; i < PL_FRAME; ++i) x[i] = (a[i] + b[i]) >> 1;
      }
      break;
    }
    case SURROUND_SIG_MEAN_NO_LFE: {
      int n = 0;
      for (int c = 0; c < 8; ++c)
        if (c != SURROUND_ROLE_LFE && (p->wav.present & (1U << c))) n++;
      const int up = p->pcm_bits == 16 ? 256 : 1;
      for (size_t i = 0; i < PL_FRAME; ++i) {
        int64_t s = 0;
        for (int c = 0; c < 8; ++c)
          if (c != SURROUND_ROLE_LFE && (p->wav.present & (1U << c))) s += p->pcm[c][i];
        s *= up;
        x[i] = (int32_t)((s >= 0 ? s + n / 2 : s - n / 2) / (n ? n : 1));
      }
      break;
    }
    default:
      memset(x, 0, sizeof(int32_t) * PL_FRAME);
  }
}

/* One RTP frame: a sub-frame per lane, same seq and RTP time. */
static bool send_frame(player_t *p) {
  lanes_refresh(p);
  if (p->need_anchor) anchor_now(p);
  int32_t x[PL_FRAME];
  for (int k = 0; k < PL_LANES; ++k) {
    lane_t *l = &p->lane[k];
    if (!l->on) continue;
    const int64_t t0 = esp_timer_get_time();
    lane_samples(p, l, x);
    const size_t n = alac_lite_encode(&l->enc, x, PL_FRAME, p->out, PL_FRAME * 3U + 64U, NULL);
    p->t_enc_us += esp_timer_get_time() - t0;
    if (!n) continue;
    const size_t wire = 2U + 12U + n + PL_TAG_LEN + PL_NONCE_TAIL;
    if (p->wlen + wire > PL_WBUF && !flush_wbuf(p)) return false;
    uint8_t *b = p->wbuf + p->wlen;
    b[0] = (uint8_t)(wire >> 8);
    b[1] = (uint8_t)wire;
    uint8_t *h = b + 2;
    put_be32(h, 0x80000000U | ((uint32_t)(l->role + 2) << 24) | (p->seq & PL_SEQ_MASK));
    put_be32(h + 4, p->rtp);
    put_be32(h + 8, l->ssrc);
    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, &p->nonce, 8);
    p->nonce++;
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(h + 12, &clen, p->out, n, h + 4, 8, NULL, nonce,
                                              p->key);
    memcpy(h + 12 + clen, nonce + 4, PL_NONCE_TAIL);
    p->wlen += wire;
    l->bytes += (uint32_t)wire;
  }
  p->seq = (p->seq + 1U) & PL_SEQ_MASK;
  p->rtp += PL_FRAME;
  p->frames_sent++;
  /* the first second goes out at once: the speakers start sooner */
  if (p->wlen >= PL_WBUF_FLUSH || (uint32_t)(p->rtp - p->burst_rtp) < (uint32_t)p->rate)
    return flush_wbuf(p);
  return true;
}

/* Sets the speakers up as the radio does; the announced format is the
 * radio's stereo one for the rate (the lanes' SSRCs tell the speakers the
 * rest, per block). */
static bool session_open(player_t *p, int volume_db) {
  p->rate = p->hr.factor > 1 ? p->hr.out_rate : p->wav.rate;
  const int ann_bits = p->rate == 48000 ? 24 : 16;
  esp_fill_random(p->key, sizeof(p->key));
  p->seq = esp_random() & PL_SEQ_MASK;
  p->rtp = esp_random();
  p->rtp0 = p->rtp;
  p->burst_rtp = p->rtp;
  p->clock_id = player_clock_id();
  while (esp_timer_get_time() - s_owns_since_us < 1000000LL && !atomic_load(&s_stop))
    vTaskDelay(pdMS_TO_TICKS(50));

  sound_format_t fmt = {.sample_rate = p->rate, .channels = 2,
                        .bits_per_sample = ann_bits, .frame_size = PL_FRAME};
  strlcpy(fmt.codec, "ALAC", sizeof(fmt.codec));
  sound_cipher_t ciph = {.type = SOUND_CIPHER_CHACHA20_POLY1305, .key_len = sizeof(p->key)};
  memcpy(ciph.key, p->key, sizeof(p->key));
  sound_engine_set_stream_type(SOUND_STREAM_BUFFERED);
  sound_engine_set_format(&fmt);
  sound_engine_set_encryption(&ciph);
  memset(&ciph, 0, sizeof(ciph));
  if (sound_engine_start_buffered(0) != ESP_OK) {
    st_set(P_FAILED, "The relay did not start");
    return false;
  }
  p->session = true;
  p->sess_rate = p->rate;
  p->sess_bits = p->pcm_bits;
  p->sess_layout = p->wav.layout;
  p->sess_present = p->wav.present;
  sound_engine_set_playout_latency_samples(0);
  gw_relay_set_lanes(true);
  const uint16_t port = sound_engine_get_buffered_port();
  p->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
                          .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (p->sock < 0 || connect(p->sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
    st_set(P_FAILED, "No connection to the relay (errno %d)", errno);
    return false;
  }
  const int one = 1;
  setsockopt(p->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  const struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(p->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sound_engine_set_volume_q15(db_to_q15(volume_db));
  p->need_anchor = true;
  p->speakers_gen = gw_speakers_audio_generation() - 1U;
  lanes_refresh(p);
  ESP_LOGI(TAG, "stream to the speakers: %d Hz lanes, relay port %u, volume %d dB", p->rate,
           (unsigned)port, volume_db);
  return true;
}

static void session_close(player_t *p) {
  if (p->sock >= 0) {
    shutdown(p->sock, SHUT_RDWR);
    close(p->sock);
    p->sock = -1;
  }
  if (p->session) {
    sound_engine_stop();
    p->session = false;
  }
}

/* ---- the file ---- */

static int fill_buf(player_t *p) {
  if (p->buf_pos > 0 && p->buf_pos < p->buf_len)
    memmove(p->buf, p->buf + p->buf_pos, p->buf_len - p->buf_pos);
  p->buf_abs += (int64_t)p->buf_pos;
  p->buf_len -= p->buf_pos;
  p->buf_pos = 0;
  if (p->eof || p->buf_len >= PL_READ_CHUNK * 2U) return 0;
  const int64_t t0 = esp_timer_get_time();
  const int n = p->src.read(&p->src, p->buf + p->buf_len, PL_READ_CHUNK * 2U - p->buf_len);
  p->t_read_us += esp_timer_get_time() - t0;
  if (n > 0) {
    p->buf_len += (size_t)n;
    p->read_bytes += (uint64_t)n;
  }
  else p->eof = true;
  return n;
}

static const char *layout_name(int layout) {
  return layout == 1 ? "mono" : layout == 2 ? "stereo" : layout == 6 ? "5.1" : "7.1";
}

static const char *kind_name(const player_t *p) {
  switch (p->kind) {
    case KIND_FLAC: return "FLAC";
    case KIND_MP3: return "MP3";
    case KIND_AAC: return "AAC";
    case KIND_M4A:
      return strcmp(p->m4a_codec, "alac") == 0   ? "ALAC (M4A)"
             : strcmp(p->m4a_codec, ".mp3") == 0 ? "MP3 (M4A)"
                                                 : "AAC (M4A)";
    default: return "WAV";
  }
}

/* What the first bytes (after an ID3v2 tag) are. */
static int sniff_kind(const uint8_t *h, size_t n) {
  if (n >= 4 && memcmp(h, "fLaC", 4) == 0) return KIND_FLAC;
  if (n >= 8 && memcmp(h + 4, "ftyp", 4) == 0) return KIND_M4A;
  if (n >= 2 && h[0] == 0xFF && (h[1] & 0xF6) == 0xF0) return KIND_AAC; /* ADTS, layer 0 */
  if (n >= 2 && h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 && (h[1] & 0x06) != 0) return KIND_MP3;
  return KIND_WAV;
}

static bool decode_stream(player_t *p);
static bool goto_track(player_t *p, int k, double pos_s);
static void status_update(player_t *p);
static bool controls(player_t *p);
static bool decode_any(player_t *p);
static bool decoder_open(player_t *p);
static bool flac_walk(player_t *p, const uint8_t *buf, int64_t base, size_t have, bool may_read);
static bool flac_scan(player_t *p);
static bool decode_flac(player_t *p);
static bool m4a_need(player_t *p, uint32_t need);

/* Reads the start of the file: skips an ID3v2 tag, finds what it is and
 * its format. What was read after the header (WAV) or all of it (the
 * decoders want the file whole) waits in p->buf. MP3, AAC and M4A tell
 * their format only through the decoder's first output (the probe). */
static bool open_input(player_t *p) {
  uint8_t *hdr = heap_caps_malloc(PL_HEADER_MAX, MALLOC_CAP_SPIRAM);
  if (!hdr) return false;
  size_t have = 0;
  bool ok = false;
  while (have < 10) {
    const int n = p->src.read(&p->src, hdr + have, 4096);
    if (n <= 0) break;
    have += (size_t)n;
  }
  size_t skip = id3v2_size(hdr, have);
  p->id3_bytes = skip;
  if (skip) {
    ESP_LOGI(TAG, "ID3v2 tag of %u bytes skipped", (unsigned)skip);
    while (skip >= have && !atomic_load(&s_stop)) { /* the tag goes on past what we have */
      skip -= have;
      const int n = p->src.read(&p->src, hdr, skip > PL_HEADER_MAX ? PL_HEADER_MAX : (skip ? skip : 4096));
      if (n <= 0) { have = 0; break; }
      have = (size_t)n;
    }
    memmove(hdr, hdr + skip, have - skip);
    have -= skip;
  }
  esp_err_t err = ESP_ERR_INVALID_SIZE;
  p->kind = sniff_kind(hdr, have);
  while (!atomic_load(&s_stop)) {
    switch (p->kind) {
      case KIND_FLAC: err = flac_parse(hdr, have, &p->wav, &p->total_frames); break;
      case KIND_M4A:
        err = m4a_open(&p->src, (int64_t)p->id3_bytes, &p->m4a, p->wav.why, sizeof(p->wav.why));
        if (err == ESP_ERR_NO_MEM) err = ESP_ERR_NOT_SUPPORTED; /* why says it */
        break;
      case KIND_MP3:
      case KIND_AAC: err = ESP_OK; break; /* the decoder finds the format */
      default: err = wav_parse(hdr, have, &p->wav); break;
    }
    if (err != ESP_ERR_INVALID_SIZE || have >= PL_HEADER_MAX) break;
    const int n = p->src.read(&p->src, hdr + have,
                              PL_HEADER_MAX - have > 4096 ? 4096 : PL_HEADER_MAX - have);
    if (n <= 0) break;
    have += (size_t)n;
  }
  if (err != ESP_OK) {
    st_set(P_FAILED, "%s", err == ESP_ERR_NOT_SUPPORTED ? p->wav.why
                           : "Not a WAV, FLAC, MP3, AAC or M4A file");
    goto done;
  }
  if (p->kind == KIND_FLAC) /* the metadata: what the header read shows */
    (void)flac_walk(p, hdr, (int64_t)p->id3_bytes, have, false);
  if (p->kind == KIND_MP3) {
    p->mp3_ok = mp3_parse_first(hdr, have, &p->mp3);
    if (p->mp3_ok && p->mp3.xing_frames)
      ESP_LOGI(TAG, "MP3 Xing/Info tag: %" PRIu32 " frames, %" PRIu32 " bytes, TOC %s",
               p->mp3.xing_frames, p->mp3.xing_bytes,
               p->mp3.has_toc ? "yes" : "no");
  }
  /* WAV: from the data chunk on; the decoders: the file from its start */
  p->buf_len = have - p->wav.data_offset;
  memcpy(p->buf, hdr + p->wav.data_offset, p->buf_len);
  p->buf_pos = 0;
  p->buf_abs = (int64_t)p->id3_bytes + (int64_t)p->wav.data_offset;
  if (p->kind == KIND_M4A) {
    const m4a_track_t *t = &p->m4a;
    strlcpy(p->m4a_codec, t->codec == M4A_ALAC ? "alac" : t->codec == M4A_AAC ? "mp4a" : ".mp3",
            sizeof(p->m4a_codec));
    p->m4a_duration_s = (double)t->duration_ts / t->timescale;
    /* room for the largest sample, and its ADTS header */
    const size_t cap = (size_t)t->max_size + PL_READ_CHUNK * 2U + 64U;
    if (cap > p->buf_cap) {
      uint8_t *nb = heap_caps_realloc(p->buf, cap + 16U, MALLOC_CAP_SPIRAM);
      if (!nb) {
        st_set(P_FAILED, "Out of memory for an M4A sample of %u bytes", (unsigned)t->max_size);
        goto done;
      }
      p->buf = nb;
      p->buf_cap = cap;
    }
    if (t->codec == M4A_AAC && !(p->afr = heap_caps_malloc(t->max_size + 16U, MALLOC_CAP_SPIRAM))) {
      st_set(P_FAILED, "Out of memory");
      goto done;
    }
    m4a_iter_seek(t, &p->m4a_it, 0);
  }
  if (p->kind == KIND_FLAC) {
    /* the decoder takes the frames one by one: from where they start */
    if (!flac_scan(p)) {
      st_set(P_FAILED, "FLAC: the start of the audio frames is not found");
      goto done;
    }
    const size_t cap = (p->flac.max_frame ? (size_t)p->flac.max_frame * 2U : 262144U) + 16384U;
    if (cap > p->buf_cap) {
      uint8_t *nb = heap_caps_realloc(p->buf, cap + 16U, MALLOC_CAP_SPIRAM);
      if (!nb) {
        st_set(P_FAILED, "Out of memory");
        goto done;
      }
      p->buf = nb;
      p->buf_cap = cap;
    }
    const int64_t a0 = p->flac_audio_start;
    if (a0 >= p->buf_abs && a0 < p->buf_abs + (int64_t)p->buf_len) {
      p->buf_pos = (size_t)(a0 - p->buf_abs);
    } else if (p->src.seek && p->src.seek(&p->src, a0) == ESP_OK) {
      p->buf_len = p->buf_pos = 0;
      p->buf_abs = a0;
    } else {
      st_set(P_FAILED, "FLAC: the audio frames cannot be reached");
      goto done;
    }
  }
  double dur = 0;
  if (p->kind == KIND_WAV) {
    const size_t fb = (size_t)p->wav.channels * (size_t)p->wav.container_bits / 8U;
    if (p->wav.data_size && p->wav.data_size != 0xFFFFFFFFU && fb)
      p->total_frames = p->wav.data_size / fb;
  } else {
    static bool registered;
    if (!registered) {
      (void)esp_flac_dec_register();
      (void)esp_mp3_dec_register();
      (void)esp_aac_dec_register();
      (void)esp_alac_dec_register();
      (void)esp_audio_simple_dec_register_default();
      registered = true;
    }
    if (!decoder_open(p)) {
      st_set(P_FAILED, "No %s decoder", kind_name(p));
      goto done;
    }
    if (!p->dpcm) { /* kept from track to track */
      p->dpcm_cap = 4608U * 8U * 4U;
      p->dpcm = heap_caps_malloc(p->dpcm_cap, MALLOC_CAP_SPIRAM);
    }
    if (!p->dpcm) {
      st_set(P_FAILED, "Out of memory");
      goto done;
    }
    if (p->kind != KIND_FLAC) {
      /* the format comes with the decoder's first output, which waits in
       * dpcm until the stream is open */
      p->probing = true;
      int idle = 0;
      while (!p->pcm_bytes && !atomic_load(&s_stop)) {
        const size_t pos0 = p->buf_pos, len0 = p->buf_len;
        const int n = fill_buf(p);
        if (!decode_any(p)) goto done;
        if (p->pcm_bytes) break;
        if (p->dec_done || (p->eof && p->buf_pos >= p->buf_len)) {
          st_set(P_FAILED, "No audio found in the %s file", kind_name(p));
          goto done;
        }
        const bool moved = n > 0 || p->buf_pos != pos0 || p->buf_len != len0;
        if (!moved && ++idle > 50) {
          st_set(P_FAILED, "The %s decoder takes nothing of this file", kind_name(p));
          goto done;
        }
      }
      p->probing = false;
      if (!p->pcm_bytes) goto done;
    }
  }
  /* gapless (phase 3d.3): the encoder's delay at the start and its padding
   * at the end are not music */
  p->out_left = UINT64_MAX;
  p->trim_start = p->trim_total = 0;
  if (p->kind == KIND_MP3 && p->mp3_ok && p->mp3.xing_frames && p->mp3.rate == p->wav.rate) {
    const uint64_t all = (uint64_t)p->mp3.xing_frames * (uint64_t)p->mp3.spf;
    p->total_frames = all;
    if (p->mp3.lame_ok && all > (uint64_t)(p->mp3.enc_delay + p->mp3.enc_padding)) {
      /* esp_mp3_dec drops the tag's frame, the delay and the padding itself
       * when it reads the file from its start (seen on the board: exactly
       * the music's frames); the length still caps a track after a seek,
       * where the decoder does not see the tag */
      p->trim_start = 0;
      p->trim_total = all - (uint64_t)p->mp3.enc_delay - (uint64_t)p->mp3.enc_padding;
    }
  }
  if (p->kind == KIND_M4A && p->m4a.timescale) {
    const m4a_track_t *t = &p->m4a;
    const uint64_t r = (uint64_t)p->wav.rate;
    p->total_frames = t->duration_ts * r / t->timescale;
    if (t->valid_ts) {
      p->trim_start = t->priming_ts * r / t->timescale;
      p->trim_total = t->valid_ts * r / t->timescale;
    }
  }
  if (p->trim_total) {
    p->total_frames = p->trim_total;
    p->skip_frames = p->trim_start;
    p->out_left = p->trim_total;
    ESP_LOGI(TAG, "gapless: %llu frames of encoder delay dropped, %llu frames of music",
             (unsigned long long)p->trim_start, (unsigned long long)p->trim_total);
  }
  if (p->total_frames) dur = (double)p->total_frames / p->wav.rate;
  else if (p->kind == KIND_M4A) dur = p->m4a_duration_s;
  /* MP3 and AAC: status_update estimates it once a few seconds are decoded */
  /* hi-res: the filters for this rate, kept over a gapless change */
  {
    const int f = decim_factor(p->wav.rate);
    if (f != p->hr.factor || p->hr_in_rate != p->wav.rate || p->hr_bits != p->wav.bits ||
        p->hr.present != p->wav.present) {
      decim_free(&p->hr);
      if (!decim_init(&p->hr, p->wav.rate, p->wav.bits, p->wav.present)) {
        st_set(P_FAILED, "Out of memory for the hi-res filters");
        goto done;
      }
      p->hr_in_rate = p->wav.rate;
      p->hr_bits = p->wav.bits;
    }
    p->pcm_bits = p->hr.factor > 1 ? 24 : p->wav.bits;
    /* where the filters run: next to a light decoder (FLAC, WAV, MP3,
     * AAC), or in the player task next to the encoders (ALAC) */
    p->hr_worker = p->hr.factor > 1 &&
                   !(p->kind == KIND_M4A && p->m4a.codec == M4A_ALAC);
  }
  const char *kind = kind_name(p);
  track_info_t *ti = &p->ti;
  if (p->hr.factor > 1)
    snprintf(ti->format, sizeof(ti->format), "%s %d Hz / %d bit / %s, /%d to %d", kind,
             p->wav.rate, p->wav.bits, layout_name(p->wav.layout), p->hr.factor, p->hr.out_rate);
  else
    snprintf(ti->format, sizeof(ti->format), "%s %d Hz / %d bit / %s", kind, p->wav.rate,
             p->wav.bits, layout_name(p->wav.layout));
  ti->rate = p->wav.rate;
  ti->bits = p->wav.bits;
  ti->channels = p->wav.channels;
  ti->layout = p->wav.layout;
  ti->duration_s = dur;
  ESP_LOGI(TAG, "file: %s %d Hz, %d bit, %d channels (%s), %.1f s%s", kind, p->wav.rate,
           p->wav.bits, p->wav.channels, layout_name(p->wav.layout), dur,
           p->hr.factor > 1 ? " (hi-res: down to the stream's rate, 24-bit lanes)" : "");
  ok = true;
done:
  free(hdr);
  return ok;
}

/* ---- PCM into frames ---- */

static uint32_t crc32_update(uint32_t crc, const uint8_t *b, size_t n) {
  static uint32_t table[256];
  if (!table[1]) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  }
  crc = ~crc;
  while (n--) crc = table[(crc ^ *b++) & 0xFFU] ^ (crc >> 8);
  return ~crc;
}

/* n frames of interleaved PCM, bytes per sample 2/3/4 little-endian, a
 * 4-byte sample shifted right by shift (8 = left-justified 24 bit): into
 * the internal slots; every full 352-sample frame goes out. */
/* ---- the decoder task's side (phase 3e: the decoder runs on core 1) ----
 * The decoders give their PCM to push_pcm, which drops what a seek or the
 * encoder's delay says, and puts the rest in the queue to the player task
 * (pcm_out). A park request (a seek, another track) drops it instead: the
 * player throws the queue away then anyway. */

typedef struct {
  uint8_t type; /* REC_PCM, REC_END (the input's end), REC_ERR */
  uint8_t bytes, shift, ch;
  uint32_t n;   /* frames that follow (REC_PCM) */
} pcm_rec_t;
enum { REC_PCM = 1, REC_END, REC_ERR, REC_SLOTS };
/* REC_SLOTS: filtered frames (hi-res), int32, the present slots in order */
#define PL_PIPE_BYTES (512U * 1024U)
#define PL_PIPE_ITEM  8192U
/* at least one tick (FreeRTOS runs at 100 Hz on the Gateway) */
#define PL_TICKS(ms) (pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1)

/* One record into the queue; waits while it is full, unless the player
 * wants the decoder parked or stopped (then the record is dropped). */
static void pipe_put(player_t *p, const pcm_rec_t *h, const uint8_t *d, size_t len) {
  memcpy(p->w_msg, h, sizeof(*h));
  if (len) memcpy(p->w_msg + sizeof(*h), d, len);
  const int64_t t0 = esp_timer_get_time();
  for (;;) {
    if (atomic_load(&p->park_req) || p->w_quit || atomic_load(&s_stop)) break;
    if (xMessageBufferSend(p->rb, p->w_msg, sizeof(*h) + len, pdMS_TO_TICKS(20)) > 0) break;
  }
  p->t_put_wait_us += esp_timer_get_time() - t0;
}

static void pipe_mark(player_t *p, uint8_t type) {
  const pcm_rec_t h = {.type = type};
  pipe_put(p, &h, NULL, 0);
}

static inline int32_t sample_at(const uint8_t *d, int bytes, int shift) {
  if (bytes == 3) return (int32_t)(((uint32_t)d[0] << 8) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 24)) >> 8;
  if (bytes == 2) return (int16_t)(d[0] | (d[1] << 8));
  return (int32_t)((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24)) >> shift;
}

/* The CRC of the first 10 s from the start or a seek (a pass of its own,
 * only while it runs). */
static void pcm_crc(player_t *p, const uint8_t *d, size_t n, int bytes, int shift) {
  const int ch = p->wav.channels;
  const uint64_t crc_end = (uint64_t)p->wav.rate * 10U;
  if (p->crc_frames >= crc_end) return;
  const size_t k = crc_end - p->crc_frames < n ? (size_t)(crc_end - p->crc_frames) : n;
  for (size_t i = 0; i < k * (size_t)ch; ++i, d += bytes) {
    const int32_t v = sample_at(d, bytes, shift);
    const uint8_t le[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    p->crc = crc32_update(p->crc, le, 4);
  }
  p->crc_frames += k;
  if (p->crc_frames == crc_end && !p->crc_logged) {
    p->crc_logged = true;
    ESP_LOGI(TAG, "PCM CRC-32 of 10 s from %.3f s (%u ch, int32 LE, file order): 0x%08" PRIx32,
             p->crc_from_s, (unsigned)ch, p->crc);
    taskENTER_CRITICAL(&s_mux);
    s_st.pcm_crc10 = p->crc;
    taskEXIT_CRITICAL(&s_mux);
  }
}

static bool push_pcm(player_t *p, const uint8_t *d, size_t n, int bytes, int shift) {
  const int ch = p->wav.channels;
  p->dec_frames += n;
  if (p->skip_frames) { /* a seek landed before its place; an encoder's delay */
    const size_t k = p->skip_frames < n ? (size_t)p->skip_frames : n;
    d += k * (size_t)ch * (size_t)bytes;
    n -= k;
    p->skip_frames -= k;
  }
  if (p->out_left != UINT64_MAX) { /* past the music: the encoder's padding */
    if (n > p->out_left) n = (size_t)p->out_left;
    p->out_left -= n;
  }
  p->track_frames += n;
  pcm_crc(p, d, n, bytes, shift); /* here: core 0 has the time */
  const size_t fb = (size_t)ch * (size_t)bytes;
  if (p->hr_worker) { /* hi-res next to a light decoder: the filters here */
    decim_t *dm = &p->hr;
    const float scale = dm->in_scale;
    const uint8_t present = p->wav.present;
    int nsl = 0;
    for (int sl = 0; sl < DECIM_SLOTS; ++sl) nsl += (present >> sl) & 1;
    while (n) {
      const size_t blk = n < DECIM_CHUNK ? n : DECIM_CHUNK;
      const int64_t t0 = esp_timer_get_time();
      for (int c = 0; c < ch; ++c) {
        const int8_t sl = p->wav.slot[c];
        if (sl < 0) continue;
        float *x = dm->in[sl];
        const uint8_t *q = d + (size_t)c * (size_t)bytes;
        if (bytes == 3) {
          for (size_t f = 0; f < blk; ++f, q += fb)
            x[f] = (float)((int32_t)(((uint32_t)q[0] << 8) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 24)) >> 8);
        } else {
          for (size_t f = 0; f < blk; ++f, q += fb) x[f] = (float)sample_at(q, bytes, shift) * scale;
        }
      }
      const int m = decim_run(dm, (int)blk);
      int32_t *o = p->w_pack;
      for (int j = 0; j < m; ++j)
        for (int sl = 0; sl < DECIM_SLOTS; ++sl)
          if (present & (1U << sl)) *o++ = dm->out[sl][j];
      p->t_hr_us += esp_timer_get_time() - t0;
      if (m) {
        const pcm_rec_t h = {.type = REC_SLOTS, .ch = (uint8_t)nsl, .n = (uint32_t)m};
        pipe_put(p, &h, (const uint8_t *)p->w_pack, (size_t)m * (size_t)nsl * sizeof(int32_t));
      }
      d += blk * fb;
      n -= blk;
    }
    return true;
  }
  const size_t per = PL_PIPE_ITEM / fb;
  while (n) {
    const size_t k = n < per ? n : per;
    const pcm_rec_t h = {.type = REC_PCM, .bytes = (uint8_t)bytes, .shift = (uint8_t)shift,
                         .ch = (uint8_t)ch, .n = (uint32_t)k};
    pipe_put(p, &h, d, k * fb);
    d += k * fb;
    n -= k;
  }
  return true;
}

/* ---- the player task's side: PCM from the queue into frames ---- */

/* Frames the decoder task filtered already (hi-res): into the frames. */
static bool slots_in(player_t *p, const int32_t *v, size_t n) {
  const uint8_t present = p->wav.present;
  for (size_t f = 0; f < n; ++f) {
    for (int sl = 0; sl < DECIM_SLOTS; ++sl)
      if (present & (1U << sl)) p->pcm[sl][p->fill] = *v++;
    if (++p->fill == PL_FRAME) {
      p->fill = 0;
      if (!send_frame(p)) return false;
    }
  }
  return true;
}

static bool pcm_in(player_t *p, const uint8_t *d, size_t n, int bytes, int shift) {
  const int ch = p->wav.channels;
  const size_t fb = (size_t)ch * (size_t)bytes;
  int8_t slot[8];
  memcpy(slot, p->wav.slot, sizeof(slot));
  if (p->hr.factor <= 1) {
    for (size_t f = 0; f < n; ++f, d += fb) {
      for (int c = 0; c < ch; ++c)
        if (slot[c] >= 0) p->pcm[slot[c]][p->fill] = sample_at(d + (size_t)c * (size_t)bytes, bytes, shift);
      if (++p->fill == PL_FRAME) {
        p->fill = 0;
        if (!send_frame(p)) return false;
      }
    }
    return true;
  }
  /* hi-res: a block at a time, each channel into its float array in
   * internal RAM, then the filters */
  decim_t *dm = &p->hr;
  const float scale = dm->in_scale;
  while (n) {
    const size_t blk = n < DECIM_CHUNK ? n : DECIM_CHUNK;
    const int64_t t0 = esp_timer_get_time();
    for (int c = 0; c < ch; ++c) {
      if (slot[c] < 0) continue;
      float *x = dm->in[slot[c]];
      const uint8_t *q = d + (size_t)c * (size_t)bytes;
      if (bytes == 3) {
        for (size_t f = 0; f < blk; ++f, q += fb)
          x[f] = (float)((int32_t)(((uint32_t)q[0] << 8) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 24)) >> 8);
      } else {
        for (size_t f = 0; f < blk; ++f, q += fb) x[f] = (float)sample_at(q, bytes, shift) * scale;
      }
    }
    const int m = decim_run(dm, (int)blk);
    p->t_hr_us += esp_timer_get_time() - t0;
    for (int j = 0; j < m;) {
      int run = m - j;
      if (run > (int)(PL_FRAME - p->fill)) run = (int)(PL_FRAME - p->fill);
      for (int sl = 0; sl < DECIM_SLOTS; ++sl)
        if (p->wav.present & (1U << sl))
          memcpy(&p->pcm[sl][p->fill], &dm->out[sl][j], (size_t)run * sizeof(int32_t));
      p->fill += (uint32_t)run;
      j += run;
      if (p->fill == PL_FRAME) {
        p->fill = 0;
        if (!send_frame(p)) return false;
      }
    }
    d += blk * fb;
    n -= blk;
  }
  return true;
}

/* WAV: PCM bytes into frames; a full frame goes out. */
static bool take_pcm(player_t *p) {
  const size_t cb = (size_t)p->wav.container_bits / 8U;
  const size_t fb = (size_t)p->wav.channels * cb;
  const uint32_t data_end_frames =
      p->wav.data_size && p->wav.data_size != 0xFFFFFFFFU ? p->wav.data_size / (uint32_t)fb : 0;
  size_t n = (p->buf_len - p->buf_pos) / fb;
  if (data_end_frames) {
    const uint64_t left = p->frames_read < data_end_frames ? data_end_frames - p->frames_read : 0;
    if (n > left) n = (size_t)left;
    if (!left) {
      p->eof = true; /* chunks after the data chunk are not audio */
      p->buf_pos = p->buf_len;
      return true;
    }
  }
  if (!n) return true;
  const uint8_t *d = p->buf + p->buf_pos;
  p->buf_pos += n * fb;
  p->frames_read += n;
  return push_pcm(p, d, n, (int)cb, 8);
}

/* The decoder's sample layout from its first output: packed little-endian
 * samples of the file's depth (16 bit in 2 bytes, 24 bit in 3; seen on the
 * board 2026-11-18 for FLAC stereo, 5.1 and 7.1). A guess from the data
 * alone failed on files that start with silence. FLAC knows its format from
 * STREAMINFO; MP3, AAC and M4A get it here, from the decoder. */
static bool dec_layout(player_t *p, const esp_audio_simple_dec_info_t *info, uint32_t size) {
  wav_info_t *w = &p->wav;
  const char *kind = kind_name(p);
  if (p->kind == KIND_FLAC) {
    if (info->sample_rate != (uint32_t)w->rate || info->channel != w->channels) {
      st_set(P_FAILED, "FLAC decoder gives %" PRIu32 " Hz / %u ch, the file says %d / %d",
             info->sample_rate, (unsigned)info->channel, w->rate, w->channels);
      return false;
    }
  } else {
    w->rate = (int)info->sample_rate;
    w->channels = info->channel;
    w->bits = info->bits_per_sample;
    w->container_bits = 0;
    memset(w->slot, -1, sizeof(w->slot));
    w->present = 0;
    if (!decim_factor(w->rate)) {
      st_set(P_FAILED, "%s at %d Hz (44.1/48 kHz and their 2x and 4x)", kind, w->rate);
      return false;
    }
    if (w->bits != 16 && w->bits != 24) {
      st_set(P_FAILED, "%s gives %d-bit samples (16 and 24 only)", kind, w->bits);
      return false;
    }
    /* ALAC's own channel order (Apple's ALAC channel layouts): 5.1 = C L R
     * Ls Rs LFE; 8 channels = C Lc Rc L R Ls Rs LFE, where Lc/Rc go to the
     * rear pair (Lrs/Rrs): Apple's afconvert leaves 7.1's rears out of ALAC
     * altogether, so an 8-channel ALAC file comes from other tools that put
     * them there (ffmpeg's 7.1 wide). */
    static const int8_t alac6[6] = {2, 0, 1, 4, 5, 3};
    static const int8_t alac8[8] = {2, 6, 7, 0, 1, 4, 5, 3};
    const bool alac = p->kind == KIND_M4A && strcmp(p->m4a_codec, "alac") == 0;
    switch (w->channels) {
      case 1: w->slot[0] = 0; w->layout = 1; break;
      case 2: w->slot[0] = 0; w->slot[1] = 1; w->layout = 2; break;
      case 6:
      case 8:
        if (alac) {
          memcpy(w->slot, w->channels == 6 ? alac6 : alac8, (size_t)w->channels);
          w->layout = w->channels;
          break;
        }
        st_set(P_FAILED, "%d-channel %s: not yet (mono and stereo; 5.1 and 7.1 as WAV, FLAC or ALAC)",
               w->channels, kind);
        return false;
      default:
        st_set(P_FAILED, "%s with %d channels (mono, stereo, 5.1, 7.1)", kind, w->channels);
        return false;
    }
    for (int c = 0; c < w->channels; ++c)
      if (w->slot[c] >= 0) w->present |= (uint8_t)(1U << w->slot[c]);
  }
  p->pcm_bytes = w->bits / 8;
  p->pcm_shift = 0;
  if (info->bits_per_sample != w->bits || size % (uint32_t)(w->channels * p->pcm_bytes) != 0) {
    st_set(P_FAILED, "%s decoder output not as expected: %u bit, %" PRIu32 " bytes", kind,
           (unsigned)info->bits_per_sample, size);
    return false;
  }
  ESP_LOGI(TAG, "%s decoder output: %" PRIu32 " Hz, %u ch, %u bit, %d bytes per sample "
           "(first block %" PRIu32 " B)", kind, info->sample_rate, (unsigned)info->channel,
           (unsigned)info->bits_per_sample, p->pcm_bytes, size);
  return true;
}

/* FLAC, MP3, AAC, M4A: what is in p->buf through the decoder; PCM goes on
 * to push_pcm. While probing it stops at the first output, which waits in
 * dpcm (dpcm_pending) and goes out first on the next call. */
static bool decode_stream(player_t *p) {
  if (p->dpcm_pending && !p->probing) {
    const size_t fb = (size_t)p->wav.channels * (size_t)p->pcm_bytes;
    const uint32_t n = p->dpcm_pending;
    p->dpcm_pending = 0;
    if (!push_pcm(p, p->dpcm, n / fb, p->pcm_bytes, p->pcm_shift)) return false;
  }
  while (!atomic_load(&s_stop)) {
    /* eos only once every byte went in: with bytes still to come the FLAC
     * parser takes eos as the last chunk and drops the rest */
    esp_audio_simple_dec_raw_t raw = {.buffer = p->buf + p->buf_pos,
                                      .len = (uint32_t)(p->buf_len - p->buf_pos),
                                      .eos = p->eof && (p->buf_pos >= p->buf_len || p->force_eos)};
    if (!raw.len && !p->eof) return true; /* more of the file first */
    esp_audio_simple_dec_out_t out = {.buffer = p->dpcm, .len = p->dpcm_cap};
    const int64_t t0 = esp_timer_get_time();
    const esp_audio_err_t r = esp_audio_simple_dec_process(p->dec, &raw, &out);
    p->t_dec_us += esp_timer_get_time() - t0;
    if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      uint8_t *n = heap_caps_realloc(p->dpcm, out.needed_size, MALLOC_CAP_SPIRAM);
      if (!n) {
        st_set(P_FAILED, "Out of memory");
        return false;
      }
      p->dpcm = n;
      p->dpcm_cap = out.needed_size;
      continue;
    }
    p->buf_pos += raw.consumed;
    p->dec_bytes += raw.consumed;
    if (out.decoded_size > 0) {
      if (!p->pcm_bytes) {
        esp_audio_simple_dec_info_t info = {0};
        esp_audio_simple_dec_get_info(p->dec, &info);
        if (!dec_layout(p, &info, out.decoded_size)) return false;
      }
      const size_t fb = (size_t)p->wav.channels * (size_t)p->pcm_bytes;
      if (out.decoded_size % fb) {
        st_set(P_FAILED, "%s decoder output of %" PRIu32 " bytes", kind_name(p), out.decoded_size);
        return false;
      }
      if (p->probing) {
        p->dpcm_pending = out.decoded_size;
        return true;
      }
      if (!push_pcm(p, p->dpcm, out.decoded_size / fb, p->pcm_bytes, p->pcm_shift)) return false;
    }
    if (raw.consumed == 0 && out.decoded_size == 0 && p->eof && !raw.eos) {
      p->force_eos = true; /* it waits for more: there is none */
      continue;
    }
    if (raw.eos) {
      /* the end: the decoder gives what it holds a frame at a time, with
       * empty answers between; done after several empty ones in a row */
      if (out.decoded_size) {
        p->eos_frames += out.decoded_size / ((size_t)p->wav.channels * (size_t)p->pcm_bytes);
        p->eos_empty = 0;
      } else if (++p->eos_empty >= 16) {
        ESP_LOGI(TAG, "%s decoder emptied at the end: %llu frames after eos (last result %d)",
                 kind_name(p), (unsigned long long)p->eos_frames, (int)r);
        p->dec_done = true;
        return true;
      }
      p->eos_calls++;
      if (p->eos_calls > 100000) {
        p->dec_done = true;
        return true;
      }
      continue;
    }
    if (r != ESP_AUDIO_ERR_OK && r != ESP_AUDIO_ERR_DATA_LACK && raw.consumed == 0 &&
        out.decoded_size == 0) {
      if (p->eof) {
        p->dec_done = true;
        return true;
      }
      ESP_LOGW(TAG, "%s decode error %d", kind_name(p), (int)r);
      return true; /* more data may help */
    }
    if (raw.consumed == 0 && out.decoded_size == 0) {
      if (p->eof) p->dec_done = true;
      return true;
    }
  }
  return true;
}

/* The decoder for the file's kind: esp_audio_simple_dec for FLAC, MP3 and
 * AAC (ADTS), esp_audio_dec fed one M4A sample at a time. A seek opens it
 * again (or resets it). */
static bool decoder_open(player_t *p) {
  if (p->dec) esp_audio_simple_dec_close(p->dec);
  p->dec = NULL;
  if (p->adec) esp_audio_dec_close(p->adec);
  p->adec = NULL;
  if (p->kind == KIND_FLAC) {
    esp_audio_dec_cfg_t c = {.type = ESP_AUDIO_TYPE_FLAC};
    if (esp_audio_dec_open(&c, &p->adec) != ESP_AUDIO_ERR_OK) return false;
    /* fLaC and STREAMINFO first, as a file starts */
    uint8_t h[42] = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    memcpy(h + 8, p->flac.streaminfo, 34);
    uint8_t pcm[64];
    esp_audio_dec_in_raw_t raw = {.buffer = h, .len = sizeof(h)};
    esp_audio_dec_out_frame_t out = {.buffer = pcm, .len = sizeof(pcm)};
    const esp_audio_err_t r = esp_audio_dec_process(p->adec, &raw, &out);
    ESP_LOGD(TAG, "FLAC header to the decoder: %d, %u taken", (int)r, (unsigned)raw.consumed);
    return true;
  }
  if (p->kind == KIND_M4A) {
    esp_audio_dec_cfg_t c = {0};
    esp_alac_dec_cfg_t alac = ESP_ALAC_DEC_CONFIG_DEFAULT();
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    if (p->m4a.codec == M4A_ALAC) {
      alac.codec_spec_info = p->m4a.config;
      alac.spec_info_len = (uint32_t)p->m4a.config_len;
      c = (esp_audio_dec_cfg_t){.type = ESP_AUDIO_TYPE_ALAC, .cfg = &alac, .cfg_sz = sizeof(alac)};
    } else if (p->m4a.codec == M4A_AAC) {
      aac.aac_plus_enable = true; /* HE-AAC at its full rate */
      c = (esp_audio_dec_cfg_t){.type = ESP_AUDIO_TYPE_AAC, .cfg = &aac, .cfg_sz = sizeof(aac)};
    } else {
      c = (esp_audio_dec_cfg_t){.type = ESP_AUDIO_TYPE_MP3};
    }
    return esp_audio_dec_open(&c, &p->adec) == ESP_AUDIO_ERR_OK;
  }
  esp_audio_simple_dec_cfg_t dc = {
      .dec_type = p->kind == KIND_FLAC ? ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC
                : p->kind == KIND_MP3  ? ESP_AUDIO_SIMPLE_DEC_TYPE_MP3
                                       : ESP_AUDIO_SIMPLE_DEC_TYPE_AAC};
  esp_aac_dec_cfg_t ac = ESP_AAC_DEC_CONFIG_DEFAULT();
  ac.aac_plus_enable = true;
  if (p->kind == KIND_AAC) {
    dc.dec_cfg = &ac;
    dc.cfg_size = sizeof(ac);
  }
  return esp_audio_simple_dec_open(&dc, &p->dec) == ESP_AUDIO_ERR_OK;
}

/* At least need bytes from buf_pos on in buf (an M4A sample), read from
 * the file as they come. False at the file's end. */
static bool m4a_need(player_t *p, uint32_t need) {
  while (p->buf_len - p->buf_pos < need) {
    if (p->buf_pos) {
      memmove(p->buf, p->buf + p->buf_pos, p->buf_len - p->buf_pos);
      p->buf_abs += (int64_t)p->buf_pos;
      p->buf_len -= p->buf_pos;
      p->buf_pos = 0;
    }
    if (p->eof || atomic_load(&s_stop)) return false;
    size_t room = p->buf_cap - p->buf_len;
    if (room > 32768U) room = 32768U;
    const int64_t t0 = esp_timer_get_time();
    const int n = p->src.read(&p->src, p->buf + p->buf_len, room);
    p->t_read_us += esp_timer_get_time() - t0;
    if (n <= 0) {
      p->eof = true;
      return false;
    }
    p->buf_len += (size_t)n;
    p->read_bytes += (uint64_t)n;
  }
  return true;
}

/* M4A: the next few samples through the decoder; PCM on to push_pcm. A
 * sample elsewhere in the file (another track in between) is sought. */
static bool decode_m4a(player_t *p) {
  if (p->dpcm_pending && !p->probing) {
    const size_t fb = (size_t)p->wav.channels * (size_t)p->pcm_bytes;
    const uint32_t n = p->dpcm_pending;
    p->dpcm_pending = 0;
    if (!push_pcm(p, p->dpcm, n / fb, p->pcm_bytes, p->pcm_shift)) return false;
  }
  for (int k = 0; k < 8 && !atomic_load(&s_stop); ++k) {
    uint64_t off;
    uint32_t size;
    if (!m4a_iter_get(&p->m4a, &p->m4a_it, &off, &size)) {
      p->eof = true;
      p->dec_done = true;
      return true;
    }
    const int64_t here = p->buf_abs + (int64_t)p->buf_pos;
    if ((int64_t)off != here) {
      if ((int64_t)off > here && (int64_t)off <= p->buf_abs + (int64_t)p->buf_len) {
        p->buf_pos = (size_t)((int64_t)off - p->buf_abs);
      } else {
        const esp_err_t err = p->src.seek ? p->src.seek(&p->src, (int64_t)off) : ESP_FAIL;
        if (err != ESP_OK) {
          st_set(P_FAILED, "The M4A audio at byte %llu cannot be read", (unsigned long long)off);
          return false;
        }
        p->buf_len = p->buf_pos = 0;
        p->buf_abs = (int64_t)off;
        p->eof = false;
      }
    }
    if (!m4a_need(p, size)) {
      if (p->eof) p->dec_done = true; /* the file ends before its index says */
      return true;
    }
    uint8_t *frame = p->buf + p->buf_pos;
    uint32_t flen = size;
    if (p->m4a.codec == M4A_AAC) {
      m4a_adts_header(&p->m4a, size, p->afr);
      memcpy(p->afr + 7, frame, size);
      frame = p->afr;
      flen = size + 7U;
    }
    esp_audio_dec_in_raw_t raw = {.buffer = frame, .len = flen};
    esp_audio_dec_out_frame_t out = {.buffer = p->dpcm, .len = p->dpcm_cap};
    const int64_t t0 = esp_timer_get_time();
    const esp_audio_err_t r = esp_audio_dec_process(p->adec, &raw, &out);
    p->t_dec_us += esp_timer_get_time() - t0;
    if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > p->dpcm_cap) {
      uint8_t *n = heap_caps_realloc(p->dpcm, out.needed_size, MALLOC_CAP_SPIRAM);
      if (!n) {
        st_set(P_FAILED, "Out of memory");
        return false;
      }
      p->dpcm = n;
      p->dpcm_cap = out.needed_size;
      --k;
      continue; /* the same sample again */
    }
    /* the sample is done with, decoded or not */
    p->buf_pos += size;
    p->dec_bytes += size;
    m4a_iter_next(&p->m4a, &p->m4a_it);
    if (r != ESP_AUDIO_ERR_OK) {
      if (!p->m4a_warned) ESP_LOGW(TAG, "M4A sample %u: decode error %d", (unsigned)p->m4a_it.sample - 1U, (int)r);
      p->m4a_warned = true;
      continue;
    }
    if (!out.decoded_size) continue;
    if (!p->pcm_bytes) {
      esp_audio_dec_info_t di = {0};
      esp_audio_dec_get_info(p->adec, &di);
      const esp_audio_simple_dec_info_t info = {.sample_rate = di.sample_rate,
                                                .bits_per_sample = di.bits_per_sample,
                                                .channel = di.channel,
                                                .bitrate = di.bitrate,
                                                .frame_size = di.frame_size};
      if (!dec_layout(p, &info, out.decoded_size)) return false;
    }
    const size_t fb = (size_t)p->wav.channels * (size_t)p->pcm_bytes;
    if (out.decoded_size % fb) {
      st_set(P_FAILED, "%s decoder output of %" PRIu32 " bytes", kind_name(p), out.decoded_size);
      return false;
    }
    if (p->probing) {
      p->dpcm_pending = out.decoded_size;
      return true;
    }
    if (!push_pcm(p, p->dpcm, out.decoded_size / fb, p->pcm_bytes, p->pcm_shift)) return false;
  }
  return true;
}

/* FLAC: one whole frame at a time to esp_audio_dec. The simple decoder's
 * parser lost frames of a few bytes (digital silence: 16 of 68 in a test
 * file, 2026-11-18), so the player finds the frames itself: a frame ends
 * where the next frame header starts and the CRC-16 before it is right. */
static bool decode_flac(player_t *p) {
  const int ch = p->wav.channels, bits = p->wav.bits;
  for (int k = 0; k < 8 && !atomic_load(&s_stop); ++k) {
    size_t avail = p->buf_len - p->buf_pos;
    const uint8_t *b = p->buf + p->buf_pos;
    uint64_t first;
    if (avail >= 16 && !flac_frame_at(b, avail, &p->flac, ch, bits, &first)) {
      size_t i = 1; /* not at a frame: on to the next one */
      while (i + 16 <= avail && !(b[i] == 0xFF && flac_frame_at(b + i, avail - i, &p->flac, ch, bits, &first)))
        ++i;
      if (!p->m4a_warned) ESP_LOGW(TAG, "FLAC: %u bytes before a frame skipped", (unsigned)i);
      p->m4a_warned = true;
      p->buf_pos += i;
      p->flac_scan = 0;
      continue;
    }
    size_t end = 0;
    for (size_t q = p->flac_scan > 6 ? p->flac_scan : 6; q + 16 <= avail; ++q) {
      if (b[q] == 0xFF && (b[q + 1] & 0xFE) == 0xF8 &&
          flac_frame_at(b + q, avail - q, &p->flac, ch, bits, &first) && flac_frame_crc_ok(b, q)) {
        end = q;
        break;
      }
    }
    if (!end) {
      if (p->eof) {
        if (avail >= 6 && flac_frame_crc_ok(b, avail)) {
          end = avail; /* the last frame */
        } else {
          if (avail) ESP_LOGW(TAG, "FLAC: %u bytes at the end are not a whole frame", (unsigned)avail);
          p->buf_pos = p->buf_len;
          p->dec_done = true;
          return true;
        }
      } else {
        p->flac_scan = avail > 16 ? avail - 15 : 6; /* look on from there next time */
        if (avail + 8192U > p->buf_cap) { /* a frame larger than the room */
          const size_t cap = p->buf_cap * 2U;
          uint8_t *nb = cap <= 4U * 1024U * 1024U ? heap_caps_realloc(p->buf, cap + 16U, MALLOC_CAP_SPIRAM) : NULL;
          if (!nb) {
            st_set(P_FAILED, "A FLAC frame larger than %u bytes", (unsigned)p->buf_cap);
            return false;
          }
          p->buf = nb;
          p->buf_cap = cap;
        }
        (void)m4a_need(p, (uint32_t)(avail + 8192U));
        if (p->buf_len - p->buf_pos <= avail && !p->eof) return true; /* nothing came */
        continue;
      }
    }
    esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)b, .len = (uint32_t)end};
    esp_audio_dec_out_frame_t out = {.buffer = p->dpcm, .len = p->dpcm_cap};
    const int64_t t0 = esp_timer_get_time();
    esp_audio_err_t r = esp_audio_dec_process(p->adec, &raw, &out);
    if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > p->dpcm_cap) {
      uint8_t *n = heap_caps_realloc(p->dpcm, out.needed_size, MALLOC_CAP_SPIRAM);
      if (!n) {
        st_set(P_FAILED, "Out of memory");
        return false;
      }
      p->dpcm = n;
      p->dpcm_cap = out.needed_size;
      out = (esp_audio_dec_out_frame_t){.buffer = p->dpcm, .len = p->dpcm_cap};
      raw.consumed = 0;
      r = esp_audio_dec_process(p->adec, &raw, &out);
    }
    p->t_dec_us += esp_timer_get_time() - t0;
    p->buf_pos += end;
    p->flac_scan = 0;
    p->dec_bytes += end;
    if (r != ESP_AUDIO_ERR_OK) {
      if (!p->m4a_warned) ESP_LOGW(TAG, "FLAC frame of %u bytes: decode error %d", (unsigned)end, (int)r);
      p->m4a_warned = true;
      continue;
    }
    if (!out.decoded_size) continue;
    if (!p->pcm_bytes) {
      esp_audio_dec_info_t di = {0};
      esp_audio_dec_get_info(p->adec, &di);
      const esp_audio_simple_dec_info_t info = {.sample_rate = di.sample_rate,
                                                .bits_per_sample = di.bits_per_sample,
                                                .channel = di.channel};
      if (!dec_layout(p, &info, out.decoded_size)) return false;
    }
    const size_t fb = (size_t)ch * (size_t)p->pcm_bytes;
    if (out.decoded_size % fb) {
      st_set(P_FAILED, "FLAC decoder output of %" PRIu32 " bytes", out.decoded_size);
      return false;
    }
    if (!push_pcm(p, p->dpcm, out.decoded_size / fb, p->pcm_bytes, p->pcm_shift)) return false;
  }
  return true;
}

static bool decode_any(player_t *p) {
  return p->kind == KIND_M4A ? decode_m4a(p) : p->kind == KIND_FLAC ? decode_flac(p)
                                                                     : decode_stream(p);
}

/* ---- the decoder task (phase 3e) ----
 * On core 1, below the relay and lwIP: it reads the file and decodes it,
 * the player task (core 0) filters, encodes and sends. The player parks it
 * at a safe point (between two decoder calls) before it touches the input
 * itself: a seek, another track, the next track at the end. */

#define PL_WORKER_STACK    12288
#define PL_WORKER_PRIORITY 3 /* core 0 has little else; the radio is off while the player plays */
#define PL_WORKER_CORE     0 /* alone there: reading and decoding are the heaviest part */

static void worker_loop(player_t *p) {
  int64_t rest_us = esp_timer_get_time();
  for (;;) {
    if (p->w_quit || atomic_load(&s_stop)) return;
    /* a heavy file keeps it busy for long: one tick a second for core 0's
     * idle task (the task watchdog) and the tasks below it */
    if (esp_timer_get_time() - rest_us > 1000000) {
      vTaskDelay(1);
      rest_us = esp_timer_get_time();
    }
    if (atomic_load(&p->park_req)) {
      atomic_store(&p->w_parked, true);
      xSemaphoreTake(p->w_resume, portMAX_DELAY);
      atomic_store(&p->w_parked, false);
      continue;
    }
    if (p->in_done) { /* the end is in the queue: the player goes on from there */
      vTaskDelay(PL_TICKS(5));
      continue;
    }
    fill_buf(p);
    const bool ok = p->kind == KIND_WAV ? take_pcm(p) : decode_any(p);
    if (atomic_load(&p->park_req)) continue; /* what it made went nowhere */
    if (!ok) {
      pipe_mark(p, REC_ERR);
      p->in_done = true;
      continue;
    }
    const bool drained = p->kind != KIND_WAV
        ? p->dec_done && !p->dpcm_pending
        : p->buf_len - p->buf_pos < (size_t)p->wav.channels * p->wav.container_bits / 8U;
    if (p->eof && drained) {
      pipe_mark(p, REC_END);
      p->in_done = true;
    }
  }
}

static void worker_task(void *arg) {
  player_t *p = arg;
  worker_loop(p);
  xSemaphoreGive(p->w_exit);
  vTaskDeleteWithCaps(NULL);
}

static bool worker_start(player_t *p) {
  p->rb = xMessageBufferCreateWithCaps(PL_PIPE_BYTES, MALLOC_CAP_SPIRAM);
  p->w_msg = heap_caps_malloc(PL_PIPE_ITEM + sizeof(pcm_rec_t), MALLOC_CAP_SPIRAM);
  p->w_pack = heap_caps_malloc((DECIM_CHUNK / 2 + 1) * DECIM_SLOTS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
  /* the player reads every sample of it: internal RAM when there is room */
  p->r_msg = heap_caps_malloc(PL_PIPE_ITEM + sizeof(pcm_rec_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p->r_msg) p->r_msg = heap_caps_malloc(PL_PIPE_ITEM + sizeof(pcm_rec_t), MALLOC_CAP_SPIRAM);
  p->w_resume = xSemaphoreCreateBinary();
  p->w_exit = xSemaphoreCreateBinary();
  if (!p->rb || !p->w_msg || !p->r_msg || !p->w_pack || !p->w_resume || !p->w_exit) return false;
  if (xTaskCreatePinnedToCoreWithCaps(worker_task, "gw_player_dec", PL_WORKER_STACK, p,
                                      PL_WORKER_PRIORITY, &p->w_task, PL_WORKER_CORE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    return false;
  p->w_alive = true;
  return true;
}

static void worker_stop(player_t *p) {
  if (p->w_alive) {
    p->w_quit = true;
    xSemaphoreGive(p->w_resume); /* in case it is parked */
    bool gone = false;
    for (int i = 0; i < 100 && !(gone = xSemaphoreTake(p->w_exit, pdMS_TO_TICKS(100)) == pdTRUE); ++i)
      (void)xMessageBufferReceive(p->rb, p->r_msg, PL_PIPE_ITEM + sizeof(pcm_rec_t), 0);
    if (!gone) ESP_LOGE(TAG, "the decoder task did not stop in 10 s");
    p->w_alive = false;
  }
  if (p->rb) vMessageBufferDeleteWithCaps(p->rb);
  free(p->w_msg);
  free(p->r_msg);
  free(p->w_pack);
  p->w_msg = p->r_msg = NULL;
  p->w_pack = NULL;
  if (p->w_resume) vSemaphoreDelete(p->w_resume);
  if (p->w_exit) vSemaphoreDelete(p->w_exit);
  p->rb = NULL;
  p->w_resume = p->w_exit = NULL;
}

/* The decoder task at a safe point, and what it made thrown away: the
 * input is the player's until pipe_resume. Nested calls count. */
static void pipe_park(player_t *p) {
  if (p->park_depth++ > 0 || !p->w_alive) return;
  atomic_store(&p->park_req, true);
  const size_t cap = PL_PIPE_ITEM + sizeof(pcm_rec_t);
  while (!atomic_load(&p->w_parked)) (void)xMessageBufferReceive(p->rb, p->r_msg, cap, PL_TICKS(5));
  while (xMessageBufferReceive(p->rb, p->r_msg, cap, 0) > 0) {
  }
}

static void pipe_resume(player_t *p) {
  if (p->park_depth > 0 && --p->park_depth > 0) return;
  if (!p->w_alive) return;
  atomic_store(&p->park_req, false);
  if (atomic_load(&p->w_parked)) xSemaphoreGive(p->w_resume);
}

/* ---- pause and seek (phase 3d) ---- */

/* The RTP time the speakers play now (the next frame to send while the
 * stream is not anchored). */
static uint32_t play_rtp(const player_t *p) {
  if (p->paused) return p->paused_rtp;
  if (!p->playing) return p->rtp;
  int64_t d = now_ns() - p->anchor_ns;
  if (d < 0) d = 0;
  uint64_t played = (uint64_t)d * (uint64_t)p->rate / 1000000000ULL;
  const uint32_t sent = p->rtp - p->anchor_rtp;
  if (played > sent) played = sent;
  return p->anchor_rtp + (uint32_t)played;
}

static double play_pos_s(const player_t *p) {
  return (double)(uint32_t)(play_rtp(p) - p->rtp0) / p->rate;
}

/* Pause as Apple's buffered senders do (SETRATEANCHORTIME rate 0): the
 * speakers keep what they hold, and resume plays on from the same RTP time
 * under a new anchor. */
static void do_pause(player_t *p) {
  if (p->paused) return;
  p->paused_rtp = play_rtp(p);
  (void)flush_wbuf(p);
  sound_engine_pause();
  p->playing = false;
  p->paused = true;
  ESP_LOGI(TAG, "paused at %.1f s", play_pos_s(p));
  st_set(P_PAUSED, NULL);
}

static void do_resume(player_t *p) {
  if (!p->paused) return;
  p->paused = false;
  if (!p->need_anchor) {
    p->anchor_ns = now_ns() + PL_RESUME_DELAY_NS;
    p->anchor_rtp = p->paused_rtp;
    sound_engine_set_anchor_time(p->clock_id, (uint64_t)p->anchor_ns, p->anchor_rtp);
    sound_engine_set_playing(true);
    p->playing = true;
  } /* else not anchored yet: the next frame anchors */
  ESP_LOGI(TAG, "resumed at %.1f s", play_pos_s(p));
  st_set(p->finishing ? P_FINISHING : P_PLAYING, NULL);
}

/* Bytes of the file: from buf when they are in it (buf holds the file from
 * byte base on), else read_at (when may_read). */
static bool file_bytes(player_t *p, const uint8_t *buf, int64_t base, size_t have, int64_t off,
                       uint8_t *dst, size_t n, bool may_read) {
  if (buf && off >= base && off + (int64_t)n <= base + (int64_t)have) {
    memcpy(dst, buf + (off - base), n);
    return true;
  }
  return may_read && p->src.read_at && p->src.read_at(&p->src, off, dst, n) == (int)n;
}

/* FLAC: STREAMINFO, the seek table and where the frames start. open_input
 * walks what it read of the file (may_read false: it stops where that
 * ends); the first seek goes on from there with read_at. */
static bool flac_walk(player_t *p, const uint8_t *buf, int64_t base, size_t have, bool may_read) {
  if (p->flac_scanned) return p->flac_audio_start > 0;
  if (!p->flac_walk_at) {
    uint8_t h[42];
    if (!file_bytes(p, buf, base, have, (int64_t)p->id3_bytes, h, sizeof(h), may_read))
      return false;
    if (memcmp(h, "fLaC", 4) != 0) {
      p->flac_scanned = true;
      return false;
    }
    flac_stream_info(h, &p->flac);
    p->flac_walk_at = (int64_t)p->id3_bytes + 4;
  }
  for (int k = 0; k < 128; ++k) {
    const int64_t at = p->flac_walk_at;
    uint8_t bh[4];
    if (!file_bytes(p, buf, base, have, at, bh, 4, may_read)) {
      if (may_read) p->flac_scanned = true;
      return false;
    }
    const uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | bh[3];
    if ((bh[0] & 0x7F) == 3 && !p->seekpts && len >= 18 && len <= 65536) {
      uint8_t *t = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
      if (!t) return false;
      if (!file_bytes(p, buf, base, have, at + 4, t, len, may_read)) {
        free(t);
        if (may_read) p->flac_scanned = true;
        return false; /* open_input: the first seek reads it */
      }
      p->seekpts = heap_caps_malloc((len / 18) * sizeof(flac_seekpt_t), MALLOC_CAP_SPIRAM);
      for (uint32_t i = 0; p->seekpts && i + 18 <= len; i += 18) {
        uint64_t smp = 0, off = 0;
        for (int b = 0; b < 8; ++b) {
          smp = (smp << 8) | t[i + b];
          off = (off << 8) | t[i + 8 + b];
        }
        if (smp == UINT64_MAX) continue; /* a placeholder */
        p->seekpts[p->n_seekpts].sample = smp;
        p->seekpts[p->n_seekpts].offset = off;
        p->n_seekpts++;
      }
      free(t);
    }
    p->flac_walk_at = at + 4 + (int64_t)len;
    if (bh[0] & 0x80) {
      p->flac_scanned = true;
      p->flac_audio_start = p->flac_walk_at;
      ESP_LOGI(TAG, "FLAC: frames from byte %lld, seek table of %u points, blocks %" PRIu32
               "..%" PRIu32, (long long)p->flac_audio_start, (unsigned)p->n_seekpts,
               p->flac.min_block, p->flac.max_block);
      return true;
    }
  }
  p->flac_scanned = true;
  return false;
}

static bool flac_scan(player_t *p) { return flac_walk(p, NULL, 0, 0, true); }

/* The first FLAC frame at or after byte off (looking 128 KiB on). */
static bool flac_frame_near(player_t *p, int64_t off, int64_t *pos, uint64_t *sample) {
  enum { CHUNK = 16384, HDR = 16 };
  uint8_t *b = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
  if (!b) return false;
  bool found = false;
  for (int k = 0; k < 8 && !found; ++k) {
    const int n = p->src.read_at(&p->src, off, b, CHUNK);
    if (n <= HDR) break;
    for (int i = 0; i + HDR <= n; ++i) {
      uint64_t s;
      if (b[i] == 0xFF && flac_frame_at(b + i, (size_t)(n - i), &p->flac, p->wav.channels,
                                        p->wav.bits, &s) &&
          (!p->total_frames || s < p->total_frames)) {
        *pos = off + i;
        *sample = s;
        found = true;
        break;
      }
    }
    off += n - HDR;
  }
  free(b);
  return found;
}

/* The last FLAC frame that starts at or before sample target, or one up to
 * half a second before it: the seek table (when there is one) brackets it,
 * then a search by the bytes narrows it down. */
static bool flac_locate(player_t *p, uint64_t target, int64_t *pos, uint64_t *sample) {
  if (!flac_scan(p)) return false;
  int64_t lo_off = p->flac_audio_start, hi_off = p->src.size;
  uint64_t lo_s = 0, hi_s = p->total_frames;
  for (size_t i = 0; i < p->n_seekpts; ++i) {
    const flac_seekpt_t *sp = &p->seekpts[i];
    if (sp->sample <= target) {
      lo_s = sp->sample;
      lo_off = p->flac_audio_start + (int64_t)sp->offset;
    } else {
      hi_s = sp->sample;
      hi_off = p->flac_audio_start + (int64_t)sp->offset;
      break;
    }
  }
  int tries = 0;
  if (hi_off > lo_off && hi_s > lo_s) {
    for (; tries < 16 && target - lo_s > (uint64_t)p->wav.rate / 2U; ++tries) {
      if (hi_off - lo_off < 64) break;
      int64_t guess = lo_off + (int64_t)((double)(target - lo_s) / (double)(hi_s - lo_s) *
                                         (double)(hi_off - lo_off));
      guess -= 8192; /* the frame found comes after the guess */
      if (guess <= lo_off) guess = lo_off + 1;
      if (guess >= hi_off) guess = lo_off + (hi_off - lo_off) / 2;
      int64_t fpos;
      uint64_t fs;
      if (!flac_frame_near(p, guess, &fpos, &fs) || fpos >= hi_off) {
        hi_off = guess; /* nothing new between the guess and hi */
        continue;
      }
      if (fs > target) {
        hi_off = fpos;
        hi_s = fs;
      } else if (fpos > lo_off) {
        lo_off = fpos;
        lo_s = fs;
      } else {
        break;
      }
    }
  }
  *pos = lo_off;
  *sample = lo_s;
  ESP_LOGI(TAG, "FLAC seek: frame at byte %lld, sample %llu (%d reads), %.2f s to drop",
           (long long)lo_off, (unsigned long long)lo_s, tries,
           (double)(target - lo_s) / p->wav.rate);
  return true;
}

/* A fresh FLAC decoder, fed fLaC and STREAMINFO before the frames. */
static bool flac_restart(player_t *p) {
  return decoder_open(p);
}

/* To to_s seconds: the speakers drop what they hold (FLUSH up to the next
 * frame's sequence number, as an AirPlay sender's FLUSHBUFFERED), the file
 * goes on from there, and the stream starts again under a new anchor. The
 * RTP time and sequence go on as they were. Nothing changes when the place
 * cannot be found. */
static void do_seek(player_t *p, double to_s, const char *why) {
  if (p->play_ix != p->in_ix) { /* the speakers still play the track before */
    (void)goto_track(p, p->play_ix, to_s);
    return;
  }
  if (!p->src.seek || !p->src.read_at) return;
  const uint64_t frate = (uint64_t)p->wav.rate; /* the file's: seeks count its frames */
  const uint64_t fac = p->hr.factor > 1 ? (uint64_t)p->hr.factor : 1U;
  uint64_t target = to_s > 0 ? (uint64_t)(to_s * (double)frate) : 0;
  if (p->total_frames && target >= p->total_frames) target = p->total_frames - 1;
  const int64_t t0 = esp_timer_get_time();
  int64_t pos;
  uint64_t at = target, skip = 0;
  uint32_t m4a_sample = 0;
  if (p->kind == KIND_WAV) {
    const uint64_t fb = (uint64_t)p->wav.channels * (uint64_t)p->wav.container_bits / 8U;
    pos = (int64_t)p->id3_bytes + (int64_t)p->wav.data_offset + (int64_t)(target * fb);
  } else if (p->kind == KIND_FLAC) {
    if (!flac_locate(p, target, &pos, &at)) {
      ESP_LOGW(TAG, "seek to %.1f s: no FLAC frame found there", to_s);
      return;
    }
    skip = target - at;
  } else if (p->kind == KIND_M4A) {
    /* exact: the sample that holds the place, from the index; AAC and MP3
     * start a sample earlier, as their frames overlap */
    const m4a_track_t *t = &p->m4a;
    uint64_t st = 0;
    const uint64_t media = target + p->trim_start; /* the encoder's delay comes first */
    const uint64_t ts = media * t->timescale / frate;
    m4a_sample = m4a_sample_at(t, ts, &st);
    if (m4a_sample >= t->samples) m4a_sample = t->samples - 1;
    if (t->codec != M4A_ALAC && m4a_sample > 0) {
      m4a_sample--;
      st -= m4a_sample_delta(t, m4a_sample);
    }
    uint64_t at_media = st * frate / t->timescale;
    if (at_media > media) at_media = media;
    skip = media - at_media;
    at = target;
    m4a_iter_t it;
    m4a_iter_seek(t, &it, m4a_sample);
    uint64_t off;
    uint32_t size;
    if (!m4a_iter_get(t, &it, &off, &size)) return;
    pos = (int64_t)off;
  } else {
    /* MP3 and AAC (ADTS): about the place, from the Xing TOC, the length or
     * the bit rate so far; the decoder finds the next frame */
    const int64_t audio0 = (int64_t)p->id3_bytes;
    const int64_t bytes = p->kind == KIND_MP3 && p->mp3_ok && p->mp3.xing_bytes
                              ? (int64_t)p->mp3.xing_bytes
                              : p->src.size - audio0;
    double frac = -1;
    if (bytes > 0 && p->total_frames) {
      const double pct = (double)target * 100.0 / (double)p->total_frames;
      if (p->kind == KIND_MP3 && p->mp3.has_toc) {
        const int i = pct < 99 ? (int)pct : 99;
        const double a = p->mp3.toc[i], b = i < 99 ? p->mp3.toc[i + 1] : 256.0;
        frac = (a + (b - a) * (pct - i)) / 256.0;
      } else {
        frac = pct / 100.0;
      }
    } else if (bytes > 0 && p->dec_frames >= frate) {
      frac = (double)target * (double)p->dec_bytes / (double)p->dec_frames / (double)bytes;
    } else if (bytes > 0 && p->kind == KIND_MP3 && p->mp3_ok && p->mp3.kbps) {
      frac = (double)target / (double)frate * p->mp3.kbps * 125.0 / (double)bytes;
    }
    if (frac < 0) {
      ESP_LOGW(TAG, "seek to %.1f s: the %s file's length is not known yet", to_s, kind_name(p));
      return;
    }
    if (frac > 0.999) frac = 0.999;
    pos = audio0 + (int64_t)(frac * (double)bytes);
  }
  pipe_park(p); /* the decoder task lets go of the input */
  const esp_err_t err = p->src.seek(&p->src, pos);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "seek to %.1f s: %s", to_s,
             err == ESP_ERR_NOT_SUPPORTED ? "the server does not seek (no HTTP Range)"
                                          : esp_err_to_name(err));
    pipe_resume(p);
    return;
  }
  p->in_done = false;
  /* the speakers and the relay drop what is not played yet */
  p->wlen = 0;
  sound_engine_pause();
  sound_engine_set_immediate_flush(p->seq, p->rtp);
  p->buf_len = p->buf_pos = 0;
  p->eof = false;
  p->dec_done = false;
  p->force_eos = false;
  p->eos_empty = p->eos_calls = 0;
  p->eos_frames = 0;
  p->dpcm_pending = 0;
  p->fill = 0;
  p->buf_abs = pos;
  if (p->kind == KIND_M4A) m4a_iter_seek(&p->m4a, &p->m4a_it, m4a_sample);
  if (p->kind != KIND_WAV &&
      !(p->kind == KIND_FLAC ? flac_restart(p) : decoder_open(p))) {
    st_set(P_FAILED, "No %s decoder after the seek", kind_name(p));
    atomic_store(&s_stop, true);
    pipe_resume(p);
    return;
  }
  if ((p->kind == KIND_MP3 || p->kind == KIND_AAC) && pos == (int64_t)p->id3_bytes)
    skip = p->trim_start; /* from the very start: the delay again */
  p->frames_read = at;
  p->skip_frames = skip;
  p->out_left = !p->trim_total ? UINT64_MAX : target < p->trim_total ? p->trim_total - target : 0;
  p->rtp0 = p->rtp - (uint32_t)(target / fac);
  decim_reset(&p->hr);
  p->burst_rtp = p->rtp;
  p->playing = false;
  p->paused = false;
  p->finishing = false;
  p->need_anchor = true;
  /* the CRC of 10 s from here: an exact seek gives the CRC the Mac finds
   * for that place in the file */
  p->crc = 0;
  p->crc_frames = 0;
  p->crc_logged = false;
  p->crc_from_s = (double)target / (double)frate;
  taskENTER_CRITICAL(&s_mux);
  s_st.pcm_crc10 = 0;
  taskEXIT_CRITICAL(&s_mux);
  ESP_LOGI(TAG, "%s %.1f s: byte %lld, in %lld ms", why ? why : "seek to", (double)target / (double)frate,
           (long long)pos, (long long)((esp_timer_get_time() - t0) / 1000));
  st_set(P_PLAYING, NULL);
  pipe_resume(p);
}

/* ---- the queue and gapless (phase 3d.3) ---- */

static int queue_len(void) {
  xSemaphoreTake(s_qlock, portMAX_DELAY);
  const int n = s_queue_n;
  xSemaphoreGive(s_qlock);
  return n;
}

static bool queue_url(int k, char *out, size_t len) {
  xSemaphoreTake(s_qlock, portMAX_DELAY);
  const bool ok = k >= 0 && k < s_queue_n;
  if (ok) strlcpy(out, s_queue[k], len);
  xSemaphoreGive(s_qlock);
  return ok;
}

/* What the speakers play now, for the status. */
static void publish_track(const track_info_t *ti) {
  taskENTER_CRITICAL(&s_mux);
  strlcpy(s_st.url, ti->url, sizeof(s_st.url));
  strlcpy(s_st.format, ti->format, sizeof(s_st.format));
  s_st.rate = ti->rate;
  s_st.bits = ti->bits;
  s_st.channels = ti->channels;
  s_st.layout = ti->layout;
  s_st.duration_s = ti->duration_s;
  s_st.duration_estimated = false;
  s_st.index = ti->ix;
  taskEXIT_CRITICAL(&s_mux);
}

/* Lets go of the input: its file and decoder. The buffers stay. */
static void track_close(player_t *p) {
  if (p->src.close) p->src.close(&p->src);
  memset(&p->src, 0, sizeof(p->src));
  if (p->dec) esp_audio_simple_dec_close(p->dec);
  p->dec = NULL;
  if (p->adec) esp_audio_dec_close(p->adec);
  p->adec = NULL;
  m4a_close(&p->m4a);
  memset(&p->m4a, 0, sizeof(p->m4a));
  memset(&p->m4a_it, 0, sizeof(p->m4a_it));
  free(p->afr);
  p->afr = NULL;
  free(p->seekpts);
  p->seekpts = NULL;
  p->n_seekpts = 0;
  p->kind = KIND_WAV;
  memset(&p->wav, 0, sizeof(p->wav));
  p->total_frames = 0;
  p->m4a_codec[0] = '\0';
  p->m4a_duration_s = 0;
  p->m4a_warned = false;
  memset(&p->mp3, 0, sizeof(p->mp3));
  p->mp3_ok = false;
  p->dpcm_pending = 0;
  p->dec_done = false;
  p->force_eos = false;
  p->eos_empty = p->eos_calls = 0;
  p->eos_frames = 0;
  p->probing = false;
  p->pcm_bytes = p->pcm_shift = 0;
  p->buf_len = p->buf_pos = 0;
  p->buf_abs = 0;
  p->frames_read = 0;
  p->eof = false;
  p->id3_bytes = 0;
  p->flac_scanned = false;
  p->flac_audio_start = 0;
  p->flac_walk_at = 0;
  p->flac_scan = 0;
  memset(&p->flac, 0, sizeof(p->flac));
  p->skip_frames = 0;
  p->dec_bytes = p->dec_frames = 0;
  p->out_left = UINT64_MAX;
  p->trim_start = p->trim_total = 0;
  p->track_frames = 0;
  p->in_done = true; /* nothing to read: the decoder task waits */
}

/* Opens queue track *k, or the first after it that opens. */
static bool open_track(player_t *p, int *k) {
  for (; *k < queue_len() && !atomic_load(&s_stop); ++*k) {
    char url[PL_URL_MAX];
    if (!queue_url(*k, url, sizeof(url))) break;
    st_set(P_STARTING, "Opening %s", url);
    esp_err_t serr;
#if CONFIG_GATEWAY_USB_PLAYER_USB
    if (strncmp(url, "usb:", 4) == 0) {
      char path[PL_URL_MAX + 8];
      snprintf(path, sizeof(path), "%s%s", GW_USB_MOUNT, url + 4);
      serr = gw_usb_ready() ? player_source_file_open(&p->src, path) : ESP_ERR_INVALID_STATE;
      if (serr != ESP_OK)
        st_set(P_FAILED, "%s", gw_usb_ready() ? "The file is not on the USB drive" : "No USB drive");
    } else
#endif
    serr = player_source_http_open(&p->src, url);
    if (serr == ESP_OK && open_input(p)) {
      p->in_ix = *k;
      p->ti.ix = *k;
      p->in_done = false;
      strlcpy(p->ti.url, url, sizeof(p->ti.url));
      ESP_LOGI(TAG, "track %d of %d: %s", *k + 1, queue_len(), url);
      return true;
    }
    if (s_st.state != P_FAILED) st_set(P_FAILED, "The file is not reachable");
    ESP_LOGW(TAG, "track %d (%s) does not play: %s", *k + 1, url, s_st.message);
    track_close(p);
  }
  return false;
}

/* The speakers can go on with the new input in the same stream. */
static bool same_stream(const player_t *p) {
  const int out_rate = p->hr.factor > 1 ? p->hr.out_rate : p->wav.rate;
  return p->session && out_rate == p->sess_rate && p->pcm_bits == p->sess_bits &&
         p->wav.layout == p->sess_layout && p->wav.present == p->sess_present;
}

static int volume_now(void) {
  taskENTER_CRITICAL(&s_mux);
  const int v = s_next_volume_db;
  taskEXIT_CRITICAL(&s_mux);
  return v;
}

/* A stream for the input's format: the same one, or a new session (the
 * rate, the depth or the channels change). */
static bool stream_for_input(player_t *p) {
  if (same_stream(p)) return true;
  if (p->session) ESP_LOGI(TAG, "new format: a new stream to the speakers");
  session_close(p);
  memset(p->lane, 0, sizeof(p->lane));
  return session_open(p, volume_now());
}

/* To queue track k (next, previous, a jump) at pos_s: the speakers drop
 * what they hold, as on a seek. */
static bool goto_track(player_t *p, int k, double pos_s) {
  pipe_park(p); /* stays parked when it fails: the player stops */
  if (p->session) {
    p->wlen = 0;
    sound_engine_pause();
    sound_engine_set_immediate_flush(p->seq, p->rtp);
  }
  p->fill = 0;
  p->n_bnd = 0;
  p->paused = p->finishing = p->playing = false;
  p->need_anchor = true;
  decim_reset(&p->hr);
  track_close(p);
  if (!open_track(p, &k)) return false;
  if (!stream_for_input(p)) return false;
  p->burst_rtp = p->rtp;
  p->rtp0 = p->rtp;
  p->play_ix = k;
  publish_track(&p->ti);
  p->crc = 0;
  p->crc_frames = 0;
  p->crc_logged = false;
  p->crc_from_s = 0;
  if (pos_s > 0) do_seek(p, pos_s, "track start at");
  st_set(P_PLAYING, NULL);
  pipe_resume(p);
  return true;
}

/* At the end of the input: the next track. Same format: its PCM goes on in
 * the frame being filled (gapless), and the status follows when the
 * speakers get there. Another format: the speakers play out what they
 * hold, then a new stream. False when the queue is done. */
static bool next_input(player_t *p) {
  int k = p->in_ix + 1;
  if (k >= queue_len()) return false;
  track_close(p);
  if (!open_track(p, &k)) return false;
  st_set(P_PLAYING, NULL); /* the speakers still play the track before */
  if (same_stream(p)) {
    if (p->n_bnd == PL_BOUNDARIES) { /* tracks shorter than the lead: drop the oldest */
      memmove(&p->bnd[0], &p->bnd[1], sizeof(p->bnd[0]) * (PL_BOUNDARIES - 1));
      p->n_bnd--;
    }
    p->bnd[p->n_bnd].rtp = p->rtp + p->fill;
    p->bnd[p->n_bnd].ti = p->ti;
    p->n_bnd++;
    ESP_LOGI(TAG, "gapless on into track %d", k + 1);
    return true;
  }
  /* the end of the last frame, then the speakers play out */
  if (p->fill) {
    for (int c = 0; c < 8; ++c) memset(&p->pcm[c][p->fill], 0, sizeof(int32_t) * (PL_FRAME - p->fill));
    p->fill = 0;
    if (!send_frame(p)) return false;
  }
  if (!flush_wbuf(p)) return false;
  st_set(P_FINISHING, "The speakers play out the track before a new format");
  while (!atomic_load(&s_stop) && (p->paused || (p->playing && lead_ns(p) > -200000000LL))) {
    if (atomic_load(&s_goto) != PL_GOTO_NONE || atomic_load(&s_seek_ms) >= 0)
      return controls(p); /* next, previous or a seek: for the track still heard */
    const int pr = atomic_exchange(&s_pause_req, 0);
    if (pr == 1) do_pause(p);
    else if (pr == 2) do_resume(p);
    status_update(p);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  p->n_bnd = 0;
  p->playing = false;
  if (!stream_for_input(p)) return false;
  p->rtp0 = p->rtp;
  p->burst_rtp = p->rtp;
  p->play_ix = k;
  publish_track(&p->ti);
  return true;
}

/* The speakers got to the next track: the status follows. */
static void boundaries(player_t *p) {
  while (p->n_bnd && p->playing && (int32_t)(play_rtp(p) - p->bnd[0].rtp) >= 0) {
    p->rtp0 = p->bnd[0].rtp;
    p->play_ix = p->bnd[0].ti.ix;
    publish_track(&p->bnd[0].ti);
    ESP_LOGI(TAG, "the speakers play track %d now", p->play_ix + 1);
    memmove(&p->bnd[0], &p->bnd[1], sizeof(p->bnd[0]) * (PL_BOUNDARIES - 1));
    p->n_bnd--;
  }
}

/* Requests from the web (pause, resume, seek, next, previous) and a
 * re-seek after a lane was added. */
static bool controls(player_t *p) {
  const int pr = atomic_exchange(&s_pause_req, 0);
  if (pr == 1) do_pause(p);
  else if (pr == 2) do_resume(p);
  const int g = atomic_exchange(&s_goto, PL_GOTO_NONE);
  if (g != PL_GOTO_NONE) {
    int k = g;
    if (g == PL_GOTO_NEXT) k = p->play_ix + 1;
    else if (g == PL_GOTO_PREV) k = play_pos_s(p) > 3.0 ? p->play_ix : p->play_ix - 1;
    if (k < 0) k = 0;
    if (k >= queue_len()) {
      ESP_LOGI(TAG, "next: the queue is done");
      return false;
    }
    ESP_LOGI(TAG, "to track %d", k + 1);
    if (!goto_track(p, k, 0)) return false;
  }
  const long long ms = atomic_exchange(&s_seek_ms, -1LL);
  if (ms >= 0) do_seek(p, (double)ms / 1000.0, NULL);
  if (p->reseek_at_us && esp_timer_get_time() >= p->reseek_at_us) {
    p->reseek_at_us = 0;
    if (p->playing && !p->paused && !p->finishing)
      do_seek(p, play_pos_s(p), "a speaker has a new role or lane: again from");
  }
  return true;
}

static void status_update(player_t *p) {
  boundaries(p);
  const int lm = (int)(lead_ns(p) / 1000000LL);
  taskENTER_CRITICAL(&s_mux);
  s_st.lead_ms = lm;
  s_st.position_s = play_pos_s(p); /* what the speakers play now */
  for (int k = 0; k < PL_LANES; ++k) s_st.lane_bytes[k] = p->lane[k].bytes;
  if ((p->kind == KIND_MP3 || p->kind == KIND_AAC) && !p->total_frames && p->src.size > 0 &&
      p->play_ix == p->in_ix && p->dec_bytes &&
      p->dec_frames >= (uint64_t)p->wav.rate * 2U) {
    /* bytes per second so far (exact for constant bit rate) */
    const double audio = (double)p->src.size - (double)p->id3_bytes;
    s_st.duration_s = audio * (double)p->dec_frames / (double)p->dec_bytes / p->wav.rate;
    s_st.duration_estimated = true;
  }
  taskEXIT_CRITICAL(&s_mux);
  const int64_t now_us = esp_timer_get_time();
  if (now_us - p->log_us >= PL_STATUS_LOG_MS * 1000LL) {
    p->log_us = now_us;
    taskENTER_CRITICAL(&s_mux);
    s_st.perf_speed = (double)p->frames_sent * PL_FRAME / p->rate / (PL_STATUS_LOG_MS / 1000.0);
    s_st.perf_read_ms = (int)(p->t_read_us / 1000);
    s_st.perf_enc_ms = (int)(p->t_enc_us / 1000);
    s_st.perf_send_ms = (int)(p->t_send_us / 1000);
    s_st.perf_read_kib = (int)(p->read_bytes / 1024U);
    s_st.perf_dec_ms = (int)(p->t_dec_us / 1000);
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "PLAYER %s seq %" PRIu32 " lead %d ms, %.1f s of %.1f s | %u frames "
             "(%.2fx real time) | read %lld ms (%llu KiB), decode %lld ms, hi-res %lld ms, encode %lld ms, "
             "send %lld ms | queue full %lld ms, empty %lld ms",
             state_name(s_st.state), p->seq, lm, s_st.position_s, s_st.duration_s,
             (unsigned)p->frames_sent,
             (double)p->frames_sent * PL_FRAME / p->rate / (PL_STATUS_LOG_MS / 1000.0),
             (long long)(p->t_read_us / 1000), (unsigned long long)(p->read_bytes / 1024U),
             (long long)(p->t_dec_us / 1000), (long long)(p->t_hr_us / 1000),
             (long long)(p->t_enc_us / 1000), (long long)(p->t_send_us / 1000),
             (long long)(p->t_put_wait_us / 1000), (long long)(p->t_get_wait_us / 1000));
    p->t_read_us = p->t_enc_us = p->t_send_us = p->t_dec_us = p->t_hr_us = 0;
    p->t_put_wait_us = p->t_get_wait_us = 0;
    p->read_bytes = 0;
    p->frames_sent = 0;
  }
}

static void player_run(int start, int volume_db) {
  if (gw_radio_active()) gw_radio_stop(); /* one local sender at a time */
  if (!airplay_release()) {
    if (!atomic_load(&s_stop)) st_set(P_FAILED, "The AirPlay sender did not let go; try again");
    airplay_listen();
    return;
  }
  take_speakers();
  const bool listening = airplay_listen();

  player_t *p = heap_caps_calloc(1, sizeof(player_t), MALLOC_CAP_SPIRAM);
  if (!p) {
    st_set(P_FAILED, "Out of memory");
    goto out_nop;
  }
  p->sock = -1;
  p->buf = heap_caps_malloc(PL_HEADER_MAX + 16U, MALLOC_CAP_SPIRAM);
  p->buf_cap = PL_HEADER_MAX;
  p->out = heap_caps_malloc(PL_FRAME * 3U + 64U, MALLOC_CAP_SPIRAM);
  p->pkt = NULL;
  p->wbuf = heap_caps_malloc(PL_WBUF, MALLOC_CAP_SPIRAM);
  if (!p->buf || !p->out || !p->wbuf) {
    st_set(P_FAILED, "Out of memory");
    goto out;
  }
  p->out_left = UINT64_MAX;
  {
    int k = start;
    if (!open_track(p, &k)) goto out;
    if (!session_open(p, volume_db)) goto out;
    p->play_ix = k;
    publish_track(&p->ti);
    if (!worker_start(p)) {
      ESP_LOGE(TAG, "decoder task: %u KiB PSRAM free (largest block %u KiB)",
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024U));
      st_set(P_FAILED, "Out of memory for the decoder task");
      goto out;
    }
  }
  st_set(P_PLAYING, NULL);

  int64_t rest_us = esp_timer_get_time();
  while (!atomic_load(&s_stop)) {
    /* filling the lead goes flat out for seconds: one tick a second for
     * core 1's idle task (the task watchdog) */
    if (esp_timer_get_time() - rest_us > 1000000) {
      vTaskDelay(1);
      rest_us = esp_timer_get_time();
    }
    if (!controls(p)) break;
    if (p->paused) {
      status_update(p);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (p->finishing) {
      /* not anchored (the end came while buffering): nothing more to play */
      if (!p->playing || lead_ns(p) <= -200000000LL) break;
      status_update(p);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (p->playing && lead_ns(p) > p->max_lead_ns) {
      if (!flush_wbuf(p)) break;
      status_update(p);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (p->playing && !p->in_done && lead_ns(p) < PL_MIN_LEAD_NS) {
      if (!flush_wbuf(p)) break;
      sound_engine_pause();
      p->playing = false;
      p->need_anchor = true;
      st_set(P_BUFFERING, "The file comes too slowly: buffering");
    }
    /* what the decoder task made */
    const int64_t tw = esp_timer_get_time();
    const size_t got = xMessageBufferReceive(p->rb, p->r_msg, PL_PIPE_ITEM + sizeof(pcm_rec_t),
                                             pdMS_TO_TICKS(20));
    p->t_get_wait_us += esp_timer_get_time() - tw;
    if (got < sizeof(pcm_rec_t)) {
      status_update(p);
      continue;
    }
    pcm_rec_t h;
    memcpy(&h, p->r_msg, sizeof(h));
    bool ok = true;
    if (h.type == REC_PCM) ok = pcm_in(p, p->r_msg + sizeof(h), h.n, h.bytes, h.shift);
    else if (h.type == REC_SLOTS) ok = slots_in(p, (const int32_t *)(p->r_msg + sizeof(h)), h.n);
    if (!ok || h.type == REC_ERR) break;
    if (h.type == REC_END) {
      pipe_park(p); /* the input is the player's now */
      ESP_LOGI(TAG, "track %d read: %llu frames out (%.3f s), %llu decoded, %llu of %lld bytes "
               "to the decoder", p->in_ix + 1,
               (unsigned long long)p->track_frames, (double)p->track_frames / p->wav.rate,
               (unsigned long long)p->dec_frames, (unsigned long long)p->dec_bytes,
               (long long)p->src.size);
      const bool more = next_input(p); /* the queue goes on */
      pipe_resume(p);
      if (more) continue;
      if (atomic_load(&s_stop) || !p->session) break;
      if (p->fill) { /* the last frame, filled with silence */
        for (int c = 0; c < 8; ++c)
          memset(&p->pcm[c][p->fill], 0, sizeof(int32_t) * (PL_FRAME - p->fill));
        p->fill = 0;
        if (!send_frame(p)) break;
      }
      if (!flush_wbuf(p)) break;
      st_set(P_FINISHING, "End of the file: the speakers play what they hold");
      p->finishing = true; /* a seek or a pause still works */
      continue;
    }
    if (p->playing && s_st.state != P_PLAYING) st_set(P_PLAYING, NULL);
    status_update(p);
  }

out:
  if (p) {
    if (s_st.state != P_FAILED) st_set(P_STOPPING, "Stopping");
    worker_stop(p); /* before the input goes */
    track_close(p);
    decim_free(&p->hr);
    free(p->dpcm);
    session_close(p);
    free(p->buf);
    free(p->out);
    free(p->wbuf);
    free(p);
  }
out_nop:
  atomic_store(&s_owns, false);
  if (!listening) (void)airplay_listen();
}

static void player_task(void *arg) {
  (void)arg;
  int vol, start;
  taskENTER_CRITICAL(&s_mux);
  s_task = xTaskGetCurrentTaskHandle();
  vol = s_next_volume_db;
  start = s_queue_start;
  taskEXIT_CRITICAL(&s_mux);
  player_run(start, vol);
  taskENTER_CRITICAL(&s_mux);
  if (s_st.state != P_FAILED) {
    s_st.state = P_IDLE;
    s_st.message[0] = '\0';
  }
  s_task = NULL;
  taskEXIT_CRITICAL(&s_mux);
  atomic_store(&s_running, false);
  ESP_LOGI(TAG, "player stopped");
  xSemaphoreGive(s_done);
  vTaskDeleteWithCaps(NULL);
}

/* ---- control ---- */

static bool init_once(void) {
  if (!s_ctl) s_ctl = xSemaphoreCreateMutex();
  if (!s_done) s_done = xSemaphoreCreateBinary();
  if (!s_qlock) s_qlock = xSemaphoreCreateMutex();
  if (!s_queue) s_queue = heap_caps_calloc(PL_QUEUE_MAX, PL_URL_MAX, MALLOC_CAP_SPIRAM);
  return s_ctl && s_done && s_qlock && s_queue;
}

static void stop_locked(void) {
  if (!atomic_load(&s_running)) return;
  atomic_store(&s_stop, true);
  int waited_ms = 0;
  while (atomic_load(&s_running)) {
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) == pdTRUE) continue;
    waited_ms += 500;
    if (waited_ms % PL_STOP_WAIT_MS == 0)
      ESP_LOGW(TAG, "still waiting for the player to stop (%d ms)", waited_ms);
  }
  (void)xSemaphoreTake(s_done, 0);
}

void gw_player_stop(void) {
  if (!init_once()) return;
  xSemaphoreTake(s_ctl, portMAX_DELAY);
  stop_locked();
  xSemaphoreGive(s_ctl);
}

void gw_player_set_volume(int volume_db) {
  volume_db = clamp_db(volume_db);
  taskENTER_CRITICAL(&s_mux);
  s_st.volume_db = volume_db;
  s_next_volume_db = volume_db;
  taskEXIT_CRITICAL(&s_mux);
  if (atomic_load(&s_owns)) sound_engine_set_volume_q15(db_to_q15(volume_db));
}

static bool url_ok(const char *url) {
  if (!url || strlen(url) >= PL_URL_MAX) return false;
  if (strncmp(url, "http://", 7) == 0) return true;
#if CONFIG_GATEWAY_USB_PLAYER_USB
  if (strncmp(url, "usb:/", 5) == 0 && !strstr(url, "/../")) return true;
#endif
  return false;
}

bool gw_player_plays_usb(void) {
  if (!s_qlock || !s_queue) return false;
  bool usb = false;
  xSemaphoreTake(s_qlock, portMAX_DELAY);
  for (int i = 0; i < s_queue_n && !usb; ++i) usb = strncmp(s_queue[i], "usb:", 4) == 0;
  xSemaphoreGive(s_qlock);
  return usb;
}

esp_err_t gw_player_play(const char *url, int volume_db) {
  return gw_player_play_queue(&url, 1, 0, volume_db);
}

esp_err_t gw_player_play_queue(const char *const *urls, int n, int start, int volume_db) {
  if (n < 1 || n > PL_QUEUE_MAX || start < 0 || start >= n) return ESP_ERR_INVALID_ARG;
  for (int i = 0; i < n; ++i)
    if (!url_ok(urls[i])) return ESP_ERR_INVALID_ARG;
  if (!init_once()) return ESP_ERR_NO_MEM;
  if (gw_calib_running()) return ESP_ERR_INVALID_STATE;
  volume_db = clamp_db(volume_db);
  xSemaphoreTake(s_ctl, portMAX_DELAY);
  stop_locked();
  (void)xSemaphoreTake(s_done, 0);
  xSemaphoreTake(s_qlock, portMAX_DELAY);
  for (int i = 0; i < n; ++i) strlcpy(s_queue[i], urls[i], PL_URL_MAX);
  s_queue_n = n;
  xSemaphoreGive(s_qlock);
  const char *url = urls[start];
  atomic_store(&s_goto, PL_GOTO_NONE);
  atomic_store(&s_seek_ms, -1LL);
  atomic_store(&s_pause_req, 0);
  taskENTER_CRITICAL(&s_mux);
  memset(&s_st, 0, sizeof(s_st));
  s_st.state = P_STARTING;
  strlcpy(s_st.url, url, sizeof(s_st.url));
  s_st.volume_db = volume_db;
  s_st.index = start;
  s_queue_start = start;
  s_next_volume_db = volume_db;
  taskEXIT_CRITICAL(&s_mux);
  atomic_store(&s_stop, false);
  atomic_store(&s_running, true);
  TaskHandle_t t = NULL;
  /* PSRAM stack: internal RAM is the Gateway's scarce resource, and the task
   * never writes flash. */
  const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(player_task, "gw_player", PL_TASK_STACK,
                                                        NULL, PL_TASK_PRIORITY, &t, PL_TASK_CORE,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) atomic_store(&s_running, false);
  xSemaphoreGive(s_ctl);
  if (ok != pdPASS) {
    st_set(P_FAILED, "Out of memory");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "play %s at %d dB (%d in the queue)", url, volume_db, n);
  return ESP_OK;
}

esp_err_t gw_player_add(const char *url) {
  if (!url_ok(url)) return ESP_ERR_INVALID_ARG;
  if (!init_once()) return ESP_ERR_NO_MEM;
  xSemaphoreTake(s_qlock, portMAX_DELAY);
  const bool room = s_queue_n < PL_QUEUE_MAX;
  if (room) strlcpy(s_queue[s_queue_n++], url, PL_URL_MAX);
  xSemaphoreGive(s_qlock);
  return room ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t go(int g) {
  if (!atomic_load(&s_running) || !atomic_load(&s_owns)) return ESP_ERR_INVALID_STATE;
  atomic_store(&s_goto, g);
  return ESP_OK;
}

esp_err_t gw_player_next(void) { return go(PL_GOTO_NEXT); }
esp_err_t gw_player_prev(void) { return go(PL_GOTO_PREV); }
esp_err_t gw_player_jump(int index) {
  if (index < 0 || index >= queue_len()) return ESP_ERR_INVALID_ARG;
  return go(index);
}

esp_err_t gw_player_pause(bool pause) {
  if (!atomic_load(&s_running) || !atomic_load(&s_owns)) return ESP_ERR_INVALID_STATE;
  atomic_store(&s_pause_req, pause ? 1 : 2);
  return ESP_OK;
}

esp_err_t gw_player_seek(double position_s) {
  if (!atomic_load(&s_running) || !atomic_load(&s_owns)) return ESP_ERR_INVALID_STATE;
  if (position_s < 0) position_s = 0;
  atomic_store(&s_seek_ms, (long long)(position_s * 1000.0));
  return ESP_OK;
}

/* ---- web ---- */

static esp_err_t status_handler(httpd_req_t *req) {
  player_status_t st;
  taskENTER_CRITICAL(&s_mux);
  st = s_st;
  st.volume_db = s_next_volume_db; /* the one it plays at, or the next file will */
  taskEXIT_CRITICAL(&s_mux);
  cJSON *root = cJSON_CreateObject();
  if (!root) return httpd_resp_send_500(req);
  cJSON_AddStringToObject(root, "state", state_name(st.state));
  cJSON_AddBoolToObject(root, "active", gw_player_active());
  cJSON_AddStringToObject(root, "message", st.message);
  cJSON_AddStringToObject(root, "url", st.url);
  cJSON_AddStringToObject(root, "format", st.format);
  cJSON_AddNumberToObject(root, "rate", st.rate);
  cJSON_AddNumberToObject(root, "bits", st.bits);
  cJSON_AddNumberToObject(root, "channels", st.channels);
  cJSON_AddNumberToObject(root, "volume_db", st.volume_db);
  cJSON_AddNumberToObject(root, "lead_ms", st.lead_ms);
  cJSON_AddNumberToObject(root, "position_s", st.position_s);
  cJSON_AddNumberToObject(root, "duration_s", st.duration_s);
  cJSON_AddNumberToObject(root, "index", st.index);
#if CONFIG_GATEWAY_USB_PLAYER_USB
  {
    gw_usb_status_t u;
    gw_usb_status(&u);
    cJSON *usb = cJSON_AddObjectToObject(root, "usb");
    if (usb) {
      cJSON_AddStringToObject(usb, "state", u.state);
      if (u.message[0]) cJSON_AddStringToObject(usb, "message", u.message);
      if (u.size_bytes) cJSON_AddNumberToObject(usb, "size_mb", (double)(u.size_bytes / 1048576U));
      if (u.free_bytes) cJSON_AddNumberToObject(usb, "free_mb", (double)(u.free_bytes / 1048576U));
    }
  }
#endif
  cJSON *q = cJSON_AddArrayToObject(root, "queue");
  if (q && s_qlock && s_queue) {
    xSemaphoreTake(s_qlock, portMAX_DELAY);
    for (int i = 0; i < s_queue_n; ++i) cJSON_AddItemToArray(q, cJSON_CreateString(s_queue[i]));
    xSemaphoreGive(s_qlock);
  }
  if (st.duration_estimated) cJSON_AddBoolToObject(root, "duration_estimated", true);
  if (st.pcm_crc10) {
    char crc[12];
    snprintf(crc, sizeof(crc), "%08" PRIx32, st.pcm_crc10);
    cJSON_AddStringToObject(root, "pcm_crc10", crc);
  }
  cJSON *perf = cJSON_AddObjectToObject(root, "perf_10s");
  if (perf) {
    cJSON_AddNumberToObject(perf, "speed", st.perf_speed);
    cJSON_AddNumberToObject(perf, "read_ms", st.perf_read_ms);
    cJSON_AddNumberToObject(perf, "read_kib", st.perf_read_kib);
    cJSON_AddNumberToObject(perf, "decode_ms", st.perf_dec_ms);
    cJSON_AddNumberToObject(perf, "encode_ms", st.perf_enc_ms);
    cJSON_AddNumberToObject(perf, "send_ms", st.perf_send_ms);
  }
  cJSON *lanes = cJSON_AddArrayToObject(root, "lanes");
  for (int k = 0; lanes && k < PL_LANES; ++k) {
    if (!st.lane_bits[k]) continue;
    cJSON *l = cJSON_CreateObject();
    cJSON_AddStringToObject(l, "role", surround_role_name(k - 1));
    cJSON_AddNumberToObject(l, "bits", st.lane_bits[k]);
    cJSON_AddNumberToObject(l, "sent_kib", st.lane_bytes[k] / 1024U);
    cJSON_AddItemToArray(lanes, l);
  }
  char *s = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!s) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/json");
  const esp_err_t r = httpd_resp_sendstr(req, s);
  free(s);
  return r;
}

static esp_err_t control_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  enum { BODY_MAX = PL_QUEUE_MAX * PL_URL_MAX + 512 };
  if (req->content_len > BODY_MAX) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too long");
  char *body = heap_caps_malloc((size_t)req->content_len + 1U, MALLOC_CAP_SPIRAM);
  if (!body) return httpd_resp_send_500(req);
  const int len = req->content_len;
  int got = 0;
  while (got < len) {
    const int r = httpd_req_recv(req, body + got, len - got);
    if (r <= 0) {
      free(body);
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    }
    got += r;
  }
  body[got] = '\0';
  cJSON *j = cJSON_Parse(body);
  free(body);
  if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not JSON");
  const cJSON *cmd = cJSON_GetObjectItem(j, "cmd");
  const cJSON *url = cJSON_GetObjectItem(j, "url");
  const cJSON *vol = cJSON_GetObjectItem(j, "vol");
  esp_err_t err = ESP_ERR_INVALID_ARG;
  const cJSON *urls = cJSON_GetObjectItem(j, "urls");
  const cJSON *index = cJSON_GetObjectItem(j, "index");
  if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "play") == 0 && cJSON_IsArray(urls)) {
    const int n = cJSON_GetArraySize(urls);
    const char **list = n > 0 && n <= PL_QUEUE_MAX ? calloc((size_t)n, sizeof(char *)) : NULL;
    if (list) {
      bool strings = true;
      for (int i = 0; i < n; ++i) {
        const cJSON *u = cJSON_GetArrayItem(urls, i);
        strings &= cJSON_IsString(u);
        list[i] = strings ? u->valuestring : NULL;
      }
      if (strings)
        err = gw_player_play_queue(list, n, cJSON_IsNumber(index) ? index->valueint : 0,
                                   cJSON_IsNumber(vol) ? vol->valueint : volume_now());
      free(list);
    }
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "play") == 0 && cJSON_IsString(url)) {
    err = gw_player_play(url->valuestring, cJSON_IsNumber(vol) ? vol->valueint : volume_now());
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "add") == 0 && cJSON_IsString(url)) {
    err = gw_player_add(url->valuestring);
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "next") == 0) {
    err = gw_player_next();
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "prev") == 0) {
    err = gw_player_prev();
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "jump") == 0 && cJSON_IsNumber(index)) {
    err = gw_player_jump(index->valueint);
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "stop") == 0) {
    gw_player_stop();
    err = ESP_OK;
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "pause") == 0) {
    err = gw_player_pause(true);
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "resume") == 0) {
    err = gw_player_pause(false);
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "seek") == 0 &&
             cJSON_IsNumber(cJSON_GetObjectItem(j, "pos"))) {
    err = gw_player_seek(cJSON_GetObjectItem(j, "pos")->valuedouble);
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "volume") == 0 && cJSON_IsNumber(vol)) {
    gw_player_set_volume(vol->valueint);
    err = ESP_OK;
  }
  cJSON_Delete(j);
  if (err != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
  return status_handler(req);
}

#if CONFIG_GATEWAY_USB_PLAYER_USB
/* ---- the drive's folders (phase 4) ----
 * GET /api/player/files?path=/Music: the folders and files there, folders
 * first, by name; "play" is a file the player takes (by its extension). */

#define PL_LIST_MAX 400

typedef struct {
  char name[128];
  bool dir;
  int64_t size;
} list_entry_t;

static int url_unhex(int c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* %XX and + in a query value */
static void url_decode(char *s) {
  char *o = s;
  for (const char *i = s; *i; ++i) {
    if (*i == '%' && url_unhex(i[1]) >= 0 && url_unhex(i[2]) >= 0) {
      *o++ = (char)(url_unhex(i[1]) * 16 + url_unhex(i[2]));
      i += 2;
    } else {
      *o++ = *i == '+' ? ' ' : *i;
    }
  }
  *o = '\0';
}

static const char *file_type(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return NULL;
  static const char *const known[] = {"flac", "wav", "m4a", "mp4", "alac", "mp3", "aac"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
    if (strcasecmp(dot + 1, known[i]) == 0) return known[i];
  return NULL;
}

static int entry_cmp(const void *a, const void *b) {
  const list_entry_t *x = a, *y = b;
  if (x->dir != y->dir) return x->dir ? -1 : 1;
  return strcasecmp(x->name, y->name);
}

static esp_err_t files_handler(httpd_req_t *req) {
  char path[PL_URL_MAX] = "/";
  char q[PL_URL_MAX + 16];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "path", path, sizeof(path)) == ESP_OK)
    url_decode(path);
  if (path[0] != '/' || strstr(path, "..")) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path");
  gw_usb_status_t u;
  gw_usb_status(&u);
  cJSON *root = cJSON_CreateObject();
  if (!root) return httpd_resp_send_500(req);
  cJSON_AddStringToObject(root, "path", path);
  cJSON_AddStringToObject(root, "usb", u.state);
  cJSON *arr = cJSON_AddArrayToObject(root, "entries");
  if (gw_usb_ready()) {
    char full[PL_URL_MAX + 8];
    snprintf(full, sizeof(full), "%s%s", GW_USB_MOUNT, path);
    DIR *dir = opendir(full);
    list_entry_t *ents = heap_caps_calloc(PL_LIST_MAX, sizeof(list_entry_t), MALLOC_CAP_SPIRAM);
    int n = 0;
    bool more = false;
    if (dir && ents) {
      struct dirent *de;
      while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue; /* ., .., and the Mac's ._ files */
        if (n == PL_LIST_MAX) {
          more = true;
          break;
        }
        list_entry_t *e = &ents[n++];
        strlcpy(e->name, de->d_name, sizeof(e->name));
        e->dir = de->d_type == DT_DIR;
        e->size = -1;
        if (!e->dir) {
          char fp[PL_URL_MAX + 8 + 260];
          const bool slash = full[strlen(full) - 1] == '/';
          const int w = snprintf(fp, sizeof(fp), "%s%s%s", full, slash ? "" : "/", de->d_name);
          struct stat st;
          if (w > 0 && (size_t)w < sizeof(fp) && stat(fp, &st) == 0) e->size = (int64_t)st.st_size;
        }
      }
      qsort(ents, (size_t)n, sizeof(ents[0]), entry_cmp);
      for (int i = 0; i < n && arr; ++i) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", ents[i].name);
        if (ents[i].dir) {
          cJSON_AddBoolToObject(o, "dir", true);
        } else {
          const char *t = file_type(ents[i].name);
          cJSON_AddNumberToObject(o, "size", (double)ents[i].size);
          if (t) cJSON_AddStringToObject(o, "type", t);
          cJSON_AddBoolToObject(o, "play", t != NULL);
        }
        cJSON_AddItemToArray(arr, o);
      }
      if (more) cJSON_AddBoolToObject(root, "more", true);
    } else {
      cJSON_AddStringToObject(root, "error", dir ? "Out of memory" : "No such folder");
    }
    if (dir) closedir(dir);
    free(ents);
  }
  char *s = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!s) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  const esp_err_t r = httpd_resp_sendstr(req, s);
  free(s);
  return r;
}
#endif

/* The Speakers page's player controls (data/www/player.js, phase 5). */
extern const uint8_t web_player_js_start[] asm("_binary_web_player_js_start");
extern const uint8_t web_player_js_end[] asm("_binary_web_player_js_end");

static esp_err_t player_js_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, (const char *)web_player_js_start,
                         web_player_js_end - web_player_js_start);
}

void gw_player_register_web(httpd_handle_t server) {
  const httpd_uri_t uris[] = {
      {.uri = "/api/player", .method = HTTP_GET, .handler = status_handler},
      {.uri = "/api/player", .method = HTTP_POST, .handler = control_handler},
      {.uri = "/player.js", .method = HTTP_GET, .handler = player_js_handler},
#if CONFIG_GATEWAY_USB_PLAYER_USB
      {.uri = "/api/player/files", .method = HTTP_GET, .handler = files_handler},
#endif
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
    const esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
    if (err != ESP_OK)
      ESP_LOGE(TAG, "web handler %s not registered: %s (max_uri_handlers?)", uris[i].uri,
               esp_err_to_name(err));
  }
}
