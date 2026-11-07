/* SPDX-License-Identifier: Apache-2.0 */
#include "fakes.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amp_control.h"
#include "ap2_fairplay.h"
#include "ap2_hap.h"
#include "ap2_plist.h"
#include "ap2_tlv8.h"
#include "apple_clock.h"
#include "sound_engine.h"
#include "airplay_identity.h"
#include "rtsp_events.h"
#include "rtsp_port.h"

fake_t fake;

/* ---- Call log ---- */

#define MAX_CALLS 512
static char s_calls[MAX_CALLS][160];
static int s_call_count;

static void call(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void call(const char *format, ...) {
  va_list ap;
  if (s_call_count >= MAX_CALLS) return;
  va_start(ap, format);
  vsnprintf(s_calls[s_call_count++], sizeof(s_calls[0]), format, ap);
  va_end(ap);
}

void calls_clear(void) { s_call_count = 0; }
int calls_count(void) { return s_call_count; }
const char *calls_get(int index) {
  return (index >= 0 && index < s_call_count) ? s_calls[index] : "";
}
int calls_find_from(int from, const char *prefix) {
  for (int i = from < 0 ? 0 : from; i < s_call_count; i++) {
    if (strncmp(s_calls[i], prefix, strlen(prefix)) == 0) return i;
  }
  return -1;
}
int calls_find(const char *prefix) { return calls_find_from(0, prefix); }
int calls_with(const char *prefix) {
  int n = 0;
  for (int i = 0; i < s_call_count; i++) {
    if (strncmp(s_calls[i], prefix, strlen(prefix)) == 0) n++;
  }
  return n;
}
void calls_dump(void) {
  for (int i = 0; i < s_call_count; i++) fprintf(stderr, "  call %d: %s\n", i, s_calls[i]);
}

/* ---- Log ---- */

static int s_warnings;
static char s_log[64][200];
static int s_log_count;

void rtsp_host_log(char level, const char *format, ...) {
  va_list ap;
  char line[200];
  va_start(ap, format);
  vsnprintf(line, sizeof(line), format, ap);
  va_end(ap);
  if (level == 'W' || level == 'E') s_warnings++;
  if (s_log_count < 64) snprintf(s_log[s_log_count++], sizeof(s_log[0]), "%s", line);
  else {
    memmove(s_log[0], s_log[1], sizeof(s_log[0]) * 63);
    snprintf(s_log[63], sizeof(s_log[0]), "%s", line);
  }
  if (getenv("RTSP_TEST_LOG") != NULL) fprintf(stderr, "%c rtsp: %s\n", level, line);
}

int log_warnings(void) { return s_warnings; }
bool log_contains(const char *text) {
  for (int i = 0; i < s_log_count; i++) {
    if (strstr(s_log[i], text) != NULL) return true;
  }
  return false;
}
void log_clear(void) {
  s_warnings = 0;
  s_log_count = 0;
}

static void event_ports_reset(void);

void fake_reset(void) {
  memset(&fake, 0, sizeof(fake));
  event_ports_reset();
  fake.saved_volume = -15.0f;
  fake.device_name = "Gateway";
  fake.next_event_port = 50000;
  calls_clear();
  log_clear();
}

/* ---- rtsp_port.h ---- */

void *rtsp_port_malloc(size_t size) {
  if (fake.malloc_fail) return NULL;
  return malloc(size);
}
void rtsp_port_free(void *ptr) { free(ptr); }
void rtsp_port_lock(void) { fake.lock_depth++; }
void rtsp_port_unlock(void) { fake.lock_depth--; }
uint32_t rtsp_port_now_ms(void) { return fake.now_ms; }

esp_err_t rtsp_port_settings_get_volume(float *volume) {
  if (fake.saved_volume_missing) return ESP_FAIL;
  *volume = fake.saved_volume;
  return ESP_OK;
}
esp_err_t rtsp_port_settings_set_volume(float volume) {
  call("settings_set_volume(%.4f)", volume);
  fake.ram_volume = volume;
  fake.ram_volume_set = true;
  return ESP_OK;
}
esp_err_t rtsp_port_settings_persist_volume(void) {
  call("settings_persist_volume()");
  if (fake.ram_volume_set) {
    fake.saved_volume = fake.ram_volume;
    fake.saved_volume_missing = false;
  }
  return ESP_OK;
}
esp_err_t rtsp_port_settings_get_name(char *name, size_t len) {
  snprintf(name, len, "%s", fake.device_name);
  return ESP_OK;
}
void rtsp_port_device_mac(uint8_t mac[6]) {
  static const uint8_t m[6] = {0xa4, 0xcf, 0x12, 0x0b, 0xfe, 0x0d};
  memcpy(mac, m, 6);
}
/* The event ports: per control connection, its keys and whether it listens. */
static struct {
  const void *conn;
  uint8_t write_key[32];
  uint8_t read_key[32];
  uint16_t port;
  bool listening;
} event_ports[16];

static void event_ports_reset(void) { memset(event_ports, 0, sizeof(event_ports)); }

static int event_port_index(const void *conn, bool add) {
  for (int i = 0; i < 16; i++) {
    if (event_ports[i].conn == conn) return i;
  }
  for (int i = 0; add && i < 16; i++) {
    if (event_ports[i].conn == NULL) {
      event_ports[i].conn = conn;
      return i;
    }
  }
  return -1;
}

bool fake_event_port(const void *conn, uint8_t write_key[32], uint8_t read_key[32],
                     uint16_t *port) {
  int i = event_port_index(conn, false);
  if (i < 0 || !event_ports[i].listening) return false;
  if (write_key != NULL) memcpy(write_key, event_ports[i].write_key, 32);
  if (read_key != NULL) memcpy(read_key, event_ports[i].read_key, 32);
  if (port != NULL) *port = event_ports[i].port;
  return true;
}

esp_err_t rtsp_port_event_open(rtsp_conn_t *conn, const uint8_t write_key[32],
                               const uint8_t read_key[32], uint16_t *port) {
  int i;
  if (fake.event_open_err != ESP_OK) {
    call("event_open(%p) failed", (void *)conn);
    return fake.event_open_err;
  }
  i = event_port_index(conn, true);
  *port = fake.next_event_port++;
  call("event_open(%p) = %u", (void *)conn, *port);
  memcpy(fake.event_write_key, write_key, 32);
  memcpy(fake.event_read_key, read_key, 32);
  if (i >= 0) {
    memcpy(event_ports[i].write_key, write_key, 32);
    memcpy(event_ports[i].read_key, read_key, 32);
    event_ports[i].port = *port;
    event_ports[i].listening = true;
  }
  return ESP_OK;
}
void rtsp_port_event_close(rtsp_conn_t *conn) {
  int i = event_port_index(conn, false);
  call("event_close(%p)", (void *)conn);
  if (i >= 0) memset(&event_ports[i], 0, sizeof(event_ports[i]));
}
void rtsp_port_conn_close(rtsp_conn_t *conn) { call("conn_close(%p)", (void *)conn); }
esp_err_t rtsp_port_udp_pair(uint16_t *data_port, uint16_t *control_port) {
  if (fake.udp_pair_err != ESP_OK) return fake.udp_pair_err;
  *data_port = 6000;
  *control_port = 6001;
  return ESP_OK;
}

/* ---- Firmware ---- */

void apple_clock_select(uint32_t master_ip, bool realtime) {
  const uint8_t *b = (const uint8_t *)&master_ip;
  call("apple_clock_select(%u.%u.%u.%u,%d)", b[0], b[1], b[2], b[3], realtime);
}
void apple_clock_release(void) { call("apple_clock_release()"); }
void amp_control_session_connected(void) { call("amp_connected()"); }
void amp_control_session_disconnected(void) { call("amp_disconnected()"); }
void rtsp_events_emit(rtsp_event_t event) {
  static const char *names[] = {"CLIENT_CONNECTED", "PLAYING", "PAUSED", "DISCONNECTED"};
  call("event(%s)", names[event]);
}

bool sound_engine_is_idle(void) { return true; }
void sound_engine_set_stream_type(sound_stream_t type) {
  call("audio_set_stream_type(%d)", (int)type);
}
void sound_engine_set_format(const sound_format_t *f) {
  call("audio_set_format(%s,%d,%d,%d,%d)", f->codec, f->sample_rate, f->channels,
       f->bits_per_sample, f->frame_size);
}
void sound_engine_set_encryption(const sound_cipher_t *e) {
  call("audio_set_encryption(%d,%zu,%02x%02x)", (int)e->type, e->key_len, e->key[0],
       e->key[31]);
}
esp_err_t sound_engine_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port) {
  call("audio_start_stream(%u,%u,%u)", data_port, control_port, tcp_port);
  return fake.start_stream_err;
}
esp_err_t sound_engine_start_buffered(uint16_t tcp_port) {
  call("audio_start_buffered(%u)", tcp_port);
  return fake.start_buffered_err;
}
uint16_t sound_engine_get_buffered_port(void) { return 7100; }
void sound_engine_stop(void) { call("audio_stop()"); }
void sound_engine_set_volume_q15(int32_t q15) { call("audio_set_volume_q15(%" PRId32 ")", q15); }
void sound_engine_set_immediate_flush(uint32_t until_seq, uint32_t until_ts) {
  call("audio_immediate_flush(%" PRIu32 ",%" PRIu32 ")", until_seq, until_ts);
}
esp_err_t sound_engine_set_deferred_flush_range(uint32_t from_seq, uint32_t from_ts,
                                                  uint32_t until_seq, uint32_t until_ts,
                                                  bool timestamps_valid) {
  call("audio_deferred_flush(%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%d)",
       from_seq, from_ts, until_seq, until_ts, timestamps_valid);
  return ESP_OK;
}
void sound_engine_realtime_flush_to_rtp(uint32_t flush_rtp) {
  call("audio_realtime_flush(%" PRIu32 ")", flush_rtp);
}
void sound_engine_pause(void) { call("audio_pause()"); }
void sound_engine_set_playing(bool playing) { call("audio_set_playing(%d)", playing); }
void sound_engine_set_anchor_time(uint64_t clock_id, uint64_t ns, uint32_t rtp) {
  call("audio_anchor(%" PRIx64 ",%" PRIu64 ",%" PRIu32 ")", clock_id, ns, rtp);
}
void sound_engine_set_client_control(uint32_t client_ip, uint16_t port) {
  const uint8_t *b = (const uint8_t *)&client_ip;
  call("audio_client_control(%u.%u.%u.%u,%u)", b[0], b[1], b[2], b[3], port);
}
void sound_engine_set_playout_latency_samples(uint32_t samples) {
  call("audio_latency(%" PRIu32 ")", samples);
}

