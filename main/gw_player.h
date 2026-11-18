#pragma once

/* The Gateway's own player (ROADMAP_USB_PLAYER.md, phase 3;
 * CONFIG_GATEWAY_USB_PLAYER). Like the web radio it is a buffered sender
 * inside the Gateway, under its own clock line, but it sends one lane per
 * speaker role through the relay's lane mode: each speaker gets only its
 * own channel as one-channel ALAC (common/surround/surround_route.h).
 *
 * Phase 3a-3c: one file over HTTP, play and stop. WAV and FLAC (16 or 24
 * bit, 44.1 or 48 kHz; mono, stereo, 5.1 or 7.1), ALAC in M4A (the same),
 * MP3, AAC (ADTS) and AAC in M4A (mono or stereo). M4A needs its index
 * (moov) before the audio. AirPlay wins as with the radio: a
 * sender's first sound_engine call stops the player (gw_local_src.h). */

#include <stdbool.h>

#include "apple_clock.h"
#include "esp_err.h"
#include "esp_http_server.h"

/* Plays url (http://) at volume_db (-60..0). Stops the radio or a file
 * that plays. Returns at once; GET /api/player shows how it goes. */
esp_err_t gw_player_play(const char *url, int volume_db);

/* Phase 3d.3: a queue of up to 64 URLs from index start; same-format
 * tracks follow each other gapless. add puts one more at the end (also
 * while it plays); next and prev (to the start of the track after 3 s)
 * and jump go to another track at once. */
esp_err_t gw_player_play_queue(const char *const *urls, int n, int start, int volume_db);
esp_err_t gw_player_add(const char *url);
esp_err_t gw_player_next(void);
esp_err_t gw_player_prev(void);
esp_err_t gw_player_jump(int index);

/* Stops the player and waits until it let go of the speakers. Nothing when
 * it does not play. Not from the player's own task. */
void gw_player_stop(void);

void gw_player_set_volume(int volume_db);

/* Phase 3d: pause (true) or resume (false), and seek to a place in the
 * file (WAV and FLAC for now). ESP_ERR_INVALID_STATE when nothing plays. */
esp_err_t gw_player_pause(bool pause);
esp_err_t gw_player_seek(double position_s);

/* The player holds the speakers. */
bool gw_player_active(void);

/* See gw_local_src.h. */
void gw_player_yield(void);
bool gw_player_clock_line(apple_clock_line_t *out);

/* GET /api/player (status) and POST /api/player {"cmd":"play","url":...,
 * "vol":-30} | {"cmd":"stop"} | {"cmd":"volume","vol":-25} | {"cmd":"pause"}
 * | {"cmd":"resume"} | {"cmd":"seek","pos":95.5} | {"cmd":"play","urls":[...],
 * "index":0} | {"cmd":"add","url":...} | {"cmd":"next"} | {"cmd":"prev"} |
 * {"cmd":"jump","index":2}. The status shows the queue and its index. POST asks the
 * admin password like the other settings. */
void gw_player_register_web(httpd_handle_t server);
