/* SPDX-License-Identifier: Apache-2.0 */
/* The request rules of the RTSP server (spec "A session", "Requests"). */
#include "rtsp_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "airplay_identity.h"
#include "amp_control.h"
#include "ap2_fairplay.h"
#include "ap2_hap.h"
#include "ap2_pairings.h"
#include "mdns_airplay.h"
#include "ap2_plist.h"
#include "ap2_tlv8.h"
#include "apple_clock.h"
#include "sound_engine.h"
#include "rtsp_core_internal.h"
#include "rtsp_events.h"
#include "rtsp_frame.h"
#include "rtsp_log.h"
#include "rtsp_msg.h"
#include "rtsp_tables.h"

#define REPLY_BODY_MAX 4096
#define REPLY_HEAD_MAX 1024
#define REALTIME_LATENCY_SAMPLES 11025U
#define STREAM_KEYS_MAX 32
#define DEVICE_NAME_MAX 128
#define BODY_LOG_BYTES 64

#define CT_BPLIST "application/x-apple-binary-plist"
#define CT_OCTET "application/octet-stream"
#define CT_TEXT_PARAMETERS "text/parameters"

/* spec "Reply codes" */
#define CODE_BAD_REQUEST 400   /* missing or wrong data */
#define CODE_TOO_LARGE 413     /* body over RTSP_BODY_MAX */
#define CODE_WRONG_STATE 455   /* stream SETUP without the session SETUP */
#define CODE_AUTH_REQUIRED 470 /* pairing failed, SETUP not encrypted */
#define CODE_INTERNAL 500      /* no memory, a port or the engine failed */
#define CODE_NOT_IMPLEMENTED 501

static const char PUBLIC_METHODS[] =
    "Public: SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, OPTIONS, "
    "POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, SETPEERSX, "
    "SETRATEANCHORTIME, LOUDNESSNORMALIZATION\r\n";

struct rtsp_conn {
  struct rtsp_conn *next;
  uint32_t peer_ip;
  void *port_ctx;
  unsigned id;
  bool closing;
  bool reply_http;

  ap2_hap_session_t *hap;
  bool encrypted;
  /* The control channel's keys and counters are the core's
   * (ap2_hap_channel_*); these hold copies for each frame. */
  rtsp_cipher_t out_cipher;     /* Gateway to sender */
  rtsp_frame_reader_t in_frames; /* sender to Gateway */
  bool switch_pending;          /* encrypt after this reply */
  bool paired;                  /* event keys are set */
  uint8_t event_write[32];      /* Gateway to sender */
  uint8_t event_read[32];       /* sender to Gateway */

  bool remote_only;
  char group_id[48];
  bool group_contains_leader;
  bool event_open;     /* its event port listens (from its session SETUP) */
  uint16_t event_port;

  rtsp_reader_t reader;
  char head[REPLY_HEAD_MAX];
  uint8_t body[REPLY_BODY_MAX];
  uint8_t frame[RTSP_FRAME_MAX + RTSP_FRAME_OVERHEAD];
};

typedef struct {
  int code;
  const char *content_type;
  size_t body_len;
  const char *extra;
  bool close;
  bool homekit_reset;
} reply_t;

static struct {
  rtsp_conn_t *connections;
  rtsp_conn_t *holder; /* the connection that holds the session */
  bool stream;
  int64_t stream_type;
  int sample_rate;
  double volume;
  bool volume_known;
  bool homekit_reset_pending;
  /* mDNS changes wait for publish_pending(), outside the lock: TXT updates
   * send packets and a new name waits for the mDNS task. */
  bool mdns_pending;       /* status flags or group may have changed */
  bool name_pending;       /* a new name was saved, not announced yet */
  bool publishing;         /* one task publishes at a time */
  unsigned next_conn_id;
} s;

static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;
  while (n--) *v++ = 0;
}

/* ---- mDNS publishing ---- */

#ifdef CONFIG_AIRPLAY_HOMEKIT
/* Under the lock: the TXT status flags or the group may have changed. */
static void mdns_mark(void) { s.mdns_pending = true; }
#endif

/* Without the lock, after a reply or a closed connection. Only one task
 * publishes; one that finds another publishing leaves the work to it, which
 * loops until nothing is pending, so the last state is always published. */
static void publish_pending(void) {
#ifdef CONFIG_AIRPLAY_HOMEKIT
  for (;;) {
    char gid[sizeof(((rtsp_conn_t *)0)->group_id)] = {0};
    bool leader = false, flags, rename;

    rtsp_port_lock();
    if ((!s.mdns_pending && !s.name_pending) || s.publishing) {
      rtsp_port_unlock();
      return;
    }
    flags = s.mdns_pending;
    rename = s.name_pending;
    s.mdns_pending = false;
    s.name_pending = false;
    s.publishing = true;
    if (s.holder != NULL) {
      memcpy(gid, s.holder->group_id, sizeof(gid));
      leader = s.holder->group_contains_leader;
    }
    rtsp_port_unlock();

    if (flags) {
      mdns_airplay_update_flags();
      mdns_airplay_set_group(gid, leader);
    }
    if (rename && rtsp_port_settings_announce_name() != ESP_OK) {
      RTSP_LOGW("speaker name saved, announcing it on the network failed");
    }

    rtsp_port_lock();
    s.publishing = false;
    rtsp_port_unlock();
  }
#endif
}