void airplay_get_pairing_id(char *out, size_t len) {
  snprintf(out, len, "%s", "0b9c2f4e-1d3a-4c5b-8e7f-6a5b4c3d2e1f");
}

/* ---- ap2_tlv8 (a real small implementation) ---- */

void ap2_tlv8_writer_init(ap2_tlv8_writer_t *w, uint8_t *buffer, size_t capacity) {
  w->buffer = buffer;
  w->size = 0;
  w->capacity = capacity;
}
bool ap2_tlv8_put(ap2_tlv8_writer_t *w, uint8_t type, const uint8_t *value, size_t len) {
  do {
    size_t n = len > 255 ? 255 : len;
    if (w->size + 2 + n > w->capacity) return false;
    w->buffer[w->size++] = type;
    w->buffer[w->size++] = (uint8_t)n;
    memcpy(w->buffer + w->size, value, n);
    w->size += n;
    value += n;
    len -= n;
  } while (len > 0);
  return true;
}
bool ap2_tlv8_put_byte(ap2_tlv8_writer_t *w, uint8_t type, uint8_t value) {
  return ap2_tlv8_put(w, type, &value, 1);
}
size_t ap2_tlv8_writer_size(const ap2_tlv8_writer_t *w) { return w->size; }
const uint8_t *ap2_tlv8_find(const uint8_t *data, size_t len, uint8_t type,
                             size_t *value_len) {
  size_t i = 0;
  if (data == NULL) return NULL;
  while (i + 2 <= len) {
    size_t n = data[i + 1];
    if (i + 2 + n > len) return NULL;
    if (data[i] == type) {
      *value_len = n;
      return data + i + 2;
    }
    i += 2 + n;
  }
  return NULL;
}
bool ap2_tlv8_get(const uint8_t *data, size_t len, uint8_t type, uint8_t *out,
                  size_t cap, size_t *out_len) {
  size_t n;
  const uint8_t *v = ap2_tlv8_find(data, len, type, &n);
  if (v == NULL || n > cap) return false;
  memcpy(out, v, n);
  *out_len = n;
  return true;
}

