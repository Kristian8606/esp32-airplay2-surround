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

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_bench_alac_lite.h"
#include "gw_calib.h"
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
#define PL_TASK_CORE     1
#define PL_FRAME         352U   /* samples per ALAC frame, like AirPlay */
#define PL_READ_CHUNK    8192U
#define PL_HEADER_MAX    65536U /* the WAV data chunk must start before this */
#define PL_START_DELAY_NS 2000000000LL
#define PL_MAX_LEAD_NS    20000000000LL
#define PL_MAX_LEAD_BUSY_NS 8000000000LL /* lanes over PL_BUSY_KBPS in all */
#define PL_BUSY_KBPS      4000
#define PL_MIN_LEAD_NS    250000000LL
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
               P_STOPPING, P_FAILED } player_state_t;

static const char *state_name(player_state_t s) {
  switch (s) {
    case P_STARTING: return "starting";
    case P_PLAYING: return "playing";
    case P_BUFFERING: return "buffering";
    case P_FINISHING: return "finishing";
    case P_STOPPING: return "stopping";
    case P_FAILED: return "failed";
    default: return "idle";
  }
}

typedef struct {
  player_state_t state;
  char message[96];
  char url[256];
  char format[48];  /* "WAV 48000 Hz / 24 bit / 5.1" */
  int rate, bits, channels, layout;
  int volume_db;
  int lead_ms;
  uint32_t lane_bytes[PL_LANES]; /* sent per lane, for the status kbit/s */
  uint8_t lane_bits[PL_LANES];   /* 0 = no lane */
  double position_s, duration_s;
  /* the last status window (PL_STATUS_LOG_MS): where the time went */
  double perf_speed;   /* frames sent / real time */
  int perf_read_ms, perf_enc_ms, perf_send_ms, perf_read_kib;
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
static char s_next_url[256];
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

typedef struct {
  /* source */
  player_source_t src;
  wav_info_t wav;
  uint8_t *buf;
  size_t buf_len, buf_pos;
  uint64_t frames_read;
  bool eof;
  /* frame being filled: internal slots */
  int32_t pcm[8][PL_FRAME];
  uint32_t fill;
  /* lanes */
  lane_t lane[PL_LANES];
  uint32_t speakers_gen;
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
  int64_t t_read_us, t_enc_us, t_send_us;
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
      l->bits = sig.kind == SURROUND_SIG_CHANNEL ? p->wav.bits : 24;
      const airplay_audio_format_t *f = surround_player_lane_format(p->rate, l->bits, mc);
      if (!f) continue;
      l->ssrc = f->ssrc;
      alac_lite_init(&l->enc, l->bits, PL_ENC_ORDER);
      l->on = true;
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
      if (p->wav.bits == 16) {
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
      const int up = p->wav.bits == 16 ? 256 : 1;
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
  if (p->wlen >= PL_WBUF_FLUSH || (uint32_t)(p->rtp - p->rtp0) < (uint32_t)p->rate)
    return flush_wbuf(p);
  return true;
}

/* Sets the speakers up as the radio does; the announced format is the
 * radio's stereo one for the rate (the lanes' SSRCs tell the speakers the
 * rest, per block). */
static bool session_open(player_t *p, int volume_db) {
  p->rate = p->wav.rate;
  const int ann_bits = p->rate == 48000 ? 24 : 16;
  esp_fill_random(p->key, sizeof(p->key));
  p->seq = esp_random() & PL_SEQ_MASK;
  p->rtp = esp_random();
  p->rtp0 = p->rtp;
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

static bool open_wav(player_t *p) {
  uint8_t *hdr = heap_caps_malloc(PL_HEADER_MAX, MALLOC_CAP_SPIRAM);
  if (!hdr) return false;
  size_t have = 0;
  esp_err_t err = ESP_ERR_INVALID_SIZE;
  while (have < PL_HEADER_MAX && !atomic_load(&s_stop)) {
    const int n = p->src.read(&p->src, hdr + have, PL_HEADER_MAX - have > 4096 ? 4096 : PL_HEADER_MAX - have);
    if (n <= 0) break;
    have += (size_t)n;
    err = wav_parse(hdr, have, &p->wav);
    if (err != ESP_ERR_INVALID_SIZE) break;
  }
  if (err != ESP_OK) {
    st_set(P_FAILED, "%s", err == ESP_ERR_NOT_SUPPORTED ? p->wav.why
                           : "Not a WAV file this player reads (phase 3a: WAV only)");
    free(hdr);
    return false;
  }
  /* what came after the header is the first PCM */
  p->buf_len = have - p->wav.data_offset;
  if (p->buf_len > PL_READ_CHUNK * 2U) {
    st_set(P_FAILED, "WAV header too large");
    free(hdr);
    return false;
  }
  memcpy(p->buf, hdr + p->wav.data_offset, p->buf_len);
  p->buf_pos = 0;
  free(hdr);
  const char *lay = p->wav.layout == 1 ? "mono" : p->wav.layout == 2 ? "stereo"
                  : p->wav.layout == 6 ? "5.1" : "7.1";
  const size_t fb = (size_t)p->wav.channels * (size_t)p->wav.container_bits / 8U;
  double dur = 0;
  if (p->wav.data_size && p->wav.data_size != 0xFFFFFFFFU && fb)
    dur = (double)(p->wav.data_size / fb) / p->wav.rate;
  taskENTER_CRITICAL(&s_mux);
  snprintf(s_st.format, sizeof(s_st.format), "WAV %d Hz / %d bit / %s", p->wav.rate,
           p->wav.bits, lay);
  s_st.rate = p->wav.rate;
  s_st.bits = p->wav.bits;
  s_st.channels = p->wav.channels;
  s_st.layout = p->wav.layout;
  s_st.duration_s = dur;
  taskEXIT_CRITICAL(&s_mux);
  ESP_LOGI(TAG, "file: WAV %d Hz, %d-bit in %d, %d channels (mask 0x%" PRIx32 ", %s), %.1f s",
           p->wav.rate, p->wav.bits, p->wav.container_bits, p->wav.channels, p->wav.mask, lay,
           dur);
  return true;
}

/* PCM bytes into frames; a full frame goes out. */
static bool take_pcm(player_t *p) {
  const size_t cb = (size_t)p->wav.container_bits / 8U;
  const size_t fb = (size_t)p->wav.channels * cb;
  const uint32_t data_end_frames =
      p->wav.data_size && p->wav.data_size != 0xFFFFFFFFU ? p->wav.data_size / (uint32_t)fb : 0;
  while (p->buf_len - p->buf_pos >= fb) {
    if (data_end_frames && p->frames_read >= data_end_frames) {
      p->eof = true; /* chunks after the data chunk are not audio */
      p->buf_pos = p->buf_len;
      break;
    }
    const uint8_t *s = p->buf + p->buf_pos;
    for (int c = 0; c < p->wav.channels; ++c, s += cb) {
      int32_t v;
      if (cb == 2) v = (int16_t)(s[0] | (s[1] << 8));
      else if (cb == 3) v = (int32_t)(((uint32_t)s[0] << 8) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 24)) >> 8;
      else v = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24)) >> 8;
      const int8_t slot = p->wav.slot[c];
      if (slot >= 0) p->pcm[slot][p->fill] = v;
    }
    p->buf_pos += fb;
    p->frames_read++;
    if (++p->fill == PL_FRAME) {
      p->fill = 0;
      if (!send_frame(p)) return false;
    }
  }
  return true;
}