static void ip_text(uint32_t ip, char out[16]) {
  const uint8_t *b = (const uint8_t *)&ip; /* network byte order */
  snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ---- Volume ---- */

static double saved_volume(void) {
  float v;
  if (rtsp_port_settings_get_volume(&v) != ESP_OK) {
    RTSP_LOGW("no saved volume, using -24");
    return RTSP_VOLUME_DEFAULT;
  }
  return (double)v;
}

double rtsp_core_current_volume(void) {
  if (!s.volume_known) {
    s.volume = saved_volume();
    s.volume_known = true;
  }
  return s.volume;
}

static void set_volume(double volume) {
  s.volume = volume;
  s.volume_known = true;
  sound_engine_set_volume_q15(rtsp_volume_to_q15(volume));
}

/* A change by the sender: to the engine and to the settings (RAM only; it
 * is written to flash when the session ends). */
static void change_volume(double volume) {
  set_volume(volume);
  if (rtsp_port_settings_set_volume((float)volume) != ESP_OK) {
    RTSP_LOGW("volume not kept in the settings");
  }
}

void rtsp_core_remote_volume(double volume) {
  if (s.holder == NULL) return;
  change_volume(volume);
#ifdef CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
  RTSP_LOGI("volume from remote command, q15 %" PRId32,
            rtsp_volume_to_q15(volume));
#endif
}

/* ---- Session ---- */

/* Every session SETUP, holder or not, has its own event port, which
 * listens before the reply and until its session ends or the connection
 * closes (spec "A second sender", "Event channel"). Opening it again is
 * nothing new. */
static esp_err_t event_port_open(rtsp_conn_t *c) {
  if (c->event_open) return ESP_OK;
  if (rtsp_port_event_open(c, c->event_write, c->event_read,
                           &c->event_port) != ESP_OK) {
    RTSP_LOGW("event port does not open");
    return ESP_FAIL;
  }
  c->event_open = true;
  return ESP_OK;
}

static void event_port_close(rtsp_conn_t *c) {
  if (!c->event_open) return;
  rtsp_port_event_close(c);
  rtsp_remote_conn_gone(c);
  c->event_open = false;
}

static esp_err_t session_start(rtsp_conn_t *c) {
  uint8_t key[32];
  sound_cipher_t enc;
  char ip[16];

  if (ap2_hap_audio_key(c->hap, key, sizeof(key)) != ESP_OK) {
    RTSP_LOGW("no audio key from the pairing");
    return ESP_FAIL;
  }
  apple_clock_select(c->peer_ip, false);
  if (event_port_open(c) != ESP_OK) {
    apple_clock_release();
    wipe(key, sizeof(key));
    return ESP_FAIL;
  }
  s.holder = c;
  s.stream = false;

  memset(&enc, 0, sizeof(enc));
  enc.type = SOUND_CIPHER_CHACHA20_POLY1305;
  memcpy(enc.key, key, sizeof(key));
  enc.key_len = sizeof(key);
  sound_engine_set_encryption(&enc);
  wipe(&enc, sizeof(enc));
  wipe(key, sizeof(key));

  s.volume_known = false;
  set_volume(rtsp_core_current_volume());

#ifdef CONFIG_AIRPLAY_HOMEKIT
  airplay_set_session_active(true);
  mdns_mark();
#endif
  amp_control_session_connected();
  rtsp_events_emit(RTSP_EVENT_CLIENT_CONNECTED);
  ip_text(c->peer_ip, ip);
  RTSP_LOGI("session start: connection %u, sender %s, event port %u", c->id,
            ip, c->event_port);
  return ESP_OK;
}

static void stream_stop(void) {
  sound_engine_set_playing(false);
  sound_engine_stop();
  s.stream = false;
}

static void session_end(void) {
  rtsp_conn_t *h = s.holder;
  sound_cipher_t none;

  if (h == NULL) return;
  sound_engine_set_playing(false);
  sound_engine_stop();
  s.stream = false;
  s.holder = NULL;
  apple_clock_release();
  rtsp_remote_detach();
  event_port_close(h);
  sound_engine_set_stream_type(SOUND_STREAM_NONE);
  memset(&none, 0, sizeof(none));
  none.type = SOUND_CIPHER_NONE;
  sound_engine_set_encryption(&none);
#ifdef CONFIG_AIRPLAY_HOMEKIT
  airplay_set_session_active(false);
  mdns_mark();
#endif
  amp_control_session_disconnected();
  rtsp_events_emit(RTSP_EVENT_DISCONNECTED);
  if (rtsp_port_settings_persist_volume() != ESP_OK) {
    RTSP_LOGW("volume not saved");
  }
  RTSP_LOGI("session end: connection %u", h->id);
}

bool rtsp_core_idle(void) {
  bool idle;
  rtsp_port_lock();
  idle = s.holder == NULL;
  rtsp_port_unlock();
  return idle;
}

/* ---- Helpers for requests ---- */

static bool is_bplist(const rtsp_msg_t *m) {
  return m->body_len >= 8 && memcmp(m->body, "bplist00", 8) == 0;
}

static bool path_is(const char *uri, const char *path) {
  size_t n = strlen(path);
  return strncmp(uri, path, n) == 0 && (uri[n] == '\0' || uri[n] == '?');
}

static void refuse(reply_t *r, int code, const char *why) {
  r->code = code;
  RTSP_LOGW("refused (%d): %s", code, why);
}

static void not_implemented(rtsp_conn_t *c, reply_t *r, const char *what) {
  RTSP_LOGW("not implemented: %s", what);
  r->code = CODE_NOT_IMPLEMENTED;
  r->content_type = "text/plain";
  memcpy(c->body, "Not Implemented", 15);
  r->body_len = 15;
}

static bool holds(const rtsp_conn_t *c) { return s.holder == c; }

rtsp_conn_t *rtsp_core_holder(void) { return s.holder; }

/* Reserve the new control resources before stopping the previous sender.
 * Called under the server lock; only an audio session may use this path. */
static esp_err_t claim_session(rtsp_conn_t *c) {
  if (c->closing) return ESP_ERR_INVALID_STATE;
  if (holds(c)) return event_port_open(c);

  uint8_t key[32];
  esp_err_t err = ap2_hap_audio_key(c->hap, key, sizeof(key));
  wipe(key, sizeof(key));
  if (err != ESP_OK) return err;
  err = event_port_open(c);
  if (err != ESP_OK) return err;

  rtsp_conn_t *old = s.holder;
  if (old != NULL) {
    RTSP_LOGI("connection %u takes the session from connection %u", c->id, old->id);
    session_end();
    old->closing = true;
    rtsp_port_conn_close(old);
  }
  return session_start(c);
}


/* ---- Requests ---- */

static void do_info(rtsp_conn_t *c, reply_t *r) {
  char device_id[18];
  char name[DEVICE_NAME_MAX];
  char pairing_id[AIRPLAY_PAIRING_ID_LEN];
  uint8_t mac[6];
  float volume;
  ap2_info_t info;
  size_t n;

  rtsp_port_device_mac(mac);
  snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (rtsp_port_settings_get_name(name, sizeof(name)) != ESP_OK) name[0] = '\0';
  name[sizeof(name) - 1] = '\0';
  airplay_get_pairing_id(pairing_id, sizeof(pairing_id));
  pairing_id[sizeof(pairing_id) - 1] = '\0';
  if (rtsp_port_settings_get_volume(&volume) != ESP_OK) {
    volume = (float)RTSP_VOLUME_DEFAULT;
  }

  memset(&info, 0, sizeof(info));
  info.device_id = device_id;
  info.name = name;
  info.model = AIRPLAY_MODEL;
  info.source_version = AIRPLAY_SOURCE_VERSION;
  info.protocol_versions = AIRPLAY_PROTOVERS;
  info.protocol_version = AIRPLAY_PROTOCOL_VERSION;
  info.status_flags = airplay_status_flags();
  info.features = ((uint64_t)AIRPLAY_FEATURES_HI << 32) | AIRPLAY_FEATURES_LO;
  info.public_key = ap2_hap_identity_public_key();
  info.public_key_len = 32;
  info.pairing_id = pairing_id;
  info.initial_volume = (double)volume;
  info.audio_stream_formats = AIRPLAY_SUPPORTED_AUDIO_STREAM;
  info.buffer_stream_formats = AIRPLAY_SUPPORTED_BUFFER_STREAM;

#ifdef CONFIG_AIRPLAY_HOMEKIT
  char serial[AIRPLAY_SERIAL_LEN], firmware[32];
  airplay_get_serial_number(serial, sizeof(serial));
  airplay_get_firmware_revision(firmware, sizeof(firmware));
  info.manufacturer = CONFIG_AIRPLAY_HOMEKIT_MANUFACTURER;
  info.serial_number = serial;
  info.firmware_revision = firmware;
#endif
  n = ap2_reply_info(c->body, sizeof(c->body), &info);
  if (n == 0) {
    refuse(r, CODE_INTERNAL, "/info reply does not fit");
    return;
  }
  r->content_type = CT_BPLIST;
  r->body_len = n;
}

static bool derive_key(const uint8_t *secret, size_t secret_len,
                       const char *salt, const char *info, uint8_t out[32]) {
  return ap2_hkdf_sha512((const uint8_t *)salt, strlen(salt), secret,
                         secret_len, (const uint8_t *)info, strlen(info), out,
                         32) == 0;
}

/* Pairing ended with keys (spec "Encryption"): the control channel's are
 * the core's; the event channel's are derived here from the pairing
 * secret. The switch happens after this reply. */
static bool take_keys(rtsp_conn_t *c) {
  size_t len = 0;
  const uint8_t *secret = ap2_hap_pairing_secret(c->hap, &len);
  /* The control channel keys are the core's and never NULL (spec); only
   * the event channel keys are derived here. */
  if (secret == NULL || len == 0) return false;
  if (!derive_key(secret, len, "Events-Salt", "Events-Write-Encryption-Key",
                  c->event_write) ||
      !derive_key(secret, len, "Events-Salt", "Events-Read-Encryption-Key",
                  c->event_read)) {
    wipe(c->event_write, sizeof(c->event_write));
    wipe(c->event_read, sizeof(c->event_read));
    return false;
  }
  c->switch_pending = true;
  c->paired = true;
  return true;
}

typedef esp_err_t (*pair_step_fn)(ap2_hap_session_t *, const uint8_t *, size_t,
                                  uint8_t *, size_t, size_t *);

static void do_pair(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r,
                    bool setup) {
  size_t state_len = 0;
  const uint8_t *state =
      ap2_tlv8_find(m->body, m->body_len, AP2_TLV_STATE, &state_len);
  int step = (state != NULL && state_len == 1) ? state[0] : 0;
  pair_step_fn fn = NULL;
  bool last = false;
  size_t out_len = 0;
  esp_err_t err = ESP_FAIL;

  if (c->hap == NULL) {
    c->hap = ap2_hap_session_new();
    if (c->hap == NULL) {
      refuse(r, CODE_INTERNAL, "no memory for pairing");
      return;
    }
  }
  if (setup) {
    if (step == 1) fn = ap2_hap_setup_m1;
    if (step == 3) { fn = ap2_hap_setup_m3; last = true; }
    if (step == 5) { fn = ap2_hap_setup_m5; last = true; }
  } else {
    if (step == 1) {
#ifdef CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
      const char *hkp = rtsp_msg_header(m, "X-Apple-HKP");
      const char *derive = rtsp_msg_header(m, "X-Apple-Pair-Derive");
      RTSP_LOGI("[%u] pair-verify M1: protocol=%.8s HKP=%.8s derive=%.8s", c->id,
                m->start[2], hkp ? hkp : "-", derive ? derive : "-");
#endif
      fn = ap2_hap_verify_m1;
    }
    if (step == 3) { fn = ap2_hap_verify_m3; last = true; }
  }
  if (fn != NULL) {
    err = fn(c->hap, m->body, m->body_len, c->body, sizeof(c->body), &out_len);
  }
  if (fn == NULL) {
    refuse(r, CODE_AUTH_REQUIRED, setup ? "pair-setup without a known State"
                                        : "pair-verify without a known State");
    return;
  }
  if (err != ESP_OK) {
    RTSP_LOGW("%s step %d: core error %d", setup ? "pair-setup" : "pair-verify",
              step, (int)err);
    refuse(r, CODE_AUTH_REQUIRED, "pairing step failed");
    return;
  }
  r->content_type = CT_OCTET;
  r->body_len = out_len;
#ifdef CONFIG_AIRPLAY_HOMEKIT
  if (setup && step == 5) mdns_mark(); /* full pair-setup stores a home */
#endif
  if (last && ap2_hap_established(c->hap)) {
    if (take_keys(c)) {
      RTSP_LOGI("connection %u paired, encrypted from the next request",
                c->id);
    } else {
      RTSP_LOGW("connection %u: no event channel keys", c->id);
    }
  }
}

#ifdef CONFIG_AIRPLAY_HOMEKIT
static void revoke_homekit_connections(rtsp_conn_t *requester, reply_t *reply) {
  /* Called with the core lock held. Keep the current encrypted channel long
   * enough to send M2/configure; shut down its socket only after that reply. */
  for (rtsp_conn_t *peer = s.connections; peer; peer = peer->next) {
    if (peer->closing || !ap2_hap_controller_revoked(peer->hap)) continue;
    peer->closing = true;
    if (s.holder == peer) session_end();
    event_port_close(peer);
    if (peer == requester) reply->close = true;
    else rtsp_port_conn_close(peer);
  }
}

static void homekit_reset_after_reply(rtsp_conn_t *c, reply_t *r) {
  s.homekit_reset_pending = true;
  c->closing = true;
  r->close = true;
  r->homekit_reset = true;
  RTSP_LOGW("HomeKit removal accepted: reset after reply");
}

static void do_configure(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  if (!c->encrypted || !c->paired) {
    refuse(r, CODE_AUTH_REQUIRED, "configure needs an encrypted paired connection");
    return;
  }
  ap2_configure_request_t config = {0};
  if (m->body_len && (!is_bplist(m) ||
      !ap2_bplist_configure_request(m->body, m->body_len, &config))) {
    refuse(r, CODE_BAD_REQUEST, "invalid ConfigurationDictionary");
    return;
  }
  uint8_t permission = 0;
  const bool admin = ap2_hap_controller(c->hap, NULL, NULL, &permission) &&
                     (permission & AP2_PERM_ADMIN);
  const bool initial = ap2_hap_setup_transient(c->hap) && ap2_pairings_count() == 0;
  if ((config.has_name || config.has_hk_access_control) && !admin && !initial) {
    refuse(r, CODE_AUTH_REQUIRED, "configure changes need a verified administrator");
    return;
  }
  if (config.has_hk_access_control && config.hk_access_control &&
      !ap2_pairings_has_admin()) {
    refuse(r, CODE_WRONG_STATE, "add a HomeKit administrator before enabling access control");
    return;
  }
  const bool reset = admin && config.has_hk_access_control &&
                     !config.hk_access_control;
  if (!reset && config.has_name) {
    if (rtsp_port_settings_set_name(config.name) != ESP_OK) {
      refuse(r, CODE_INTERNAL, "speaker name was not saved");
      return;
    }
    s.name_pending = true; /* host name and mDNS follow after the reply */
  }
  char name[DEVICE_NAME_MAX], pi[AIRPLAY_PAIRING_ID_LEN];
  const esp_err_t name_err = reset
      ? rtsp_port_homekit_reset_name(name, sizeof(name))
      : rtsp_port_settings_get_name(name, sizeof(name));
  if (name_err != ESP_OK) {
    refuse(r, CODE_INTERNAL, "cannot read speaker name");
    return;
  }
  airplay_get_pairing_id(pi, sizeof(pi));
  r->body_len = ap2_reply_configure(c->body, sizeof(c->body), pi,
      ap2_hap_identity_public_key(), name, !reset && ap2_pairings_has_admin());
  r->content_type = CT_BPLIST;
  if (!r->body_len) {
    refuse(r, CODE_INTERNAL, "configure reply does not fit");
    return;
  }
  if (!reset && config.has_name) RTSP_LOGI("HomeKit configure: speaker name saved");
  if (reset) homekit_reset_after_reply(c, r);
  else revoke_homekit_connections(c, r);
}

static void do_pairings(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  enum { ADD = 3, REMOVE = 4, LIST = 5, PERMISSIONS = 0x0B };
  uint8_t method = 0, perm = 0, id[AP2_PAIRING_ID_MAX], pk[32];
  size_t len = 0, id_len = 0, pk_len = 0, perm_len = 0;
  if (!ap2_tlv8_get(m->body, m->body_len, AP2_TLV_METHOD, &method, 1, &len)) {
    if (path_is(m->start[1], "/pair-add")) method = ADD;
    else if (path_is(m->start[1], "/pair-remove")) method = REMOVE;
    else if (path_is(m->start[1], "/pair-list")) method = LIST;
  }
  uint8_t current_perm = 0, error = AP2_TLV_ERROR_UNKNOWN;
  const bool admin = c->encrypted && c->paired &&
      ap2_hap_controller(c->hap, NULL, NULL, &current_perm) &&
      (current_perm & AP2_PERM_ADMIN);
  const bool first = !admin && method == ADD && c->encrypted && c->paired &&
      ap2_hap_setup_transient(c->hap) && ap2_pairings_count() == 0;
  r->content_type = CT_OCTET;
  const bool id_ok = ap2_tlv8_get(m->body, m->body_len, AP2_TLV_IDENTIFIER,
                                  id, sizeof(id), &id_len) && id_len > 0;
  const bool pk_ok = ap2_tlv8_get(m->body, m->body_len, AP2_TLV_PUBLIC_KEY,
                                  pk, sizeof(pk), &pk_len) && pk_len == sizeof(pk);
  size_t raw_perm_len = 0;
  const bool has_perm = ap2_tlv8_find(m->body, m->body_len, PERMISSIONS,
                                     &raw_perm_len) != NULL;
  const bool perm_ok = !has_perm ||
      (ap2_tlv8_get(m->body, m->body_len, PERMISSIONS, &perm, 1, &perm_len) &&
       perm_len == 1 && perm <= AP2_PERM_ADMIN);
  uint8_t stored_pk[32], stored_perm;
  /* A lost enrollment reply may be retried without proving the owner's
   * private key. Acknowledge an identical existing admin only; grant no
   * authenticated session and no access to listing or other mutations. */
  const bool retry = !admin && !first && method == ADD && c->encrypted &&
      c->paired && id_ok && pk_ok && perm_ok && (!has_perm || perm == AP2_PERM_ADMIN) &&
      ap2_pairings_find(id, id_len, stored_pk, &stored_perm) &&
      stored_perm == AP2_PERM_ADMIN && !memcmp(stored_pk, pk, sizeof(pk));
  if (retry) {
    error = 0;
  } else if (!admin && !first) {
    error = AP2_TLV_ERROR_AUTHENTICATION;
  } else if (method == LIST) {
    r->body_len = ap2_pairings_list_tlv(c->body, sizeof(c->body));
    if (r->body_len) return;
  } else if (method == ADD || method == REMOVE) {
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (method == ADD && id_ok && pk_ok && perm_ok) {
      if (first) perm = AP2_PERM_ADMIN;
      err = ap2_pairings_add(id, id_len, pk, perm);
      if (err == ESP_OK && first) ap2_hap_set_controller(c->hap, id, id_len, perm);
    } else if (method == REMOVE && id_ok) {
      if (ap2_pairings_is_last_admin(id, id_len)) {
        homekit_reset_after_reply(c, r);
        err = ESP_OK;
      } else {
        err = ap2_pairings_remove(id, id_len);
        if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
      }
    }
    if (err == ESP_OK) {
      error = 0;
      revoke_homekit_connections(c, r);
    } else if (err == ESP_ERR_NO_MEM) error = AP2_TLV_ERROR_MAX_PEERS;
  }
  ap2_tlv8_writer_t w;
  ap2_tlv8_writer_init(&w, c->body, sizeof(c->body));
  ap2_tlv8_put_byte(&w, AP2_TLV_STATE, 2);
  if (error) ap2_tlv8_put_byte(&w, AP2_TLV_ERROR, error);
  r->body_len = ap2_tlv8_writer_size(&w);
  mdns_mark(); /* joining or leaving a home changes the status flags */
}
#endif

static void do_fp_setup(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  int n = ap2_fairplay_setup(m->body, m->body_len, c->body, sizeof(c->body));
  r->content_type = CT_OCTET;
  if (n < 0) {
    RTSP_LOGW("fp-setup: unknown message (%zu bytes)", m->body_len);
    c->body[0] = 0x00;
    r->body_len = 1;
    return;
  }
  r->body_len = (size_t)n;
}

static void do_setup_session(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  char timing[16];
  size_t n;

  if (ap2_bplist_string(m->body, m->body_len, "timingProtocol", timing,
                        sizeof(timing))) {
    if (strcmp(timing, "PTP") == 0) {
      RTSP_LOGI("timingProtocol PTP");
    } else {
      RTSP_LOGW("timingProtocol %s, taken as PTP", timing);
    }
  } else {
    RTSP_LOGW("no timingProtocol, taken as PTP");
  }
#ifdef CONFIG_AIRPLAY_HOMEKIT
  int64_t remote = 0, leader = 0;
  (void)ap2_bplist_int_deep(m->body, m->body_len, "isRemoteControlOnly", &remote);
  c->remote_only = remote != 0;
  if (c->remote_only) {
    if (holds(c)) {
      refuse(r, CODE_WRONG_STATE, "audio holder cannot become remote-only");
      c->remote_only = false;
      return;
    }
    if (event_port_open(c) != ESP_OK) {
      refuse(r, CODE_INTERNAL, "no remote event port");
      return;
    }
    n = ap2_reply_setup_session(c->body, sizeof(c->body), c->event_port);
    if (!n) { refuse(r, CODE_INTERNAL, "remote SETUP reply does not fit"); return; }
    r->content_type = CT_BPLIST;
    r->body_len = n;
    return;
  }
  char group[sizeof(c->group_id)] = {0};
  if (ap2_bplist_string_deep(m->body, m->body_len, "groupUUID", group, sizeof(group)) &&
      group[0]) {
    memcpy(c->group_id, group, sizeof(group));
    (void)ap2_bplist_int_deep(m->body, m->body_len, "groupContainsGroupLeader", &leader);
    c->group_contains_leader = leader != 0;
  }
#endif
#ifdef CONFIG_AIRPLAY_HOMEKIT
  /* Selecting this speaker transfers ownership now. Buffered playback is
   * still gated by SETRATEANCHORTIME, not by pairing or session SETUP. */
  if (claim_session(c) != ESP_OK) {
    refuse(r, CODE_INTERNAL, "session takeover could not be prepared");
    return;
  }
#else
  if (s.holder == NULL) {
    if (session_start(c) != ESP_OK) {
      refuse(r, CODE_INTERNAL, "session does not start");
      return;
    }
  } else if (event_port_open(c) != ESP_OK) {
    refuse(r, CODE_INTERNAL, "no event port");
    return;
  } else if (s.holder != c) {
    RTSP_LOGI("connection %u: session held by connection %u, event port %u "
              "listens", c->id, s.holder->id, c->event_port);
  }
#endif
#ifdef CONFIG_AIRPLAY_HOMEKIT
  if (holds(c)) mdns_mark(); /* this SETUP's group */
#endif
  n = ap2_reply_setup_session(c->body, sizeof(c->body), c->event_port);
  if (n == 0) {
    refuse(r, CODE_INTERNAL, "SETUP reply does not fit");
    return;
  }
  r->content_type = CT_BPLIST;
  r->body_len = n;
}

static const ap2_bplist_key_t *find_key(const ap2_bplist_key_t *keys,
                                        size_t count, const char *name,
                                        uint8_t type) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(keys[i].key, name) == 0) {
      return keys[i].value_type == type ? &keys[i] : NULL;
    }
  }
  return NULL;
}