/* ---- ap2_hap ---- */

struct ap2_hap_session {
  int setup_state;
  int verify_state;
  bool transient;
  bool established;
  uint8_t secret[64];
  size_t secret_len;
  uint8_t channel_key[2][32]; /* AP2_HAP_SEND, AP2_HAP_RECEIVE */
  uint64_t channel_counter[2];
};

static void mix(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);

/* Pairing ended: the control channel's keys as the spec derives them, the
 * counters from fake.channel_counter_start (0 like the real core unless a
 * test moves it). */
static void establish(ap2_hap_session_t *s, bool transient) {
  static const char salt[] = "Control-Salt";
  static const char read[] = "Control-Read-Encryption-Key";
  static const char write[] = "Control-Write-Encryption-Key";
  s->established = true;
  s->secret_len = fake_pairing_secret(transient, s->secret);
  mix((const uint8_t *)salt, sizeof(salt) - 1, s->secret, s->secret_len,
      (const uint8_t *)read, sizeof(read) - 1, s->channel_key[AP2_HAP_SEND], 32);
  mix((const uint8_t *)salt, sizeof(salt) - 1, s->secret, s->secret_len,
      (const uint8_t *)write, sizeof(write) - 1, s->channel_key[AP2_HAP_RECEIVE], 32);
  s->channel_counter[0] = s->channel_counter[1] = fake.channel_counter_start;
}

