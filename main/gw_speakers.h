#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "surround_proto.h"

/* Gateway registry of the speakers' control links.
 *
 * Starts the control listener (SURROUND_CONTROL_PORT), advertises it by mDNS
 * and runs one task that accepts speakers, reads HELLO/PONG/START_RESULT and
 * pings them. Call after mdns_airplay_init(). */
esp_err_t gw_speakers_start(void);

/* Forward one audio_receiver call to the speakers. Never waits on a
 * socket: a speaker whose socket cannot take the whole frame at once is
 * dropped.
 *
 * A speaker that connects during a stream waits for the sender's next FLUSH:
 * on SEEK_FLUSH, DEFERRED_FLUSH or IMMEDIATE_FLUSH it first receives the
 * session state the other speakers already have (stream type, format, key,
 * start, playout latency, volume, anchor, play state), then the flush. */
void gw_speakers_send(surround_msg_type_t type, const void *payload,
                      size_t len);

/* Forward a volume later, from the speakers task (within one TIME interval).
 * Only atomics: safe inside a critical section, where RTSP sets the volume. */
void gw_speakers_post_volume(int32_t volume_q15);

/* One speaker for the web page (registered speakers only). */
typedef struct {
  char name[24];
  char ip[16];
  uint8_t mac[6];
  char firmware[24];
  const char *state; /* "active", "waiting for FLUSH" */
  bool config_valid;
  surround_config_t config;
  bool audio; /* has an audio port (a stream is on) */
  bool status_valid; /* a recent STATUS */
  surround_status_t status;
} gw_speaker_info_t;

size_t gw_speakers_list(gw_speaker_info_t out[SURROUND_MAX_SPEAKERS]);

/* Send SET_CONFIG to the speaker with this MAC; it answers CONFIG, which the
 * next gw_speakers_list() shows. ESP_ERR_NOT_FOUND when not connected. */
esp_err_t gw_speakers_set_config(const uint8_t mac[6],
                                 const surround_set_config_t *cfg);

/* Room calibration: ask the speaker with this MAC to play test chirps; its
 * CAL_RESULT goes to gw_calib_on_result(). ESP_ERR_NOT_FOUND when not
 * connected. */
esp_err_t gw_speakers_cal_play(const uint8_t mac[6],
                               const surround_cal_play_t *m);

/* This ESP's clock (esp_timer, ns) and the access point's TSF, through the
 * line the TIME messages use. False until it has enough samples. */
bool gw_speakers_local_to_tsf(int64_t local_ns, int64_t *tsf_ns);
bool gw_speakers_tsf_to_local(int64_t tsf_ns, int64_t *local_ns);

/* Where the relay copies the buffered audio stream, one entry per speaker
 * slot. */
typedef struct {
  /* Nonzero while the speaker has a buffered listener; a new value on every
   * START_RESULT, so the relay reconnects when the speaker restarted. */
  uint32_t id;
  uint32_t ip;  /* IPv4, network byte order */
  uint16_t port; /* buffered TCP listener, 0 for realtime */
  uint16_t rt_data_port; /* realtime UDP ports, 0 for buffered */
  uint16_t rt_control_port;
  /* START_BUFFERED sent, START_RESULT not back yet: the relay holds the
   * stream briefly so the speaker does not miss its start. */
  bool pending;
  int64_t pending_since_us;
  char name[24];
} gw_audio_target_t;

/* Changes whenever any entry of gw_speakers_audio_targets() changes. */
uint32_t gw_speakers_audio_generation(void);
void gw_speakers_audio_targets(gw_audio_target_t out[SURROUND_MAX_SPEAKERS]);