static void do_setup_stream(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r,
                            size_t stream_count) {
  ap2_bplist_key_t keys[STREAM_KEYS_MAX];
  size_t key_count = 0;
  const ap2_bplist_key_t *k_type, *k_format, *k_control;
  int64_t type;
  uint64_t audio_format;
  sound_format_t format;
  uint8_t shk[32];
  size_t shk_len = 0;
  uint16_t client_control = 0;
  uint16_t data_port = 0, control_port = 0;
  uint32_t buffer_size = 0;
  uint32_t latency;
  sound_cipher_t enc;
  esp_err_t err;
  size_t n;

  if (!c->event_open) {
    refuse(r, CODE_WRONG_STATE, "stream SETUP without session SETUP");
    return;
  }
  if (stream_count != 1) {
    refuse(r, CODE_BAD_REQUEST, "SETUP needs exactly one stream");
    return;
  }
  if (!ap2_bplist_stream_keys(m->body, m->body_len, 0, keys, STREAM_KEYS_MAX,
                              &key_count)) {
    refuse(r, CODE_BAD_REQUEST, "stream keys unreadable");
    return;
  }
  k_type = find_key(keys, key_count, "type", AP2_BPLIST_INT);
  k_format = find_key(keys, key_count, "audioFormat", AP2_BPLIST_INT);
  if (k_type == NULL || k_format == NULL) {
    refuse(r, CODE_BAD_REQUEST, "stream without type or audioFormat");
    return;
  }
  type = k_type->int_value;
  audio_format = (uint64_t)k_format->int_value;
  if (!rtsp_format_lookup(type, audio_format, &format)) {
    RTSP_LOGW("format 0x%" PRIx64 " for type %" PRId64 " not offered",
              audio_format, type);
    refuse(r, CODE_BAD_REQUEST, "format not in the table");
    return;
  }
  if (type == SOUND_STREAM_REALTIME) {
    k_control = find_key(keys, key_count, "controlPort", AP2_BPLIST_INT);
    if (k_control == NULL || k_control->int_value <= 0 ||
        k_control->int_value > 65535) {
      refuse(r, CODE_BAD_REQUEST, "realtime stream without controlPort");
      return;
    }
    client_control = (uint16_t)k_control->int_value;
  }
  if (!ap2_bplist_stream_crypto(m->body, m->body_len, type, NULL, 0, NULL,
                                NULL, 0, NULL, shk, sizeof(shk), &shk_len) ||
      shk_len != sizeof(shk)) {
    wipe(shk, sizeof(shk));
    refuse(r, CODE_BAD_REQUEST, "stream without a 32-byte shk");
    return;
  }

  if (claim_session(c) != ESP_OK) {
    wipe(shk, sizeof(shk));
    refuse(r, CODE_INTERNAL, "session does not start");
    return;
  }
  if (s.stream) {
    RTSP_LOGI("stream replaced");
    stream_stop();
  }

  sound_engine_set_stream_type((sound_stream_t)type);
  sound_engine_set_format(&format);
  memset(&enc, 0, sizeof(enc));
  enc.type = SOUND_CIPHER_CHACHA20_POLY1305;
  memcpy(enc.key, shk, sizeof(shk));
  enc.key_len = sizeof(shk);
  sound_engine_set_encryption(&enc);
  wipe(&enc, sizeof(enc));
  wipe(shk, sizeof(shk));
  apple_clock_select(c->peer_ip, type == SOUND_STREAM_REALTIME);

  if (type == SOUND_STREAM_REALTIME) {
    latency = REALTIME_LATENCY_SAMPLES;
    err = rtsp_port_udp_pair(&data_port, &control_port);
    if (err == ESP_OK) err = sound_engine_start_stream(data_port, control_port, 0);
    if (err != ESP_OK) {
      stream_stop();
      refuse(r, CODE_INTERNAL, "realtime reception does not start");
      return;
    }
    sound_engine_set_playout_latency_samples(latency);
    sound_engine_set_client_control(c->peer_ip, client_control);
    sound_engine_set_playing(true);
    rtsp_events_emit(RTSP_EVENT_PLAYING);
  } else {
    latency = 0;
    err = sound_engine_start_buffered(0);
    if (err != ESP_OK) {
      stream_stop();
      refuse(r, CODE_INTERNAL, "buffered reception does not start");
      return;
    }
    data_port = sound_engine_get_buffered_port();
    control_port = 0;
    buffer_size = AP2_BUFFERED_AUDIO_ADVERTISED_BYTES;
    sound_engine_set_playout_latency_samples(latency);
  }
  s.stream = true;
  s.stream_type = type;
  s.sample_rate = format.sample_rate;
  RTSP_LOGI("stream: type %" PRId64 ", %s %d Hz %d bit, %d frames, data port "
            "%u, control port %u, latency %" PRIu32,
            type, format.codec, format.sample_rate, format.bits_per_sample,
            format.frame_size, data_port, control_port, latency);

  n = ap2_reply_setup_stream(c->body, sizeof(c->body), type, data_port,
                             control_port, buffer_size);
  if (n == 0) {
    refuse(r, CODE_INTERNAL, "SETUP reply does not fit");
    return;
  }
  r->content_type = CT_BPLIST;
  r->body_len = n;
}

