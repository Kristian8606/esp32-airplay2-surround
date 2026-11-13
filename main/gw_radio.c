/* Web radio on the Gateway. See gw_radio.h.
 *
 * One task does it all: it hands the speakers over from AirPlay, reads the
 * station, decodes it, encodes ALAC frames of 352 samples, seals each one
 * like an AirPlay buffered packet and writes it to the Gateway's own relay
 * over a loopback TCP connection, exactly where an Apple sender writes.
 *
 * Timing: the radio's clock line is the Gateway's esp_timer clock itself
 * (time = local ns, rate 0) under a clock id of its own. The anchor puts the
 * first frame START_DELAY ahead. Like an Apple buffered sender, the task
 * then sends as far ahead as it can get: everything the station gives (its
 * burst on connect, any catching up later) up to MAX_LEAD ahead of the play
 * position, so each speaker holds that much and rides out a Wi-Fi dropout of
 * its own. The station's pace then keeps the lead. If the station stalls
 * until the lead falls under MIN_LEAD, the speakers pause and the next frame
 * gets a new anchor, START_DELAY ahead again. The song title arrives with
 * the audio it belongs to and is shown when that audio plays. */

#include "gw_radio.h"

#include <ctype.h>
#include <math.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_calib.h"
#include "esp_aac_dec.h"
#include "esp_alac_enc.h"
#include "esp_mp3_dec.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "rtsp_server.h"
#include "sodium.h"
#include "sound_engine.h"
#include "web_server_internal.h"
#include "airplay_audio_formats.h"

static const char *TAG = "gw_radio";

#define RADIO_TASK_STACK    16384
#define RADIO_TASK_PRIORITY 3
#define RADIO_TASK_CORE     0
#define RADIO_FRAME         352U   /* samples per ALAC frame, like AirPlay */
#define RADIO_HTTP_BUF      4096
#define RADIO_HTTP_TIMEOUT_MS 4000
#define RADIO_START_DELAY_NS  2000000000LL /* anchor ahead of now */
/* Read the station while the frames sent are less than this ahead of the
 * play position. 20 s of ALAC is 2.5 MB at 44.1 kHz / 16 bit and 3.8 MB at
 * 48 kHz / 24 bit: within the speakers' 6 MiB buffer and the relay's 4 MiB. */
#define RADIO_MAX_LEAD_NS     20000000000LL
#define RADIO_MIN_LEAD_NS     250000000LL  /* under this: pause, re-anchor */
#define RADIO_SENDER_STOP_MS  8000  /* the AirPlay session ends in this */
#define RADIO_STOP_WAIT_MS    6000
#define RADIO_RECONNECTS      5
#define RADIO_SEQ_MASK        0x007fffffU
#define RADIO_TAG_LEN         16U  /* Poly1305 */
#define RADIO_NONCE_TAIL      8U
#define RADIO_STATUS_LOG_MS   10000
#define RADIO_NVS_NS          "radio"
#define RADIO_FAVS_MAX        8192 /* favourites JSON, bytes */
#define RADIO_VOLUME_DEFAULT  (-30)

typedef enum {
  R_IDLE = 0,
  R_STARTING,
  R_PLAYING,
  R_BUFFERING,
  R_STOPPING,
  R_FAILED,
} radio_state_t;

static const char *state_name(radio_state_t st) {
  switch (st) {
    case R_STARTING: return "starting";
    case R_PLAYING: return "playing";
    case R_BUFFERING: return "buffering";
    case R_STOPPING: return "stopping";
    case R_FAILED: return "failed";
    default: return "idle";
  }
}

/* What the web page shows; guarded by s_mux. */
typedef struct {
  radio_state_t state;
  char url[256];
  char name[64];    /* the station's own (icy-name) */
  char label[64];   /* the name the page gave */
  char logo[256];
  char title[128];
  char message[96];
  char codec[8];
  int src_rate, src_channels, bitrate_kbps;
  int out_rate, out_bits;
  int volume_db;
  int lead_ms;
  uint32_t rebuffers;
  uint32_t enc_errors; /* ALAC frames lost (8 ms each) */
} radio_status_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static radio_status_t s_st;
static SemaphoreHandle_t s_ctl;   /* start/stop, one at a time */
static SemaphoreHandle_t s_done;  /* given by the task when it ends */
static TaskHandle_t s_task;
static atomic_bool s_running;   /* a radio task exists */
static atomic_bool s_stop;
static atomic_bool s_owns;        /* the radio holds the speakers */
static apple_clock_line_t s_line; /* valid while s_owns */
static int64_t s_owns_since_us;   /* the speakers' TIME follows s_line */
static uint16_t s_epoch = 0x5200;
static char s_next_url[256];
static int s_next_volume_db;

/* ---- status ---- */

