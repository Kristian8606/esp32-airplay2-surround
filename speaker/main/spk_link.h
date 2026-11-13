#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Speaker: the control link to the Gateway.
 *
 * Finds the Gateway (CONFIG_AIRPLAY_SURROUND_GATEWAY_HOST, else mDNS
 * _apsurround._tcp), connects, sends HELLO and makes every forwarded
 * sound_engine call locally. Reconnects whenever the link drops; a stream
 * in progress is stopped first. Call after sound_engine_init(). */
esp_err_t spk_link_start(void);

/* Web page (Wi-Fi scan, latency test): drop the link, stop the stream and
 * stay away until spk_link_resume(), so the audio engine can be released.
 * The speaker then reconnects and joins like a late speaker.
 * ESP_ERR_TIMEOUT: the link task has not settled within 3 s yet. */
esp_err_t spk_link_suspend(void);
/* No link and no stream. */
bool spk_link_idle(void);
void spk_link_resume(void);

/* A setting the Gateway shows (the role) changed on this speaker's own page:
 * report it on the link. */
void spk_link_config_changed(void);