static void do_setup(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  size_t count = 0;
  if (!c->encrypted || !c->paired || c->hap == NULL) {
    refuse(r, CODE_AUTH_REQUIRED, "SETUP on a connection that is not encrypted");
    return;
  }
  if (!is_bplist(m)) {
    refuse(r, CODE_BAD_REQUEST, "SETUP body is not a binary plist");
    return;
  }
  if (ap2_bplist_stream_count(m->body, m->body_len, &count)) {
    if (c->remote_only) {
      refuse(r, CODE_WRONG_STATE, "stream on remote-only connection");
      return;
    }
    do_setup_stream(c, m, r, count);
  } else {
    do_setup_session(c, m, r);
  }
}

static void do_teardown(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  size_t count = 0;
  bool streams = is_bplist(m) &&
                 ap2_bplist_stream_count(m->body, m->body_len, &count);
  if (!streams) {
    r->close = true; /* also when nothing changes */
  }
  if (!holds(c)) {
    RTSP_LOGI("TEARDOWN from connection %u without the session: ignored",
              c->id);
    return;
  }
  if (streams) {
    stream_stop();
    rtsp_events_emit(RTSP_EVENT_PAUSED);
    RTSP_LOGI("stream end");
    return;
  }
  session_end();
}

static void do_set_parameter(rtsp_conn_t *c, const rtsp_msg_t *m,
                             reply_t *r) {
  const char *type = rtsp_msg_header(m, "Content-Type");
  const char *cursor = (const char *)m->body;
  const char *end = cursor != NULL ? cursor + m->body_len : NULL;
  const char *line;
  size_t line_len;
  bool have_volume = false;
  double volume = 0.0;

  if (!rtsp_content_type_is(type, CT_TEXT_PARAMETERS)) {
    /* track metadata (DMAP), artwork (image/...) and the rest: accepted,
     * not used */
    return;
  }
  while (rtsp_next_line(&cursor, end, &line, &line_len)) {
    char name[32];
    char value[64];
    if (line_len == 0) continue;
    if (!rtsp_split_param(line, line_len, name, sizeof(name), value,
                          sizeof(value))) {
      continue;
    }
    if (rtsp_str_ieq(name, "volume")) {
      if (!rtsp_parse_volume(value, &volume)) {
        refuse(r, CODE_BAD_REQUEST, "volume is not a number");
        return;
      }
      have_volume = true;
    }
    /* progress: accepted */
  }
  if (have_volume && holds(c)) {
    volume = rtsp_volume_clamp(volume);
    change_volume(volume);
#ifdef CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
    char text[24];
    rtsp_format_volume(volume, text, sizeof(text));
    RTSP_LOGI("volume %s, q15 %" PRId32, text, rtsp_volume_to_q15(volume));
#endif
  }
}