static void status_update(player_t *p) {
  const int lm = (int)(lead_ns(p) / 1000000LL);
  taskENTER_CRITICAL(&s_mux);
  s_st.lead_ms = lm;
  {
    /* what the speakers play now: sent minus the lead, never before 0 */
    const double pos = p->playing
        ? (double)(int64_t)((uint32_t)(p->rtp - p->rtp0)) / p->rate - (double)lm / 1000.0
        : 0;
    s_st.position_s = pos > 0 ? pos : 0;
  }
  for (int k = 0; k < PL_LANES; ++k) s_st.lane_bytes[k] = p->lane[k].bytes;
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
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "PLAYER %s seq %" PRIu32 " lead %d ms, %.1f s of %.1f s | %u frames "
             "(%.2fx real time) | read %lld ms (%llu KiB), encode %lld ms, send %lld ms",
             state_name(s_st.state), p->seq, lm, s_st.position_s, s_st.duration_s,
             (unsigned)p->frames_sent,
             (double)p->frames_sent * PL_FRAME / p->rate / (PL_STATUS_LOG_MS / 1000.0),
             (long long)(p->t_read_us / 1000), (unsigned long long)(p->read_bytes / 1024U),
             (long long)(p->t_enc_us / 1000), (long long)(p->t_send_us / 1000));
    p->t_read_us = p->t_enc_us = p->t_send_us = 0;
    p->read_bytes = 0;
    p->frames_sent = 0;
  }
}