static void st_set(radio_state_t state, const char *fmt, ...) {
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

/* ---- the radio's clock line ---- */

static uint64_t radio_clock_id(void) {
  uint8_t mac[6] = {0};
  (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
  /* Like an EUI-64 from the MAC, with "RD" in the middle: never the id of an
   * Apple sender nor the Gateway's own PTP identity (ff:fe there). */
  return ((uint64_t)mac[0] << 56) | ((uint64_t)mac[1] << 48) |
         ((uint64_t)mac[2] << 40) | (0x52ULL << 32) | (0x44ULL << 24) |
         ((uint64_t)mac[3] << 16) | ((uint64_t)mac[4] << 8) | mac[5];
}

static int64_t now_ns(void) { return esp_timer_get_time() * 1000LL; }

bool gw_radio_clock_line(apple_clock_line_t *out) {
  if (!atomic_load(&s_owns)) return false;
  taskENTER_CRITICAL(&s_mux);
  *out = s_line;
  taskEXIT_CRITICAL(&s_mux);
  return true;
}

bool gw_radio_active(void) { return atomic_load(&s_owns); }

void gw_radio_yield(void) {
  if (!atomic_load(&s_owns)) return;
  if (s_task && xTaskGetCurrentTaskHandle() == s_task) return;
  ESP_LOGI(TAG, "an AirPlay sender takes the speakers: the radio stops");
  gw_radio_stop();
}

/* ---- ICY metadata (StreamTitle between audio blocks) ---- */

typedef struct {
  int metaint;      /* audio bytes between metadata blocks, 0 = none */
  int audio_left;   /* audio bytes until the next length byte */
  int meta_left;    /* metadata bytes still to read */
  int meta_len;
  char meta[4081];  /* 255 * 16 + 1 */
  bool has_title;   /* a StreamTitle came; title holds it */
  char title[128];
} icy_t;

/* The StreamTitle of a metadata block, for the audio after it. */
static void icy_title(icy_t *icy) {
  icy->meta[icy->meta_len] = '\0';
  const char *p = strstr(icy->meta, "StreamTitle='");
  if (!p) return;
  p += 13;
  const char *e = strstr(p, "';");
  if (!e) e = p + strlen(p);
  size_t n = (size_t)(e - p);
  if (n >= sizeof(icy->title)) n = sizeof(icy->title) - 1;
  memcpy(icy->title, p, n);
  icy->title[n] = '\0';
  icy->has_title = true;
}

static void title_show(const char *t) {
  taskENTER_CRITICAL(&s_mux);
  const bool changed = strcmp(s_st.title, t) != 0;
  if (changed) strlcpy(s_st.title, t, sizeof(s_st.title));
  taskEXIT_CRITICAL(&s_mux);
  if (changed) ESP_LOGI(TAG, "now: %s", t);
}

/* Removes the metadata from buf in place; returns the audio length. */
static int icy_strip(icy_t *icy, uint8_t *buf, int len) {
  if (icy->metaint <= 0) return len;
  int out = 0;
  for (int i = 0; i < len;) {
    if (icy->meta_left > 0) {
      int n = len - i < icy->meta_left ? len - i : icy->meta_left;
      memcpy(icy->meta + icy->meta_len, buf + i, (size_t)n);
      icy->meta_len += n;
      icy->meta_left -= n;
      i += n;
      if (icy->meta_left == 0) {
        icy_title(icy);
        icy->audio_left = icy->metaint;
      }
    } else if (icy->audio_left > 0) {
      int n = len - i < icy->audio_left ? len - i : icy->audio_left;
      memmove(buf + out, buf + i, (size_t)n);
      out += n;
      icy->audio_left -= n;
      i += n;
    } else {
      icy->meta_len = 0;
      icy->meta_left = buf[i++] * 16;
      if (icy->meta_left == 0) icy->audio_left = icy->metaint;
    }
  }
  return out;
}

/* ---- the station ---- */

typedef struct {
  esp_http_client_handle_t http;
  icy_t *icy;
  esp_audio_simple_dec_type_t dec_type;
  char content_type[48];
} station_t;

static esp_err_t http_event(esp_http_client_event_t *e) {
  station_t *st = (station_t *)e->user_data;
  if (e->event_id != HTTP_EVENT_ON_HEADER || !st) return ESP_OK;
  if (strcasecmp(e->header_key, "icy-metaint") == 0) {
    st->icy->metaint = atoi(e->header_value);
  } else if (strcasecmp(e->header_key, "content-type") == 0) {
    strlcpy(st->content_type, e->header_value, sizeof(st->content_type));
  } else if (strcasecmp(e->header_key, "icy-name") == 0) {
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_st.name, e->header_value, sizeof(s_st.name));
    taskEXIT_CRITICAL(&s_mux);
  } else if (strcasecmp(e->header_key, "icy-br") == 0) {
    taskENTER_CRITICAL(&s_mux);
    s_st.bitrate_kbps = atoi(e->header_value);
    taskEXIT_CRITICAL(&s_mux);
  }
  return ESP_OK;
}

static void station_close(station_t *st) {
  if (st->http) {
    esp_http_client_close(st->http);
    esp_http_client_cleanup(st->http);
    st->http = NULL;
  }
}

static esp_audio_simple_dec_type_t dec_type_for(const char *ctype,
                                                const char *url) {
  char t[48];
  size_t i = 0;
  for (; ctype[i] && i + 1 < sizeof(t); ++i) t[i] = (char)tolower((unsigned char)ctype[i]);
  t[i] = '\0';
  if (strstr(t, "aac")) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
  if (strstr(t, "mpeg") || strstr(t, "mp3")) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
  if (strstr(url, ".aac")) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

/* Opens the station, following redirects. */
static esp_err_t station_open(station_t *st, const char *url) {
  station_close(st);
  memset(st->icy, 0, sizeof(*st->icy));
  st->content_type[0] = '\0';
  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = RADIO_HTTP_TIMEOUT_MS,
      .buffer_size = RADIO_HTTP_BUF,
      .event_handler = http_event,
      .user_data = st,
      .user_agent = "ESP32-Surround-Gateway",
      .crt_bundle_attach = esp_crt_bundle_attach,
      .max_redirection_count = 5,
      .keep_alive_enable = false,
  };
  st->http = esp_http_client_init(&cfg);
  if (!st->http) return ESP_ERR_NO_MEM;
  esp_http_client_set_header(st->http, "Icy-MetaData", "1");
  for (int hop = 0; hop < 6; ++hop) {
    if (atomic_load(&s_stop)) return ESP_FAIL;
    esp_err_t err = esp_http_client_open(st->http, 0);
    if (err != ESP_OK) {
      st_set(R_STARTING, "Station not reachable (%s)", esp_err_to_name(err));
      return err;
    }
    (void)esp_http_client_fetch_headers(st->http);
    const int code = esp_http_client_get_status_code(st->http);
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      esp_http_client_set_redirection(st->http);
      esp_http_client_close(st->http);
      memset(st->icy, 0, sizeof(*st->icy));
      continue;
    }
    if (code != 200) {
      st_set(R_STARTING, "Station answered HTTP %d", code);
      return ESP_FAIL;
    }
    st->icy->audio_left = st->icy->metaint;
    st->dec_type = dec_type_for(st->content_type, url);
    ESP_LOGI(TAG, "station open: %s, %s, metadata every %d bytes",
             st->content_type[0] ? st->content_type : "no content type",
             st->dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? "AAC" : "MP3",
             st->icy->metaint);
    return ESP_OK;
  }
  st_set(R_STARTING, "Too many redirects");
  return ESP_FAIL;
}