static const uint8_t PUBLIC_KEY[32] = {0xED, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                       16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 0x25};

esp_err_t ap2_hap_identity_init(void) { return ESP_OK; }
const uint8_t *ap2_hap_identity_public_key(void) { return PUBLIC_KEY; }

ap2_hap_session_t *ap2_hap_session_new(void) {
  if (fake.malloc_fail || fake.hap_new_fail) return NULL;
  return calloc(1, sizeof(ap2_hap_session_t));
}
void ap2_hap_session_delete(ap2_hap_session_t *s) { free(s); }

size_t fake_pairing_secret(bool transient, uint8_t *out) {
  size_t n = transient ? 64 : 32;
  for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(i * 3 + (transient ? 1 : 7));
  return n;
}

static esp_err_t hap_reply(int state, uint8_t *out, size_t cap, size_t *out_len) {
  ap2_tlv8_writer_t w;
  ap2_tlv8_writer_init(&w, out, cap);
  if (fake.hap_mode == 2) return ESP_FAIL;
  ap2_tlv8_put_byte(&w, AP2_TLV_STATE, (uint8_t)state);
  if (fake.hap_mode == 1) {
    ap2_tlv8_put_byte(&w, AP2_TLV_ERROR, AP2_TLV_ERROR_AUTHENTICATION);
  } else {
    static const uint8_t pk[16] = {0x50};
    ap2_tlv8_put(&w, AP2_TLV_PUBLIC_KEY, pk, sizeof(pk));
  }
  *out_len = ap2_tlv8_writer_size(&w);
  return ESP_OK;
}