static void player_run(const char *url, int volume_db) {
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
  p->buf = heap_caps_malloc(PL_READ_CHUNK * 2U + 16U, MALLOC_CAP_SPIRAM);
  p->out = heap_caps_malloc(PL_FRAME * 3U + 64U, MALLOC_CAP_SPIRAM);
  p->pkt = NULL;
  p->wbuf = heap_caps_malloc(PL_WBUF, MALLOC_CAP_SPIRAM);
  if (!p->buf || !p->out || !p->wbuf) {
    st_set(P_FAILED, "Out of memory");
    goto out;
  }
  st_set(P_STARTING, "Opening %s", url);
  if (player_source_http_open(&p->src, url) != ESP_OK) {
    st_set(P_FAILED, "The file is not reachable");
    goto out;
  }
  if (!open_wav(p)) goto out;
  if (!session_open(p, volume_db)) goto out;
  st_set(P_PLAYING, NULL);

  while (!atomic_load(&s_stop)) {
    if (p->playing && lead_ns(p) > p->max_lead_ns) {
      if (!flush_wbuf(p)) break;
      status_update(p);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (p->playing && !p->eof && lead_ns(p) < PL_MIN_LEAD_NS) {
      if (!flush_wbuf(p)) break;
      sound_engine_pause();
      p->playing = false;
      p->need_anchor = true;
      st_set(P_BUFFERING, "The file comes too slowly: buffering");
    }
    fill_buf(p);
    if (!take_pcm(p)) break;
    if (p->eof && p->buf_len - p->buf_pos < (size_t)p->wav.channels * p->wav.container_bits / 8U) {
      if (p->fill) { /* the last frame, filled with silence */
        for (int c = 0; c < 8; ++c)
          memset(&p->pcm[c][p->fill], 0, sizeof(int32_t) * (PL_FRAME - p->fill));
        p->fill = 0;
        if (!send_frame(p)) break;
      }
      if (!flush_wbuf(p)) break;
      st_set(P_FINISHING, "End of the file: the speakers play what they hold");
      while (!atomic_load(&s_stop) && lead_ns(p) > -200000000LL) {
        status_update(p);
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      break;
    }
    if (p->playing && s_st.state != P_PLAYING) st_set(P_PLAYING, NULL);
    status_update(p);
  }

out:
  if (p) {
    if (s_st.state != P_FAILED) st_set(P_STOPPING, "Stopping");
    if (p->src.close) p->src.close(&p->src);
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
  char url[sizeof(s_next_url)];
  int vol;
  taskENTER_CRITICAL(&s_mux);
  s_task = xTaskGetCurrentTaskHandle();
  strlcpy(url, s_next_url, sizeof(url));
  vol = s_next_volume_db;
  taskEXIT_CRITICAL(&s_mux);
  player_run(url, vol);
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
  return s_ctl && s_done;
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

esp_err_t gw_player_play(const char *url, int volume_db) {
  if (!url || strncmp(url, "http://", 7) != 0) return ESP_ERR_INVALID_ARG;
  if (strlen(url) >= sizeof(s_next_url)) return ESP_ERR_INVALID_SIZE;
  if (!init_once()) return ESP_ERR_NO_MEM;
  if (gw_calib_running()) return ESP_ERR_INVALID_STATE;
  volume_db = clamp_db(volume_db);
  xSemaphoreTake(s_ctl, portMAX_DELAY);
  stop_locked();
  (void)xSemaphoreTake(s_done, 0);
  taskENTER_CRITICAL(&s_mux);
  memset(&s_st, 0, sizeof(s_st));
  s_st.state = P_STARTING;
  strlcpy(s_st.url, url, sizeof(s_st.url));
  s_st.volume_db = volume_db;
  strlcpy(s_next_url, url, sizeof(s_next_url));
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
  ESP_LOGI(TAG, "play %s at %d dB", url, volume_db);
  return ESP_OK;
}

/* ---- web ---- */

static esp_err_t status_handler(httpd_req_t *req) {
  player_status_t st;
  taskENTER_CRITICAL(&s_mux);
  st = s_st;
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
  cJSON *perf = cJSON_AddObjectToObject(root, "perf_10s");
  if (perf) {
    cJSON_AddNumberToObject(perf, "speed", st.perf_speed);
    cJSON_AddNumberToObject(perf, "read_ms", st.perf_read_ms);
    cJSON_AddNumberToObject(perf, "read_kib", st.perf_read_kib);
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
  char body[512];
  const int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
  int got = 0;
  while (got < len) {
    const int r = httpd_req_recv(req, body + got, len - got);
    if (r <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    got += r;
  }
  body[got] = '\0';
  cJSON *j = cJSON_Parse(body);
  if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not JSON");
  const cJSON *cmd = cJSON_GetObjectItem(j, "cmd");
  const cJSON *url = cJSON_GetObjectItem(j, "url");
  const cJSON *vol = cJSON_GetObjectItem(j, "vol");
  esp_err_t err = ESP_ERR_INVALID_ARG;
  if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "play") == 0 && cJSON_IsString(url)) {
    err = gw_player_play(url->valuestring, cJSON_IsNumber(vol) ? vol->valueint : -30);
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "stop") == 0) {
    gw_player_stop();
    err = ESP_OK;
  } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "volume") == 0 && cJSON_IsNumber(vol)) {
    gw_player_set_volume(vol->valueint);
    err = ESP_OK;
  }
  cJSON_Delete(j);
  if (err != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
  return status_handler(req);
}

void gw_player_register_web(httpd_handle_t server) {
  const httpd_uri_t uris[] = {
      {.uri = "/api/player", .method = HTTP_GET, .handler = status_handler},
      {.uri = "/api/player", .method = HTTP_POST, .handler = control_handler},
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
    const esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
    if (err != ESP_OK)
      ESP_LOGE(TAG, "web handler %s not registered: %s (max_uri_handlers?)", uris[i].uri,
               esp_err_to_name(err));
  }
}