/* ---- the stream to the speakers ---- */

typedef struct {
  /* ALAC */
  void *enc;
  int rate, bits;
  int in_size, out_size;
  uint8_t *in;      /* one frame of PCM in the encoder's layout */
  uint8_t *out;     /* one encoded frame */
  int16_t pcm[RADIO_FRAME * 2]; /* stereo frame being filled */
  uint32_t fill;    /* samples per channel in pcm */
  /* packets */
  uint8_t key[32];
  uint64_t nonce;
  uint32_t seq, rtp;
  uint32_t ssrc;
  uint8_t *pkt;
  int sock;
  bool session;     /* the sound_engine session is set up */
  bool playing;     /* anchored and playing */
  bool need_anchor; /* the next frame gets a new anchor */
  uint64_t clock_id;
  int64_t anchor_ns; /* local time of anchor_rtp */
  uint32_t anchor_rtp;
  int64_t log_us;
} stream_t;

static int64_t lead_ns(const stream_t *o) {
  if (!o->playing) return RADIO_START_DELAY_NS;
  const int64_t sent_ns =
      (int64_t)(uint32_t)(o->rtp - o->anchor_rtp) * 1000000000LL / o->rate;
  return o->anchor_ns + sent_ns - now_ns();
}

/* The RTP time playing now; false while nothing is anchored. */
static bool play_rtp(const stream_t *o, uint32_t *rtp) {
  if (!o->playing) return false;
  const int64_t d = now_ns() - o->anchor_ns;
  *rtp = d < 0 ? o->anchor_rtp - 1U
               : o->anchor_rtp + (uint32_t)(d * o->rate / 1000000000LL);
  return true;
}

/* Song titles waiting for their audio to play (the lead is up to 20 s). */
#define RADIO_TITLES 4
typedef struct {
  struct {
    uint32_t rtp;
    char t[128];
  } q[RADIO_TITLES];
  int n;
} titles_t;

static void titles_pop(titles_t *tl) {
  title_show(tl->q[0].t);
  memmove(&tl->q[0], &tl->q[1], sizeof(tl->q[0]) * (size_t)(tl->n - 1));
  tl->n--;
}

static void titles_push(titles_t *tl, const stream_t *o, const char *t) {
  if (!o->session) {
    title_show(t); /* nothing plays yet: the title is the station's now */
    return;
  }
  if (tl->n == RADIO_TITLES) titles_pop(tl);
  /* The audio after the metadata starts after what is decoded so far. */
  tl->q[tl->n].rtp = o->rtp + o->fill;
  strlcpy(tl->q[tl->n].t, t, sizeof(tl->q[0].t));
  tl->n++;
}

static void titles_due(titles_t *tl, const stream_t *o) {
  uint32_t now = 0;
  if (!tl->n || !play_rtp(o, &now)) return;
  while (tl->n && (int32_t)(now - tl->q[0].rtp) >= 0) titles_pop(tl);
}

static bool send_all(int sock, const uint8_t *p, size_t n) {
  while (n > 0) {
    if (atomic_load(&s_stop)) return false;
    const int w = send(sock, p, n, 0);
    if (w > 0) {
      p += w;
      n -= (size_t)w;
    } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue; /* the relay's buffer is full: it reads again soon */
    } else {
      ESP_LOGW(TAG, "relay connection lost (errno %d)", errno);
      return false;
    }
  }
  return true;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void anchor_now(stream_t *o) {
  o->anchor_ns = now_ns() + RADIO_START_DELAY_NS;
  o->anchor_rtp = o->rtp;
  /* time = local ns on the radio's line */
  sound_engine_set_anchor_time(o->clock_id, (uint64_t)o->anchor_ns, o->anchor_rtp);
  sound_engine_set_playing(true);
  o->playing = true;
  o->need_anchor = false;
}

