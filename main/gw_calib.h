#pragma once

/* Gateway: room calibration with the microphone (gw_mic.c) at the listening
 * seat. Each speaker in turn plays test chirps (CAL_PLAY) and says when they
 * left it on the access point's TSF; the Gateway finds them in its recording
 * and gets each speaker's travel time and level at the seat. From those it
 * suggests every speaker's delay (the farthest gets 0) and level trim (the
 * quietest gets 0 dB). Nothing changes on the speakers until
 * gw_calib_apply(). */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "surround_proto.h"

typedef enum {
  GW_CAL_IDLE = 0,
  GW_CAL_BUSY,
  GW_CAL_DONE,   /* results (or the microphone test) ready */
  GW_CAL_FAILED, /* message says why */
} gw_cal_state_t;

typedef struct {
  uint8_t mac[6];
  char name[24];
  int8_t channel;     /* role, aac_speaker_t */
  bool measured;      /* heard clearly */
  bool low;           /* subwoofer chirp */
  bool inverted;      /* the chirp came back upside down: a hint only (+/-
                       * swapped, or a 2-way speaker's tweeter wired so) */
  float tof_ms;       /* travel time to the microphone */
  float distance_m;
  float level_db;     /* level at the seat relative to the level played:
                       * RMS of the chirp and its first reflections */
  float level_spread_db; /* level spread over the chirps */
  float match;        /* how well the recording matched the chirp, 0..1 */
  float spread_ms;    /* travel time spread over the chirps */
  bool has_suggestion;
  float delay_ms;     /* suggested delay */
  bool has_trim;
  float trim_db;      /* suggested level trim */
  char note[64];
} gw_cal_speaker_t;

typedef struct {
  gw_cal_state_t state;
  bool mic_built;     /* built with the microphone */
  bool mic_test;      /* the last run was only the microphone test */
  char message[160];
  uint8_t step, steps;
  bool sender_stopped; /* the run ended the sender's session */
  bool noise_valid;
  float noise_dbfs;   /* room noise, RMS */
  float peak_dbfs;    /* loudest sample of the noise recording */
  uint8_t count;
  gw_cal_speaker_t spk[SURROUND_MAX_SPEAKERS];
} gw_cal_status_t;

/* Start in the background: the microphone test alone, or the full run.
 * The full run ends a sender's session first (the speakers can only play
 * chirps between streams); AirPlay is back when it ends. ESP_ERR_INVALID_STATE
 * while one runs. */
esp_err_t gw_calib_start(bool mic_test_only);
void gw_calib_get(gw_cal_status_t *out);
/* Send the suggestions to the speakers (they save them like a manual edit).
 * Returns how many speakers took them through *applied. */
esp_err_t gw_calib_apply(int *applied);
void gw_calib_discard(void);
/* A run (test or measurement) is under way: speakers must not be disturbed. */
bool gw_calib_running(void);

/* gw_speakers.c: a speaker's CAL_RESULT (called with its lock held). */
void gw_calib_on_result(const uint8_t mac[6], const surround_cal_result_t *r);