static void do_get_parameter(rtsp_conn_t *c, const rtsp_msg_t *m,
                             reply_t *r) {
  const char *cursor = (const char *)m->body;
  const char *end = cursor != NULL ? cursor + m->body_len : NULL;
  const char *line;
  size_t line_len;

  while (rtsp_next_line(&cursor, end, &line, &line_len)) {
    while (line_len > 0 && (line[line_len - 1] == ' ' || line[line_len - 1] == '\t')) {
      line_len--;
    }
    while (line_len > 0 && (*line == ' ' || *line == '\t')) {
      line++;
      line_len--;
    }
    if (line_len == 6 && strncmp(line, "volume", 6) == 0) {
      char text[24];
      int n;
      rtsp_format_volume(rtsp_core_current_volume(), text, sizeof(text));
      n = snprintf((char *)c->body, sizeof(c->body), "volume: %s\r\n", text);
      r->content_type = CT_TEXT_PARAMETERS;
      r->body_len = (size_t)n;
      return;
    }
  }
}

static void do_feedback(rtsp_conn_t *c, reply_t *r) {
  size_t n;
  if (!holds(c) || !s.stream) return;
  n = ap2_reply_feedback(c->body, sizeof(c->body), s.stream_type,
                         (double)s.sample_rate);
  if (n == 0) {
    RTSP_LOGW("feedback reply does not fit");
    return;
  }
  r->content_type = CT_BPLIST;
  r->body_len = n;
}

