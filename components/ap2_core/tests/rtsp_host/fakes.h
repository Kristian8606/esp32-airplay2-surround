/* SPDX-License-Identifier: Apache-2.0 */
/* Fakes of the firmware and ap2_core interfaces and of rtsp_port.h, for the
 * host tests. Every call that changes something is written to a call log
 * as text, e.g. "sound_engine_set_volume_q15(8192)". */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ---- Call log ---- */

void calls_clear(void);
int calls_count(void);
const char *calls_get(int index);
/* Index of the first call starting with prefix at or after from, or -1. */
int calls_find_from(int from, const char *prefix);
int calls_find(const char *prefix);
/* Number of calls starting with prefix. */
int calls_with(const char *prefix);
void calls_dump(void);

/* ---- Log ---- */

int log_warnings(void);
bool log_contains(const char *text);
void log_clear(void);

/* ---- Behaviour ---- */

typedef struct {
  float saved_volume;         /* in flash: settings_get_volume */
  bool saved_volume_missing;
  float ram_volume;           /* settings_set_volume */
  bool ram_volume_set;
  const char *device_name;
  bool malloc_fail;          /* rtsp_port_malloc returns NULL */
  esp_err_t event_open_err;
  esp_err_t udp_pair_err;
  esp_err_t start_stream_err;
  esp_err_t start_buffered_err;
  int hap_mode;              /* 0 normal, 1 refuse (ESP_OK + error TLV), 2 ESP_FAIL */
  bool audio_key_fail;
  bool hap_new_fail;         /* ap2_hap_session_new returns NULL */
  uint32_t now_ms;
  int lock_depth;            /* checked to be 0 between calls */
  uint8_t event_write_key[32]; /* the keys of the last event_open */
  uint8_t event_read_key[32];
  uint16_t next_event_port;
  uint64_t channel_counter_start; /* the core's control channel counters */
  int channel_key_early;          /* ap2_hap_channel_key before established */
  int fp_reply_len;               /* fp-setup M1 reply length, 0: 142 */
  uint64_t channel_counted[2];    /* ap2_hap_channel_count calls per direction */
} fake_t;

extern fake_t fake;

void fake_reset(void);

/* The pairing secret the fake core ends a pairing with: 64 bytes after
 * transient pair-setup, 32 after pair-verify. */
size_t fake_pairing_secret(bool transient, uint8_t *out);
/* Whether conn's event port listens now; its keys and port number if so
 * (each may be NULL). */
bool fake_event_port(const void *conn, uint8_t write_key[32], uint8_t read_key[32],
                     uint16_t *port);
/* The fake ap2_hkdf_sha512 without writing a call to the log. */
void fake_kdf(const char *salt, const uint8_t *secret, size_t secret_len, const char *info,
              uint8_t key[32]);

/* ---- Fake binary plist ----
 *
 * Bodies are "bplist00" and then text: root entries "key=value" separated
 * by ';'. Values: i<integer> (decimal, or 0x hex), r<real>, s<string>,
 * d<n> (n bytes of data, byte k = 0xA0 + k), and for "streams" an array
 * [{key=value,...}{...}]. Anything else makes every reader return false. */