/* Encode, seal and send the full frame in o->pcm. */
static bool send_frame(stream_t *o) {
  const size_t n = RADIO_FRAME * 2U;
  if (o->bits == 16) {
    memcpy(o->in, o->pcm, n * sizeof(int16_t));
  } else { /* 24 bit, packed little-endian: the 16-bit sample, 8 bits up */
    uint8_t *d = o->in;
    for (size_t i = 0; i < n; ++i) {
      const uint16_t v = (uint16_t)o->pcm[i];
      *d++ = 0;
      *d++ = (uint8_t)v;
      *d++ = (uint8_t)(v >> 8);
    }
  }
  esp_audio_enc_in_frame_t in = {.buffer = o->in, .len = (uint32_t)o->in_size};
  esp_audio_enc_out_frame_t out = {.buffer = o->out, .len = (uint32_t)o->out_size};
  if (esp_alac_enc_process(o->enc, &in, &out) != ESP_AUDIO_ERR_OK ||
      out.encoded_bytes == 0) {
    /* The frame is left out: the next one takes its RTP time, so every
     * speaker skips the same 8 ms and the lead is 8 ms shorter. */
    uint32_t errs;
    taskENTER_CRITICAL(&s_mux);
    errs = ++s_st.enc_errors;
    taskEXIT_CRITICAL(&s_mux);
    if ((errs & 0x3fU) == 1U) ESP_LOGW(TAG, "ALAC encode failed (%" PRIu32 " frames so far)", errs);
    return true;
  }
  if (o->need_anchor) anchor_now(o);

  /* [2-byte length][12-byte RTP header][sealed ALAC][16-byte tag][8-byte nonce] */
  const size_t wire = 2U + 12U + out.encoded_bytes + RADIO_TAG_LEN + RADIO_NONCE_TAIL;
  uint8_t *p = o->pkt;
  p[0] = (uint8_t)(wire >> 8);
  p[1] = (uint8_t)wire;
  uint8_t *h = p + 2;
  put_be32(h, 0x80000000U | (o->seq & RADIO_SEQ_MASK));
  put_be32(h + 4, o->rtp);
  put_be32(h + 8, o->ssrc);
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &o->nonce, 8);
  o->nonce++;
  unsigned long long clen = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(h + 12, &clen, o->out,
                                            out.encoded_bytes, h + 4, 8, NULL,
                                            nonce, o->key);
  memcpy(h + 12 + clen, nonce + 4, RADIO_NONCE_TAIL);
  o->seq = (o->seq + 1U) & RADIO_SEQ_MASK;
  o->rtp += RADIO_FRAME;
  return send_all(o->sock, p, wire);
}

static int32_t db_to_q15(int db) {
  return (int32_t)lroundf(32768.0f * powf(10.0f, (float)db / 20.0f));
}