static void do_command(const rtsp_msg_t *m) {
  char type[64];
  if (is_bplist(m) &&
      ap2_bplist_string(m->body, m->body_len, "type", type, sizeof(type))) {
    RTSP_LOGI("/command type %s", type);
  } else {
    RTSP_LOGW("/command without a readable type");
  }
}

static void do_set_rate_anchor_time(rtsp_conn_t *c, const rtsp_msg_t *m,
                                    reply_t *r) {
  double rate;
  int64_t secs, frac, timeline, rtp;
  uint64_t ns;

  if (!holds(c)) return;
  if (!is_bplist(m) || !ap2_bplist_real(m->body, m->body_len, "rate", &rate)) {
    refuse(r, CODE_BAD_REQUEST, "SETRATEANCHORTIME without rate");
    return;
  }
  if (rate == 0.0) {
    sound_engine_pause();
    rtsp_events_emit(RTSP_EVENT_PAUSED);
    RTSP_LOGI("rate 0: pause");
    return;
  }
  if (!ap2_bplist_int(m->body, m->body_len, "networkTimeSecs", &secs) ||
      !ap2_bplist_int(m->body, m->body_len, "networkTimeFrac", &frac) ||
      !ap2_bplist_int(m->body, m->body_len, "networkTimeTimelineID",
                      &timeline) ||
      !ap2_bplist_int(m->body, m->body_len, "rtpTime", &rtp)) {
    refuse(r, CODE_BAD_REQUEST, "SETRATEANCHORTIME without the anchor");
    return;
  }
  ns = rtsp_network_time_ns((uint64_t)secs, (uint64_t)frac);
  sound_engine_set_anchor_time((uint64_t)timeline, ns, (uint32_t)rtp);
  sound_engine_set_playing(true);
  rtsp_events_emit(RTSP_EVENT_PLAYING);
  RTSP_LOGI("rate non-zero: play from rtp %" PRIu32, (uint32_t)rtp);
}

static void do_flush_buffered(rtsp_conn_t *c, const rtsp_msg_t *m,
                              reply_t *r) {
  int64_t until_seq, until_ts, from_seq, from_ts;
  bool have_from_ts;

  if (!holds(c)) return;
  if (!is_bplist(m) ||
      !ap2_bplist_int(m->body, m->body_len, "flushUntilSeq", &until_seq) ||
      !ap2_bplist_int(m->body, m->body_len, "flushUntilTS", &until_ts)) {
    refuse(r, CODE_BAD_REQUEST, "FLUSHBUFFERED without flushUntilSeq/TS");
    return;
  }
  if (ap2_bplist_int(m->body, m->body_len, "flushFromSeq", &from_seq)) {
    have_from_ts =
        ap2_bplist_int(m->body, m->body_len, "flushFromTS", &from_ts);
    if (!have_from_ts) from_ts = 0;
    if (sound_engine_set_deferred_flush_range(
            (uint32_t)from_seq, (uint32_t)from_ts, (uint32_t)until_seq,
            (uint32_t)until_ts, have_from_ts) != ESP_OK) {
      refuse(r, CODE_INTERNAL, "deferred flush refused by the engine");
      return;
    }
    RTSP_LOGI("deferred flush %" PRIu32 "..%" PRIu32, (uint32_t)from_seq,
              (uint32_t)until_seq);
    return;
  }
  sound_engine_set_immediate_flush((uint32_t)until_seq, (uint32_t)until_ts);
  RTSP_LOGI("flush until %" PRIu32, (uint32_t)until_seq);
}

static void do_flush(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  const char *info = rtsp_msg_header(m, "RTP-Info");
  uint32_t rtptime;
  if (!holds(c)) return;
  if (info == NULL || !rtsp_parse_rtp_info(info, &rtptime)) {
    refuse(r, CODE_BAD_REQUEST, "FLUSH without RTP-Info rtptime");
    return;
  }
  sound_engine_realtime_flush_to_rtp(rtptime);
  RTSP_LOGI("realtime flush to rtp %" PRIu32, rtptime);
}