esp_err_t ap2_hap_setup_m1(ap2_hap_session_t *s, const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t cap, size_t *out_len) {
  size_t n;
  const uint8_t *flags = ap2_tlv8_find(in, in_len, AP2_TLV_FLAGS, &n);
  call("hap_setup_m1()");
  s->transient = flags != NULL && n == 1 && (flags[0] & AP2_TLV_FLAG_TRANSIENT);
  s->setup_state = 2;
  return hap_reply(2, out, cap, out_len);
}
esp_err_t ap2_hap_setup_m3(ap2_hap_session_t *s, const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t cap, size_t *out_len) {
  (void)in; (void)in_len;
  call("hap_setup_m3()");
  if (s->setup_state != 2) return ESP_FAIL;
  s->setup_state = 4;
  if (fake.hap_mode == 0 && s->transient) establish(s, true);
  return hap_reply(4, out, cap, out_len);
}
esp_err_t ap2_hap_setup_m5(ap2_hap_session_t *s, const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t cap, size_t *out_len) {
  (void)in; (void)in_len;
  call("hap_setup_m5()");
  s->setup_state = 6;
  return hap_reply(6, out, cap, out_len);
}
esp_err_t ap2_hap_verify_m1(ap2_hap_session_t *s, const uint8_t *in, size_t in_len,
                            uint8_t *out, size_t cap, size_t *out_len) {
  (void)in; (void)in_len;
  call("hap_verify_m1()");
  s->verify_state = 2;
  return hap_reply(2, out, cap, out_len);
}
esp_err_t ap2_hap_verify_m3(ap2_hap_session_t *s, const uint8_t *in, size_t in_len,
                            uint8_t *out, size_t cap, size_t *out_len) {
  (void)in; (void)in_len;
  call("hap_verify_m3()");
  if (s->verify_state != 2) return ESP_FAIL;
  s->verify_state = 4;
  if (fake.hap_mode == 0) establish(s, false);
  return hap_reply(4, out, cap, out_len);
}
int ap2_hap_setup_state(const ap2_hap_session_t *s) { return s->setup_state; }
int ap2_hap_verify_state(const ap2_hap_session_t *s) { return s->verify_state; }
bool ap2_hap_established(const ap2_hap_session_t *s) { return s->established; }
const uint8_t *ap2_hap_shared_secret(const ap2_hap_session_t *s) { return s->secret; }
const uint8_t *ap2_hap_pairing_secret(const ap2_hap_session_t *s, size_t *len) {
  if (!s->established) {
    *len = 0;
    return NULL;
  }
  *len = s->secret_len;
  return s->secret;
}
const uint8_t *ap2_hap_channel_key(const ap2_hap_session_t *s, ap2_hap_direction_t d) {
  /* never NULL; before pairing ends the key is not set yet */
  if (!s->established) fake.channel_key_early++;
  return s->channel_key[d];
}
uint64_t ap2_hap_channel_counter(const ap2_hap_session_t *s, ap2_hap_direction_t d) {
  return s->channel_counter[d];
}
void ap2_hap_channel_count(ap2_hap_session_t *s, ap2_hap_direction_t d) {
  s->channel_counter[d]++;
  fake.channel_counted[d]++;
}
esp_err_t ap2_hap_audio_key(const ap2_hap_session_t *s, uint8_t *key, size_t key_len) {
  if (!s->established || fake.audio_key_fail) return ESP_FAIL;
  for (size_t i = 0; i < key_len; i++) key[i] = (uint8_t)(0xA0 + i);
  return ESP_OK;
}

/* Not HKDF: a deterministic mix of every input, enough to tell keys apart. */
int ap2_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                    size_t ikm_len, const uint8_t *info, size_t info_len,
                    uint8_t *okm, size_t okm_len) {
  call("hkdf(%.*s,%.*s,%zu)", (int)salt_len, (const char *)salt, (int)info_len,
       (const char *)info, ikm_len);
  mix(salt, salt_len, ikm, ikm_len, info, info_len, okm, okm_len);
  return 0;
}

