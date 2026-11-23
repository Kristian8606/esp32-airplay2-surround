#pragma once

#include <stdbool.h>

#include "apple_clock.h"
#include "esp_err.h"
#include "esp_http_server.h"

/* Web radio on the Gateway: an addition beside AirPlay, not a change to it.
 *
 * Starting the radio ends the AirPlay session the way the room calibration
 * does (rtsp_server_stop) and starts the AirPlay listener again at once, so
 * the Gateway stays an AirPlay receiver. The radio then plays to the speakers
 * as a buffered (type 103) sender inside the Gateway: it reads the station
 * (HTTP/HTTPS, MP3 or AAC), encodes ALAC (48 kHz streams as 48 kHz / 24 bit,
 * others in their own rate at 16 bit), and makes the same sound_engine calls
 * RTSP makes, so the speakers get the same messages and the relay the same
 * frames as from an Apple sender. The speakers' clock line is the Gateway's
 * own clock while the radio plays.
 *
 * AirPlay wins: the first sound_engine call of a sender while the radio
 * plays stops the radio before the call goes on (gw_radio_yield). */

/* Play url (http:// or https://) with the shared volume, or set it from
 * volume_db (-60..0, GW_LOCAL_VOLUME_KEEP_DB to keep it). name and logo
 * (may be NULL)
 * are shown on the page. Stops a station that plays. ESP_ERR_INVALID_STATE
 * while the room calibration runs. Returns at once; /api/radio shows how the
 * start goes. */
esp_err_t gw_radio_start(const char *url, const char *name, const char *logo,
                         int volume_db);

/* Stops the radio and waits until it let go of the speakers (at most a few
 * seconds). Nothing when it does not play. Not from the radio's own task. */
void gw_radio_stop(void);

/* The radio holds the speakers (from the AirPlay hand-over until it let go). */
bool gw_radio_active(void);

/* gw_proxy.c, at the start of a sound_engine call: when the radio holds the
 * speakers and the call is not the radio's own, the radio stops first. */
void gw_radio_yield(void);

/* gw_speakers.c: while the radio holds the speakers, the line its TIME
 * messages carry (the Gateway's own clock); false otherwise. */
bool gw_radio_clock_line(apple_clock_line_t *out);

/* GET /api/radio (status), POST /api/radio/start {"url","name","logo","vol"},
 * POST /api/radio/stop, POST /api/radio/volume?pct=50 (legacy: db=-30), GET/POST
 * /api/radio/favs (a JSON array kept in NVS) and GET /radio.js (the page's
 * radio controls). What changes something asks the admin password
 * (web_admin_ok), like the other settings. */
void gw_radio_register_web(httpd_handle_t server);
