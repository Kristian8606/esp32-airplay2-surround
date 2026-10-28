#pragma once

#include "esp_err.h"

/* Speaker in Surround mode: the control link to the Gateway.
 *
 * Finds the Gateway (CONFIG_AIRPLAY_SURROUND_GATEWAY_HOST, else mDNS
 * _apsurround._tcp), connects, sends HELLO and makes every forwarded
 * audio_receiver call locally. Reconnects whenever the link drops; a stream
 * in progress is stopped first. Replaces mDNS/RTSP in this mode; call after
 * audio_receiver_init(). */
esp_err_t spk_link_start(void);

/* Web page (Wi-Fi scan, latency test): drop the link, stop the stream and
 * stay away until spk_link_resume(), so the audio engine can be released.
 * The speaker then reconnects and joins like a late speaker. */
void spk_link_suspend(void);
void spk_link_resume(void);

/* A setting the Gateway shows (the role) changed on this speaker's own page:
 * report it on the link. */
void spk_link_config_changed(void);