/* Sets the speakers up exactly like an Apple buffered sender does. */
static bool session_open(stream_t *o, int src_rate, int volume_db) {
  o->rate = src_rate == 48000 ? 48000 : src_rate;
  o->bits = o->rate == 48000 ? 24 : 16;
  const airplay_audio_format_t *f = airplay_audio_format_resolve(
      SOUND_STREAM_BUFFERED, 0, 2, o->rate, RADIO_FRAME, o->bits, 2);
  if (!f) {
    st_set(R_FAILED, "The speakers do not take %d Hz", src_rate);
    return false;
  }
  o->ssrc = f->ssrc;

  esp_alac_enc_config_t ec = ESP_ALAC_ENC_CONFIG_DEFAULT();
  ec.sample_rate = o->rate;
  ec.channel = 2;
  ec.bits_per_sample = (uint8_t)o->bits;
  ec.fast_mode = true;
  ec.frame_samples = RADIO_FRAME;
  if (esp_alac_enc_open(&ec, sizeof(ec), &o->enc) != ESP_AUDIO_ERR_OK) {
    st_set(R_FAILED, "ALAC encoder not available");
    return false;
  }
  esp_alac_enc_get_frame_size(o->enc, &o->in_size, &o->out_size);
  o->in = heap_caps_malloc((size_t)o->in_size, MALLOC_CAP_SPIRAM);
  o->out = heap_caps_malloc((size_t)o->out_size, MALLOC_CAP_SPIRAM);
  o->pkt = heap_caps_malloc((size_t)o->out_size + 64U, MALLOC_CAP_SPIRAM);
  if (!o->in || !o->out || !o->pkt) {
    st_set(R_FAILED, "Out of memory");
    return false;
  }
  esp_audio_enc_info_t ei = {0};
  if (esp_alac_enc_get_info(o->enc, &ei) == ESP_AUDIO_ERR_OK &&
      ei.codec_spec_info && ei.spec_info_len >= 24) {
    /* The speakers' decoder assumes the reference pb/mb/kb 40/10/14. */
    const uint8_t *c = ei.codec_spec_info + (ei.spec_info_len - 24);
    ESP_LOGI(TAG, "ALAC cookie: frame %" PRIu32 " bits %u pb/mb/kb %u/%u/%u ch %u",
             ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) |
                 ((uint32_t)c[2] << 8) | c[3],
             c[5], c[6], c[7], c[8], c[9]);
  }

  esp_fill_random(o->key, sizeof(o->key));
  o->seq = esp_random() & RADIO_SEQ_MASK;
  o->rtp = esp_random();
  o->clock_id = s_line.grandmaster_clock_id;
  /* The speakers take the anchor once a few TIME messages (every 125 ms)
   * carried the new line. */
  while (esp_timer_get_time() - s_owns_since_us < 1000000LL && !atomic_load(&s_stop))
    vTaskDelay(pdMS_TO_TICKS(50));

  sound_format_t fmt = {.sample_rate = o->rate, .channels = 2,
                        .bits_per_sample = o->bits, .frame_size = RADIO_FRAME};
  strlcpy(fmt.codec, "ALAC", sizeof(fmt.codec));
  sound_cipher_t ciph = {.type = SOUND_CIPHER_CHACHA20_POLY1305,
                         .key_len = sizeof(o->key)};
  memcpy(ciph.key, o->key, sizeof(o->key));

  sound_engine_set_stream_type(SOUND_STREAM_BUFFERED);
  sound_engine_set_format(&fmt);
  sound_engine_set_encryption(&ciph);
  memset(&ciph, 0, sizeof(ciph));
  if (sound_engine_start_buffered(0) != ESP_OK) {
    st_set(R_FAILED, "The relay did not start");
    return false;
  }
  o->session = true;
  sound_engine_set_playout_latency_samples(0);
  const uint16_t port = sound_engine_get_buffered_port();

  o->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
                          .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (o->sock < 0 || connect(o->sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
    st_set(R_FAILED, "No connection to the relay (errno %d)", errno);
    return false;
  }
  const int one = 1;
  setsockopt(o->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  const struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(o->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sound_engine_set_volume_q15(db_to_q15(volume_db));
  o->need_anchor = true;

  taskENTER_CRITICAL(&s_mux);
  s_st.out_rate = o->rate;
  s_st.out_bits = o->bits;
  taskEXIT_CRITICAL(&s_mux);
  ESP_LOGI(TAG, "stream to the speakers: ALAC %d Hz / %d bit / stereo, "
           "relay port %u, volume %d dB", o->rate, o->bits, (unsigned)port,
           volume_db);
  return true;
}

static void session_close(stream_t *o) {
  if (o->sock >= 0) {
    shutdown(o->sock, SHUT_RDWR);
    close(o->sock);
    o->sock = -1;
  }
  if (o->session) {
    sound_engine_stop(); /* STOP to the speakers, the relay closes */
    o->session = false;
  }
  if (o->enc) esp_alac_enc_close(o->enc);
  o->enc = NULL;
  free(o->in);
  free(o->out);
  free(o->pkt);
  o->in = o->out = o->pkt = NULL;
}

/* Decoded PCM into ALAC frames. */
static bool take_pcm(stream_t *o, const int16_t *pcm, size_t frames, int ch) {
  for (size_t i = 0; i < frames; ++i) {
    const int16_t l = pcm[i * ch];
    const int16_t r = ch > 1 ? pcm[i * ch + 1] : l;
    o->pcm[o->fill * 2] = l;
    o->pcm[o->fill * 2 + 1] = r;
    if (++o->fill == RADIO_FRAME) {
      o->fill = 0;
      if (!send_frame(o)) return false;
    }
  }
  return true;
}

/* ---- hand-over from and back to AirPlay ---- */

static bool airplay_release(void) {
  st_set(R_STARTING, "Disconnecting the AirPlay sender");
  rtsp_server_stop();
  for (int i = 0; i < RADIO_SENDER_STOP_MS / 100; ++i) {
    if (rtsp_server_is_idle() && sound_engine_is_idle()) return true;
    if (atomic_load(&s_stop)) return false; /* stopped meanwhile */
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return rtsp_server_is_idle() && sound_engine_is_idle();
}

/* Starts the AirPlay listener the radio stopped. Only right after
 * rtsp_server_stop(): never while AirPlay takes the speakers back, when the
 * RTSP task holds its lock and waits in gw_radio_yield() for this task (the
 * RTSP calls here take that lock too). */
static bool airplay_listen(void) {
  const esp_err_t err = rtsp_server_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AirPlay listener did not start again: %s", esp_err_to_name(err));
  }
  return err == ESP_OK;
}

static void take_speakers(void) {
  apple_clock_line_t l = {
      .ready = true,
      .realtime = false,
      .source_clock_id = radio_clock_id(),
      .grandmaster_clock_id = radio_clock_id(),
      .epoch = s_epoch++,
      .base_local_ns = 0,
      .base_apple_ns = 0,
      .rate_ppb = 0,
      .ready_since_ns = 0, /* the Gateway's clock runs since its boot */
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

/* ---- the task ---- */

static void radio_run(const char *url, int volume_db) {
  if (!airplay_release()) {
    if (!atomic_load(&s_stop)) st_set(R_FAILED, "The AirPlay sender did not let go; try again");
    airplay_listen();
    return;
  }
  take_speakers();
  /* AirPlay can take the speakers back at any time. */
  const bool listening = airplay_listen();

  station_t st = {0};
  stream_t o = {.sock = -1};
  titles_t titles = {0};
  esp_audio_simple_dec_handle_t dec = NULL;
  uint8_t *in = heap_caps_malloc(RADIO_HTTP_BUF, MALLOC_CAP_SPIRAM);
  uint32_t pcm_cap = 4608 * 2 * 2;
  uint8_t *pcm = heap_caps_malloc(pcm_cap, MALLOC_CAP_SPIRAM);
  st.icy = heap_caps_calloc(1, sizeof(icy_t), MALLOC_CAP_SPIRAM);
  if (!in || !pcm || !st.icy) {
    st_set(R_FAILED, "Out of memory");
    goto out;
  }
  static bool codecs_registered;
  if (!codecs_registered) {
    /* The decoders the stations need (only these get linked), and the
     * simple decoder's parsers that feed them. */
    esp_mp3_dec_register();
    esp_aac_dec_register();
    esp_audio_simple_dec_register_default();
    codecs_registered = true;
  }

  int reconnects = 0;
  st_set(R_STARTING, "Connecting to the station");
  if (station_open(&st, url) != ESP_OK) {
    taskENTER_CRITICAL(&s_mux);
    s_st.state = R_FAILED;
    taskEXIT_CRITICAL(&s_mux);
    goto out;
  }

  while (!atomic_load(&s_stop)) {
    if (!dec) {
      esp_audio_simple_dec_cfg_t dc = {.dec_type = st.dec_type};
      esp_aac_dec_cfg_t ac = ESP_AAC_DEC_CONFIG_DEFAULT();
      ac.aac_plus_enable = true; /* HE-AAC (AAC+) stations at their full rate */
      if (st.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
        dc.dec_cfg = &ac;
        dc.cfg_size = sizeof(ac);
      }
      if (esp_audio_simple_dec_open(&dc, &dec) != ESP_AUDIO_ERR_OK) {
        st_set(R_FAILED, "No decoder for this stream");
        break;
      }
      taskENTER_CRITICAL(&s_mux);
      strlcpy(s_st.codec, st.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? "AAC" : "MP3",
              sizeof(s_st.codec));
      taskEXIT_CRITICAL(&s_mux);
    }

    titles_due(&titles, &o);
    /* Ahead as far as the station gives, up to MAX_LEAD. */
    if (o.session && lead_ns(&o) > RADIO_MAX_LEAD_NS) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (o.playing && lead_ns(&o) < RADIO_MIN_LEAD_NS) {
      sound_engine_pause();
      o.playing = false;
      o.need_anchor = true;
      taskENTER_CRITICAL(&s_mux);
      s_st.rebuffers++;
      taskEXIT_CRITICAL(&s_mux);
      st_set(R_BUFFERING, "The station is late: buffering");
    }

    int n = esp_http_client_read(st.http, (char *)in, RADIO_HTTP_BUF);
    if (n == -ESP_ERR_HTTP_EAGAIN) continue;
    if (n <= 0) {
      if (++reconnects > RADIO_RECONNECTS) {
        st_set(R_FAILED, "The station stopped sending");
        break;
      }
      ESP_LOGW(TAG, "station closed the stream: connecting again (%d)", reconnects);
      vTaskDelay(pdMS_TO_TICKS(1000));
      if (station_open(&st, url) != ESP_OK) continue;
      esp_audio_simple_dec_close(dec);
      dec = NULL;
      continue;
    }
    n = icy_strip(st.icy, in, n);
    if (st.icy->has_title) {
      st.icy->has_title = false;
      titles_push(&titles, &o, st.icy->title);
    }

    esp_audio_simple_dec_raw_t raw = {.buffer = in, .len = (uint32_t)n};
    while (raw.len > 0 && !atomic_load(&s_stop)) {
      esp_audio_simple_dec_out_t fr = {.buffer = pcm, .len = pcm_cap};
      const esp_audio_err_t r = esp_audio_simple_dec_process(dec, &raw, &fr);
      if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        uint8_t *p = heap_caps_realloc(pcm, fr.needed_size, MALLOC_CAP_SPIRAM);
        if (!p) { atomic_store(&s_stop, true); break; }
        pcm = p;
        pcm_cap = fr.needed_size;
        continue;
      }
      if (r != ESP_AUDIO_ERR_OK && r != ESP_AUDIO_ERR_DATA_LACK) {
        if (raw.consumed == 0) break;
      }
      if (fr.decoded_size > 0) {
        esp_audio_simple_dec_info_t info = {0};
        esp_audio_simple_dec_get_info(dec, &info);
        if (!o.session) {
          taskENTER_CRITICAL(&s_mux);
          s_st.src_rate = (int)info.sample_rate;
          s_st.src_channels = info.channel;
          if (!s_st.bitrate_kbps) s_st.bitrate_kbps = (int)(info.bitrate / 1000U);
          taskEXIT_CRITICAL(&s_mux);
          ESP_LOGI(TAG, "station: %" PRIu32 " Hz, %u ch, %u bit, %" PRIu32 " kbit/s",
                   info.sample_rate, info.channel, info.bits_per_sample,
                   info.bitrate / 1000U);
          if (info.bits_per_sample != 16 || info.channel < 1 ||
              !session_open(&o, (int)info.sample_rate, volume_db)) {
            if (info.bits_per_sample != 16) st_set(R_FAILED, "Unexpected PCM");
            atomic_store(&s_stop, true);
            break;
          }
          st_set(R_PLAYING, NULL);
        } else if ((int)info.sample_rate != o.rate) {
          st_set(R_FAILED, "The station changed its sample rate");
          atomic_store(&s_stop, true);
          break;
        }
        const size_t frames = fr.decoded_size / (2U * info.channel);
        if (!take_pcm(&o, (const int16_t *)pcm, frames, info.channel)) {
          atomic_store(&s_stop, true);
          break;
        }
        reconnects = 0;
        if (o.playing) {
          radio_state_t cur;
          taskENTER_CRITICAL(&s_mux);
          cur = s_st.state;
          taskEXIT_CRITICAL(&s_mux);
          if (cur != R_PLAYING) st_set(R_PLAYING, NULL);
        }
      }
      if (raw.consumed == 0 && fr.decoded_size == 0) break;
      raw.buffer += raw.consumed;
      raw.len -= raw.consumed;
    }

    const int64_t now_us = esp_timer_get_time();
    if (o.session) {
      const int lm = (int)(lead_ns(&o) / 1000000LL);
      taskENTER_CRITICAL(&s_mux);
      s_st.lead_ms = lm;
      taskEXIT_CRITICAL(&s_mux);
      if (now_us - o.log_us >= RADIO_STATUS_LOG_MS * 1000LL) {
        o.log_us = now_us;
        ESP_LOGI(TAG, "RADIO %s seq %" PRIu32 " rtp %" PRIu32 " lead %d ms",
                 o.playing ? "playing" : "buffering", o.seq, o.rtp, lm);
      }
    }
  }

out:
  {
    radio_state_t cur;
    taskENTER_CRITICAL(&s_mux);
    cur = s_st.state;
    taskEXIT_CRITICAL(&s_mux);
    if (cur != R_FAILED) st_set(R_STOPPING, "Stopping");
  }
  station_close(&st);
  if (dec) esp_audio_simple_dec_close(dec);
  session_close(&o);
  atomic_store(&s_owns, false); /* TIME follows the Apple clock again */
  if (!listening) (void)airplay_listen(); /* one more try, if the first failed */
  free(in);
  free(pcm);
  free(st.icy);
}

static void radio_task(void *arg) {
  (void)arg;
  char url[sizeof(s_next_url)];
  int vol;
  taskENTER_CRITICAL(&s_mux);
  s_task = xTaskGetCurrentTaskHandle();
  strlcpy(url, s_next_url, sizeof(url));
  vol = s_next_volume_db;
  taskEXIT_CRITICAL(&s_mux);
  radio_run(url, vol);
  taskENTER_CRITICAL(&s_mux);
  if (s_st.state != R_FAILED) {
    s_st.state = R_IDLE;
    s_st.message[0] = '\0';
  }
  s_task = NULL;
  taskEXIT_CRITICAL(&s_mux);
  atomic_store(&s_running, false);
  ESP_LOGI(TAG, "radio stopped");
  xSemaphoreGive(s_done);
  vTaskDelete(NULL);
}

/* ---- control ---- */

static bool init_once(void) {
  if (!s_ctl) s_ctl = xSemaphoreCreateMutex();
  if (!s_done) s_done = xSemaphoreCreateBinary();
  return s_ctl && s_done;
}

/* Returns once the radio task has ended: never two radio tasks, and AirPlay
 * gets the speakers only after the radio let go. Every wait in the task is
 * bounded and looks at s_stop, so this is a few seconds at most. */
static void stop_locked(void) {
  if (!atomic_load(&s_running)) return;
  atomic_store(&s_stop, true);
  int waited_ms = 0;
  while (atomic_load(&s_running)) {
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) == pdTRUE) continue;
    waited_ms += 500;
    if (waited_ms % RADIO_STOP_WAIT_MS == 0) {
      ESP_LOGW(TAG, "still waiting for the radio to stop (%d ms)", waited_ms);
    }
  }
  (void)xSemaphoreTake(s_done, 0);
}

void gw_radio_stop(void) {
  if (!init_once()) return;
  xSemaphoreTake(s_ctl, portMAX_DELAY);
  stop_locked();
  xSemaphoreGive(s_ctl);
}

/* ---- saved settings (NVS) ---- */

static int saved_volume(void) {
  nvs_handle_t h;
  int8_t v = RADIO_VOLUME_DEFAULT;
  if (nvs_open(RADIO_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    (void)nvs_get_i8(h, "vol", &v);
    nvs_close(h);
  }
  return v;
}

static void save_volume(int db) {
  nvs_handle_t h;
  if (nvs_open(RADIO_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  int8_t old = 0;
  if (nvs_get_i8(h, "vol", &old) != ESP_OK || old != db) {
    if (nvs_set_i8(h, "vol", (int8_t)db) == ESP_OK) (void)nvs_commit(h);
  }
  nvs_close(h);
}

static int clamp_db(int db) { return db > 0 ? 0 : db < -60 ? -60 : db; }

void gw_radio_set_volume(int volume_db, bool save) {
  volume_db = clamp_db(volume_db);
  taskENTER_CRITICAL(&s_mux);
  s_st.volume_db = volume_db;
  taskEXIT_CRITICAL(&s_mux);
  if (atomic_load(&s_owns)) sound_engine_set_volume_q15(db_to_q15(volume_db));
  if (save) save_volume(volume_db);
}

esp_err_t gw_radio_start(const char *url, const char *name, const char *logo,
                         int volume_db) {
  if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0))
    return ESP_ERR_INVALID_ARG;
  if (strlen(url) >= sizeof(s_next_url)) return ESP_ERR_INVALID_SIZE;
  if (!init_once()) return ESP_ERR_NO_MEM;
  if (gw_calib_running()) return ESP_ERR_INVALID_STATE;
  volume_db = volume_db == GW_RADIO_SAVED_VOLUME ? saved_volume() : clamp_db(volume_db);
  xSemaphoreTake(s_ctl, portMAX_DELAY);
  stop_locked();
  if (atomic_load(&s_running)) {
    xSemaphoreGive(s_ctl);
    return ESP_ERR_INVALID_STATE;
  }
  (void)xSemaphoreTake(s_done, 0); /* a task that ended by itself gave it */
  taskENTER_CRITICAL(&s_mux);
  memset(&s_st, 0, sizeof(s_st));
  s_st.state = R_STARTING;
  strlcpy(s_st.url, url, sizeof(s_st.url));
  if (name) strlcpy(s_st.label, name, sizeof(s_st.label));
  if (logo) strlcpy(s_st.logo, logo, sizeof(s_st.logo));
  s_st.volume_db = volume_db;
  strlcpy(s_next_url, url, sizeof(s_next_url));
  s_next_volume_db = volume_db;
  taskEXIT_CRITICAL(&s_mux);
  atomic_store(&s_stop, false);
  atomic_store(&s_running, true);
  TaskHandle_t t = NULL;
  const BaseType_t ok = xTaskCreatePinnedToCore(radio_task, "gw_radio", RADIO_TASK_STACK,
                                                NULL, RADIO_TASK_PRIORITY, &t,
                                                RADIO_TASK_CORE);
  if (ok != pdPASS) atomic_store(&s_running, false);
  xSemaphoreGive(s_ctl);
  if (ok != pdPASS) {
    st_set(R_FAILED, "Out of memory");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "radio start: %s at %d dB", url, volume_db);
  return ESP_OK;
}

/* ---- web ---- */

static esp_err_t send_status(httpd_req_t *req) {
  radio_status_t st;
  taskENTER_CRITICAL(&s_mux);
  st = s_st;
  taskEXIT_CRITICAL(&s_mux);
  cJSON *root = cJSON_CreateObject();
  if (!root) return httpd_resp_send_500(req);
  cJSON_AddStringToObject(root, "state", state_name(st.state));
  cJSON_AddBoolToObject(root, "active", gw_radio_active());
  cJSON_AddStringToObject(root, "url", st.url);
  cJSON_AddBoolToObject(root, "airplay", !gw_radio_active() && !sound_engine_is_idle());
  cJSON_AddStringToObject(root, "name", st.label[0] ? st.label : st.name);
  cJSON_AddStringToObject(root, "logo", st.logo);
  cJSON_AddStringToObject(root, "title", st.title);
  cJSON_AddStringToObject(root, "message", st.message);
  cJSON_AddStringToObject(root, "codec", st.codec);
  cJSON_AddNumberToObject(root, "bitrate_kbps", st.bitrate_kbps);
  cJSON_AddNumberToObject(root, "source_rate", st.src_rate);
  cJSON_AddNumberToObject(root, "source_channels", st.src_channels);
  cJSON_AddNumberToObject(root, "alac_rate", st.out_rate);
  cJSON_AddNumberToObject(root, "alac_bits", st.out_bits);
  cJSON_AddNumberToObject(root, "volume_db", st.volume_db);
  cJSON_AddNumberToObject(root, "lead_ms", st.lead_ms);
  cJSON_AddNumberToObject(root, "rebuffers", st.rebuffers);
  cJSON_AddNumberToObject(root, "encoder_errors", st.enc_errors);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t err = httpd_resp_sendstr(req, json);
  free(json);
  return err;
}

static esp_err_t status_handler(httpd_req_t *req) { return send_status(req); }

/* The page: POST {"url":..,"name":..,"logo":..,"vol":-30} (a URL may be
 * longer than a GET query can carry). */
static esp_err_t start_post_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  if (req->content_len == 0 || req->content_len > 2048) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body");
  }
  char *b = malloc(req->content_len + 1);
  if (!b) return httpd_resp_send_500(req);
  size_t got = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, b + got, req->content_len - got);
    if (r <= 0) {
      free(b);
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body");
    }
    got += (size_t)r;
  }
  b[got] = '\0';
  cJSON *j = cJSON_Parse(b);
  free(b);
  const cJSON *u = cJSON_GetObjectItem(j, "url");
  const cJSON *n = cJSON_GetObjectItem(j, "name");
  const cJSON *l = cJSON_GetObjectItem(j, "logo");
  const cJSON *v = cJSON_GetObjectItem(j, "vol");
  const esp_err_t err = gw_radio_start(cJSON_IsString(u) ? u->valuestring : NULL,
                                       cJSON_IsString(n) ? n->valuestring : NULL,
                                       cJSON_IsString(l) ? l->valuestring : NULL,
                                       cJSON_IsNumber(v) ? v->valueint : GW_RADIO_SAVED_VOLUME);
  cJSON_Delete(j);
  if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a station URL");
  }
  if (err == ESP_ERR_INVALID_STATE) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "The room calibration runs");
  }
  if (err != ESP_OK) return httpd_resp_send_500(req);
  return send_status(req);
}