void fake_kdf(const char *salt, const uint8_t *secret, size_t secret_len, const char *info,
              uint8_t key[32]) {
  mix((const uint8_t *)salt, strlen(salt), secret, secret_len, (const uint8_t *)info,
      strlen(info), key, 32);
}

static void mix(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < salt_len; i++) h = (h ^ salt[i]) * 16777619u;
  for (size_t i = 0; i < ikm_len; i++) h = (h ^ ikm[i]) * 16777619u;
  for (size_t i = 0; i < info_len; i++) h = (h ^ info[i]) * 16777619u;
  for (size_t i = 0; i < okm_len; i++) {
    h = (h ^ (uint32_t)i) * 16777619u;
    okm[i] = (uint8_t)(h >> 24);
  }
}

/* ---- ap2_fairplay ---- */

int ap2_fairplay_setup(const uint8_t *req, size_t len, uint8_t *reply, size_t cap) {
  int n;
  if (len < 16 || memcmp(req, "FPLY", 4) != 0 || req[4] != 3) return -1;
  if (req[6] == 1 && len == 16 && req[14] <= 3) n = fake.fp_reply_len > 0 ? fake.fp_reply_len : 142;
  else if (req[6] == 3 && len == 164) n = 32;
  else return -1;
  if ((size_t)n > cap) return -1;
  memset(reply, 0x46, (size_t)n);
  return n;
}

/* ---- Fake binary plist ---- */

typedef struct {
  char key[48];
  char kind; /* i r s d [ */
  char text[64];
} fentry_t;

typedef struct {
  fentry_t e[16];
  int n;
} fdict_t;

typedef struct {
  fdict_t root;
  fdict_t streams[4];
  int nstreams;
} fplist_t;

static bool parse_entry(const char **pp, const char *end, char stop1, char stop2,
                        fentry_t *e) {
  const char *p = *pp;
  const char *eq = p;
  size_t n;
  while (eq < end && *eq != '=') eq++;
  if (eq >= end || eq == p || (size_t)(eq - p) >= sizeof(e->key)) return false;
  memcpy(e->key, p, (size_t)(eq - p));
  e->key[eq - p] = '\0';
  p = eq + 1;
  if (p >= end) return false;
  e->kind = *p++;
  if (strchr("irsd", e->kind) == NULL) return false;
  n = 0;
  while (p < end && *p != stop1 && *p != stop2) {
    if (n + 1 >= sizeof(e->text)) return false;
    e->text[n++] = *p++;
  }
  e->text[n] = '\0';
  *pp = p;
  return true;
}

static bool fparse(const uint8_t *plist, size_t len, fplist_t *out) {
  const char *p = (const char *)plist;
  const char *end = p + len;
  memset(out, 0, sizeof(*out));
  if (plist == NULL || len < 8 || memcmp(p, "bplist00", 8) != 0) return false;
  p += 8;
  while (p < end) {
    if (strncmp(p, "streams=[", 9) == 0 && end - p >= 9) {
      p += 9;
      while (p < end && *p == '{') {
        fdict_t *d;
        if (out->nstreams >= 4) return false;
        d = &out->streams[out->nstreams++];
        p++;
        while (p < end && *p != '}') {
          if (d->n >= 16 || !parse_entry(&p, end, ',', '}', &d->e[d->n++])) return false;
          if (p < end && *p == ',') p++;
        }
        if (p >= end) return false;
        p++; /* } */
      }
      if (p >= end || *p != ']') return false;
      p++;
      if (out->root.n >= 16) return false;
      snprintf(out->root.e[out->root.n].key, sizeof(out->root.e[0].key), "streams");
      out->root.e[out->root.n++].kind = '[';
    } else {
      if (out->root.n >= 16 || !parse_entry(&p, end, ';', ';', &out->root.e[out->root.n++])) {
        return false;
      }
    }
    if (p < end) {
      if (*p != ';') return false;
      p++;
    }
  }
  return true;
}