static void do_pause(rtsp_conn_t *c) {
  if (!holds(c)) return;
  sound_engine_pause();
  rtsp_events_emit(RTSP_EVENT_PAUSED);
}

static bool body_loggable(const char *method, const char *uri) {
  if (strcmp(method, "SETUP") == 0) return false; /* shk */
  if (strcmp(method, "POST") == 0 &&
      (path_is(uri, "/pair-setup") || path_is(uri, "/pair-verify") ||
       path_is(uri, "/fp-setup") || path_is(uri, "/configure") ||
       path_is(uri, "/pair-add") || path_is(uri, "/pairings"))) {
    return false;
  }
  return true;
}

/* One line per request and per reply only with CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
 * (spec "Log"): /feedback alone comes every 2 s. */
static void log_request(const rtsp_conn_t *c, const rtsp_msg_t *m) {
#ifdef CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
  const char *cseq = rtsp_msg_header(m, "CSeq");
  RTSP_LOGI("[%u] %s %s CSeq %s body %zu", c->id, m->start[0], m->start[1],
            cseq != NULL ? cseq : "-", m->body_len);
  if (m->body != NULL && body_loggable(m->start[0], m->start[1])) {
    char text[BODY_LOG_BYTES + 1];
    size_t n = m->body_len < BODY_LOG_BYTES ? m->body_len : BODY_LOG_BYTES;
    for (size_t i = 0; i < n; i++) {
      char ch = (char)m->body[i];
      text[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '.';
    }
    text[n] = '\0';
    RTSP_LOGI("[%u] body: %s", c->id, text);
  }
#else
  (void)c;
  (void)m;
  (void)body_loggable;
#endif
}

static void handle(rtsp_conn_t *c, const rtsp_msg_t *m, reply_t *r) {
  const char *method = m->start[0];
  const char *uri = m->start[1];

  log_request(c, m);
  if (s.homekit_reset_pending) {
    r->close = true;
    refuse(r, 503, "HomeKit reset in progress");
    return;
  }
  if (c->closing) {
    r->close = true;
    refuse(r, CODE_AUTH_REQUIRED, "connection is closing");
    return;
  }
  if (m->body_state == RTSP_BODY_TOO_LARGE) {
    refuse(r, CODE_TOO_LARGE, "body over the limit");
    return;
  }
  if (m->body_state == RTSP_BODY_NO_MEMORY) {
    refuse(r, CODE_INTERNAL, "no memory for the body");
    return;
  }

  if (strcmp(method, "OPTIONS") == 0) {
    r->extra = PUBLIC_METHODS;
  } else if (strcmp(method, "GET") == 0) {
    if (path_is(uri, "/info")) {
      do_info(c, r);
    } else {
      not_implemented(c, r, "GET of an unknown path");
    }
  } else if (strcmp(method, "POST") == 0) {
    if (path_is(uri, "/pair-setup")) {
      do_pair(c, m, r, true);
    } else if (path_is(uri, "/pair-verify")) {
      do_pair(c, m, r, false);
    } else if (path_is(uri, "/fp-setup")) {
      do_fp_setup(c, m, r);
#ifdef CONFIG_AIRPLAY_HOMEKIT
    } else if (path_is(uri, "/configure")) {
      do_configure(c, m, r);
    } else if (path_is(uri, "/pair-add") || path_is(uri, "/pair-remove") ||
               path_is(uri, "/pair-list") || path_is(uri, "/pairings")) {
      do_pairings(c, m, r);
#endif
    } else if (path_is(uri, "/feedback")) {
      do_feedback(c, r);
    } else if (path_is(uri, "/command")) {
      do_command(m);
    } else if (path_is(uri, "/audioMode")) {
      /* accepted */
    } else {
      not_implemented(c, r, "POST to an unknown path");
    }
  } else if (strcmp(method, "SETUP") == 0) {
    do_setup(c, m, r);
  } else if (strcmp(method, "TEARDOWN") == 0) {
    do_teardown(c, m, r);
  } else if (strcmp(method, "SET_PARAMETER") == 0) {
    do_set_parameter(c, m, r);
  } else if (strcmp(method, "GET_PARAMETER") == 0) {
    do_get_parameter(c, m, r);
  } else if (strcmp(method, "SETRATEANCHORTIME") == 0) {
    do_set_rate_anchor_time(c, m, r);
  } else if (strcmp(method, "FLUSHBUFFERED") == 0) {
    do_flush_buffered(c, m, r);
  } else if (strcmp(method, "FLUSH") == 0) {
    do_flush(c, m, r);
  } else if (strcmp(method, "PAUSE") == 0) {
    do_pause(c);
  } else if (strcmp(method, "RECORD") == 0 ||
             strcmp(method, "SETPEERS") == 0 ||
             strcmp(method, "SETPEERSX") == 0 ||
             strcmp(method, "LOUDNESSNORMALIZATION") == 0) {
    /* 200, nothing changes */
  } else {
    not_implemented(c, r, method);
  }
}

/* ---- Connection ---- */

rtsp_conn_t *rtsp_conn_new(uint32_t peer_ip, void *port_ctx) {
  rtsp_conn_t *c = rtsp_port_malloc(sizeof(*c));
  if (c == NULL) return NULL;
  memset(c, 0, sizeof(*c));
  c->peer_ip = peer_ip;
  c->port_ctx = port_ctx;
  rtsp_reader_init(&c->reader, RTSP_BODY_MAX);
  rtsp_port_lock();
  c->id = ++s.next_conn_id;
  c->next = s.connections;
  s.connections = c;
  rtsp_port_unlock();
  return c;
}

void *rtsp_conn_port_ctx(const rtsp_conn_t *conn) { return conn->port_ctx; }

/* One message: plain as it is, encrypted as rtsp_frame_gather() cuts it. */
static int send_message(rtsp_conn_t *c, const uint8_t *head, size_t head_len,
                        const uint8_t *body, size_t body_len,
                        rtsp_send_fn send, void *ctx) {
  size_t off = 0, n;
  if (!c->encrypted) {
    if (send(ctx, head, head_len) != 0) return -1;
    return body_len > 0 ? send(ctx, body, body_len) : 0;
  }
  while ((n = rtsp_frame_gather(head, head_len, body, body_len, off,
                                c->frame + 2)) > 0) {
    size_t frame_len;
    c->out_cipher.counter = ap2_hap_channel_counter(c->hap, AP2_HAP_SEND);
    frame_len = rtsp_frame_seal(&c->out_cipher, c->frame + 2, n, c->frame);
    ap2_hap_channel_count(c->hap, AP2_HAP_SEND);
    if (send(ctx, c->frame, frame_len) != 0) return -1;
    off += n;
  }
  return 0;
}

static int send_reply(rtsp_conn_t *c, const char *cseq, const reply_t *r,
                      rtsp_send_fn send, void *ctx) {
  char extra[sizeof(PUBLIC_METHODS) + 32];
  size_t head_len;

  snprintf(extra, sizeof(extra), "%s%s", r->extra != NULL ? r->extra : "",
           r->close ? "Connection: close\r\n" : "");
  head_len = rtsp_reply_head(c->head, sizeof(c->head), r->code, cseq, extra,
                             r->content_type, r->body_len, c->reply_http);
  if (head_len == 0) return -1;
#ifdef CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
  RTSP_LOGI("[%u] reply %d body %zu", c->id, r->code, r->body_len);
#endif
  return send_message(c, (const uint8_t *)c->head, head_len, c->body,
                      r->body_len, send, ctx);
}

static void switch_to_encrypted(rtsp_conn_t *c) {
  rtsp_cipher_init(&c->out_cipher, ap2_hap_channel_key(c->hap, AP2_HAP_SEND));
  rtsp_frame_reader_init(&c->in_frames,
                         ap2_hap_channel_key(c->hap, AP2_HAP_RECEIVE));
  c->switch_pending = false;
  c->encrypted = true;
}

/* Plain bytes of requests. Stops early, with *used short of len, when the
 * connection becomes encrypted: the rest is ciphertext. */
static int process_plain(rtsp_conn_t *c, const uint8_t *data, size_t len,
                         size_t *used_out, rtsp_send_fn send, void *ctx) {
  size_t used = 0;
  bool was_encrypted = c->encrypted;

  while (used < len) {
    size_t n = 0;
    rtsp_read_result_t rr =
        rtsp_reader_feed(&c->reader, data + used, len - used, &n);
    used += n;
    if (rr == RTSP_READ_MORE) break;
    if (rr == RTSP_READ_BAD) {
      reply_t r = {.code = CODE_BAD_REQUEST, .close = true};
      RTSP_LOGW("[%u] malformed request, closing", c->id);
      send_reply(c, NULL, &r, send, ctx);
      *used_out = used;
      return -1;
    }
    {
      reply_t r = {.code = 200};
      const char *cseq = rtsp_msg_header(&c->reader.msg, "CSeq");
      int rc;
      if (c->reader.msg.start[2][0] == '\0') {
        /* a request line without version cannot be read either */
        r.code = CODE_BAD_REQUEST;
        r.close = true;
        RTSP_LOGW("[%u] request line without version, closing", c->id);
        send_reply(c, cseq, &r, send, ctx);
        rtsp_reader_next(&c->reader);
        *used_out = used;
        return -1;
      }
      c->reply_http = rtsp_str_ieq(c->reader.msg.start[2], "HTTP/1.1") ||
                      rtsp_str_ieq(c->reader.msg.start[2], "HTTP/1.0");
#ifdef CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
      /* Timing and memory of each pair-verify step, with the request log. */
      const bool verify_trace = path_is(c->reader.msg.start[1], "/pair-verify");
#else
      const bool verify_trace = false;
#endif
      rtsp_pairing_memory_t mem_before = {0}, mem_after;
      uint32_t arrived = 0, locked = 0, handled = 0;
      if (verify_trace) {
        rtsp_port_pairing_memory(&mem_before);
        arrived = rtsp_port_now_ms();
      }
      rtsp_port_lock();
      if (verify_trace) locked = rtsp_port_now_ms();
      handle(c, &c->reader.msg, &r);
      rtsp_port_unlock();
      if (verify_trace) handled = rtsp_port_now_ms();
      rc = send_reply(c, cseq, &r, send, ctx);
      if (verify_trace) {
        const uint32_t sent = rtsp_port_now_ms();
        rtsp_port_pairing_memory(&mem_after);
        const uint8_t *pk = ap2_hap_identity_public_key();
        /* Log only after sending: do not add console latency to M2. Key
         * bytes below are a public identity tag, never session/private keys. */
        RTSP_LOGI("[%u] pair-verify reply=%d body=%zu send=%d wait=%" PRIu32
                  "ms work=%" PRIu32 "ms tx=%" PRIu32 "ms pk=%02x%02x%02x%02x",
                  c->id, r.code, r.body_len, rc, locked - arrived,
                  handled - locked, sent - handled, pk[0], pk[1], pk[2], pk[3]);
        RTSP_LOGI("[%u] pair-verify RAM bytes before/after: internal=%" PRIu32
                  "/%" PRIu32 " largest=%" PRIu32 "/%" PRIu32
                  " PSRAM=%" PRIu32 "/%" PRIu32 " largest=%" PRIu32 "/%" PRIu32,
                  c->id, mem_before.internal_free, mem_after.internal_free,
                  mem_before.internal_largest, mem_after.internal_largest,
                  mem_before.psram_free, mem_after.psram_free,
                  mem_before.psram_largest, mem_after.psram_largest);
      }
#ifdef CONFIG_AIRPLAY_HOMEKIT
      /* The removal was authenticated before this point. Complete it even
       * if the peer disconnects while its final response is being sent. */
      if (r.homekit_reset) rtsp_port_homekit_reset();
#endif
      publish_pending(); /* after the reply, outside the lock */
      rtsp_reader_next(&c->reader);
      if (rc != 0 || r.close) {
        *used_out = used;
        return -1;
      }
    }
    if (c->switch_pending) {
      switch_to_encrypted(c);
      if (!was_encrypted) break;
    }
  }
  *used_out = used;
  return 0;
}

int rtsp_conn_receive(rtsp_conn_t *c, const uint8_t *data, size_t len,
                      rtsp_send_fn send, void *ctx) {
  size_t off = 0;
  if (c->closing) return -1;
  while (off < len) {
    size_t used = 0;
    if (!c->encrypted) {
      if (process_plain(c, data + off, len - off, &used, send, ctx) != 0) {
        return -1;
      }
      off += used;
      if (!c->encrypted) break; /* all consumed */
    } else {
      const uint8_t *plain = NULL;
      size_t plain_len = 0, plain_used = 0;
      rtsp_frame_result_t fr;
      c->in_frames.cipher.counter =
          ap2_hap_channel_counter(c->hap, AP2_HAP_RECEIVE);
      fr = rtsp_frame_feed(&c->in_frames, data + off, len - off, &used, &plain,
                           &plain_len);
      off += used;
      if (fr == RTSP_FRAME_ERROR) {
        RTSP_LOGW("[%u] frame does not open, closing", c->id);
        return -1;
      }
      if (fr == RTSP_FRAME_MORE) break;
      ap2_hap_channel_count(c->hap, AP2_HAP_RECEIVE);
      if (process_plain(c, plain, plain_len, &plain_used, send, ctx) != 0) {
        return -1;
      }
    }
    if (c->closing) return -1;
  }
  return 0;
}

void rtsp_conn_end(rtsp_conn_t *c) {
  if (c == NULL) return;
  rtsp_port_lock();
  if (s.holder == c) {
    RTSP_LOGI("connection %u of the session closed", c->id);
    session_end();
  }
  event_port_close(c);
  rtsp_conn_t **link = &s.connections;
  while (*link && *link != c) link = &(*link)->next;
  if (*link) *link = c->next;
  rtsp_port_unlock();
  publish_pending(); /* a session that ended with its connection */
  rtsp_reader_next(&c->reader);
  ap2_hap_session_delete(c->hap);
  wipe(c, sizeof(*c));
  rtsp_port_free(c);
}