static esp_err_t stop_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  gw_radio_stop();
  return send_status(req);
}

static esp_err_t volume_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  char q[48], v[8], live[4];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
      httpd_query_key_value(q, "db", v, sizeof(v)) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "Usage: POST /api/radio/volume?db=-30[&live=1]");
  }
  /* live=1: the slider is still moving; the last value comes without it. */
  const bool moving = httpd_query_key_value(q, "live", live, sizeof(live)) == ESP_OK &&
                      strcmp(live, "1") == 0;
  gw_radio_set_volume(atoi(v), !moving);
  return send_status(req);
}

/* Favourites: the page's JSON array, kept as it is. */
static esp_err_t favs_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  nvs_handle_t h;
  size_t len = 0;
  if (nvs_open(RADIO_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_blob(h, "favs", NULL, &len) == ESP_OK && len > 0 && len <= RADIO_FAVS_MAX) {
      char *b = malloc(len);
      if (b && nvs_get_blob(h, "favs", b, &len) == ESP_OK) {
        nvs_close(h);
        const esp_err_t err = httpd_resp_send(req, b, (ssize_t)len);
        free(b);
        return err;
      }
      free(b);
    }
    nvs_close(h);
  }
  return httpd_resp_sendstr(req, "[]");
}

static esp_err_t favs_post_handler(httpd_req_t *req) {
  if (!web_admin_ok(req)) return ESP_OK;
  if (req->content_len == 0 || req->content_len > RADIO_FAVS_MAX) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too many favourites");
  }
  char *b = malloc(req->content_len + 1);
  if (!b) return httpd_resp_send_500(req);
  size_t got = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, b + got, req->content_len - got);
    if (r <= 0) {
      free(b);
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body");
    }
    got += (size_t)r;
  }
  b[got] = '\0';
  cJSON *j = cJSON_Parse(b);
  const bool ok = j && cJSON_IsArray(j);
  cJSON_Delete(j);
  esp_err_t err = ESP_FAIL;
  nvs_handle_t h;
  if (ok && nvs_open(RADIO_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    err = nvs_set_blob(h, "favs", b, got);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
  }
  free(b);
  if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a JSON array");
  if (err != ESP_OK) return httpd_resp_send_500(req);
  return favs_get_handler(req);
}

extern const uint8_t web_radio_js_start[] asm("_binary_web_radio_js_start");
extern const uint8_t web_radio_js_end[] asm("_binary_web_radio_js_end");

static esp_err_t radio_js_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, (const char *)web_radio_js_start,
                         web_radio_js_end - web_radio_js_start);
}

void gw_radio_register_web(httpd_handle_t server) {
  (void)init_once();
  const httpd_uri_t uris[] = {
      {.uri = "/api/radio", .method = HTTP_GET, .handler = status_handler},
      {.uri = "/api/radio/start", .method = HTTP_POST, .handler = start_post_handler},
      {.uri = "/api/radio/stop", .method = HTTP_POST, .handler = stop_handler},
      {.uri = "/api/radio/volume", .method = HTTP_POST, .handler = volume_handler},
      {.uri = "/api/radio/favs", .method = HTTP_GET, .handler = favs_get_handler},
      {.uri = "/api/radio/favs", .method = HTTP_POST, .handler = favs_post_handler},
      {.uri = "/radio.js", .method = HTTP_GET, .handler = radio_js_handler},
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
  }
}