static const fentry_t *fget(const fdict_t *d, const char *key) {
  for (int i = 0; i < d->n; i++) {
    if (strcmp(d->e[i].key, key) == 0) return &d->e[i];
  }
  return NULL;
}

static bool fint(const fentry_t *e, int64_t *out) {
  char *end;
  if (e == NULL || e->kind != 'i') return false;
  if (strncmp(e->text, "0x", 2) == 0) *out = (int64_t)strtoull(e->text, &end, 16);
  else *out = strtoll(e->text, &end, 10);
  return *end == '\0' && end != e->text;
}

bool ap2_bplist_data_deep(const uint8_t *plist, size_t plist_len, const char *key,
                          uint8_t *out, size_t cap, size_t *out_len) {
  (void)plist; (void)plist_len; (void)key; (void)out; (void)cap; (void)out_len;
  return false;
}

bool ap2_bplist_int(const uint8_t *plist, size_t len, const char *key, int64_t *out) {
  fplist_t f;
  return fparse(plist, len, &f) && fint(fget(&f.root, key), out);
}

bool ap2_bplist_real(const uint8_t *plist, size_t len, const char *key, double *out) {
  fplist_t f;
  const fentry_t *e;
  int64_t i;
  if (!fparse(plist, len, &f)) return false;
  e = fget(&f.root, key);
  if (e != NULL && e->kind == 'r') {
    char *end;
    *out = strtod(e->text, &end);
    return *end == '\0';
  }
  if (fint(e, &i)) {
    *out = (double)i;
    return true;
  }
  return false;
}

bool ap2_bplist_string(const uint8_t *plist, size_t len, const char *key, char *out,
                       size_t cap) {
  fplist_t f;
  const fentry_t *e;
  if (!fparse(plist, len, &f)) return false;
  e = fget(&f.root, key);
  if (e == NULL || e->kind != 's' || strlen(e->text) + 1 > cap) return false;
  strcpy(out, e->text);
  return true;
}

bool ap2_bplist_stream_count(const uint8_t *plist, size_t len, size_t *count) {
  fplist_t f;
  if (!fparse(plist, len, &f) || fget(&f.root, "streams") == NULL) return false;
  *count = (size_t)f.nstreams;
  return true;
}

bool ap2_bplist_stream_info(const uint8_t *plist, size_t len, size_t index, int64_t *type,
                            size_t *ekey_len, size_t *eiv_len, size_t *shk_len) {
  (void)plist; (void)len; (void)index; (void)type; (void)ekey_len; (void)eiv_len; (void)shk_len;
  return false;
}

bool ap2_bplist_stream_keys(const uint8_t *plist, size_t len, size_t index,
                            ap2_bplist_key_t *out, size_t cap, size_t *count) {
  fplist_t f;
  const fdict_t *d;
  if (!fparse(plist, len, &f) || index >= (size_t)f.nstreams) return false;
  d = &f.streams[index];
  if (d->n == 0 || (size_t)d->n > cap) return false;
  for (int i = 0; i < d->n; i++) {
    const fentry_t *e = &d->e[i];
    memset(&out[i], 0, sizeof(out[i]));
    snprintf(out[i].key, sizeof(out[i].key), "%s", e->key);
    switch (e->kind) {
      case 'i':
        out[i].value_type = AP2_BPLIST_INT;
        if (!fint(e, &out[i].int_value)) return false;
        break;
      case 'r': {
        double v = strtod(e->text, NULL);
        if (strcmp(e->key, "sr") != 0 || v <= 0 || v != (double)(int64_t)v) return false;
        out[i].value_type = AP2_BPLIST_INT;
        out[i].int_value = (int64_t)v;
        break;
      }
      case 's':
        out[i].value_type = AP2_BPLIST_STRING;
        out[i].value_len = strlen(e->text);
        break;
      case 'd':
        out[i].value_type = AP2_BPLIST_DATA;
        out[i].value_len = (size_t)atoi(e->text);
        break;
      default:
        return false;
    }
  }
  *count = (size_t)d->n;
  return true;
}

