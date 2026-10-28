#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Gateway side of the AirPlay 2 realtime (type 96) ALAC stream.
 *
 * The sender sends RTP audio to the data port and sync (D7) and
 * retransmitted packets to the control port, both UDP. The relay copies
 * every datagram unchanged to the same port kind of each speaker
 * (gw_speakers_audio_targets()). Speakers send their retransmit requests
 * (NACKs) to the Gateway's control port; the relay passes each distinct
 * request on to the sender's control port, and the answer comes back to the
 * Gateway's control port and goes to every speaker like any control packet. */

/* Binds exactly these ports (chosen by RTSP SETUP). Idempotent. */
esp_err_t gw_rt_relay_start(uint16_t data_port, uint16_t control_port);
void gw_rt_relay_stop(void);
bool gw_rt_relay_is_idle(void);
uint16_t gw_rt_relay_control_port(void);
/* Where NACKs go: the sender's control address (network byte order IP). */
void gw_rt_relay_set_sender_control(uint32_t ip, uint16_t port);