static void fill_data(const fentry_t *e, uint8_t *buf, size_t cap, size_t *len) {
  size_t n;
  if (e == NULL || e->kind != 'd' || buf == NULL || len == NULL) return;
  n = (size_t)atoi(e->text);
  if (n > cap) return;
  for (size_t k = 0; k < n; k++) buf[k] = (uint8_t)(0xA0 + k);
  *len = n;
}

bool ap2_bplist_stream_crypto(const uint8_t *plist, size_t len, int64_t stream_type,
                              uint8_t *ekey, size_t ekey_cap, size_t *ekey_len,
                              uint8_t *eiv, size_t eiv_cap, size_t *eiv_len,
                              uint8_t *shk, size_t shk_cap, size_t *shk_len) {
  fplist_t f;
  if (!fparse(plist, len, &f)) return false;
  for (int i = 0; i < f.nstreams; i++) {
    int64_t t;
    size_t n0 = 0, n1 = 0, n2 = 0;
    if (!fint(fget(&f.streams[i], "type"), &t) || t != stream_type) continue;
    fill_data(fget(&f.streams[i], "ekey"), ekey, ekey_cap, &n0);
    fill_data(fget(&f.streams[i], "eiv"), eiv, eiv_cap, &n1);
    fill_data(fget(&f.streams[i], "shk"), shk, shk_cap, &n2);
    if (ekey_len) *ekey_len = n0;
    if (eiv_len) *eiv_len = n1;
    if (shk_len) *shk_len = n2;
    return n0 || n1 || n2;
  }
  return false;
}

/* Writers: readable text instead of binary plists. */
size_t ap2_reply_setup_session(uint8_t *out, size_t cap, uint16_t event_port) {
  int n = snprintf((char *)out, cap, "SESSION eventPort=%u timingPort=0", event_port);
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
size_t ap2_reply_setup_stream(uint8_t *out, size_t cap, int64_t type, uint16_t data_port,
                              uint16_t control_port, uint32_t audio_buffer_size) {
  int n = snprintf((char *)out, cap,
                   "STREAM type=%" PRId64 " dataPort=%u controlPort=%u audioBufferSize=%" PRIu32,
                   type, data_port, control_port, audio_buffer_size);
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
size_t ap2_reply_feedback(uint8_t *out, size_t cap, int64_t type, double sample_rate) {
  int n = snprintf((char *)out, cap, "FEEDBACK type=%" PRId64 " sr=%.1f", type, sample_rate);
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
size_t ap2_reply_info(uint8_t *out, size_t cap, const ap2_info_t *i) {
  int n = snprintf((char *)out, cap,
                   "INFO deviceid=%s;name=%s;model=%s;srcvers=%s;protovers=%s;vv=%" PRId64
                   ";statusFlags=%" PRId64 ";features=%" PRIu64 ";pk=%zu:%02x..%02x;pi=%s"
                   ";initialVolume=%.4f;audioStream=%" PRIu64 ";bufferStream=%" PRIu64,
                   i->device_id, i->name, i->model, i->source_version, i->protocol_versions,
                   i->protocol_version, i->status_flags, i->features, i->public_key_len,
                   i->public_key[0], i->public_key[31], i->pairing_id, i->initial_volume,
                   i->audio_stream_formats, i->buffer_stream_formats);
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
size_t ap2_event_command(uint8_t *out, size_t cap, uint8_t command) {
  int n;
  if (command > 5) return 0;
  n = snprintf((char *)out, cap, "CMD %u", command);
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
size_t ap2_event_volume(uint8_t *out, size_t cap, double volume) {
  int n;
  if (volume < 0.0 || volume > 1.0) return 0;
  n = snprintf((char *)out, cap, "VOL %.4f", volume);
  return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}
