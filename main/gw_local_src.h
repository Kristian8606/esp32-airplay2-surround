#pragma once

/* The Gateway's own senders (web radio, USB player): what gw_proxy.c,
 * gw_speakers.c and gw_calib.c ask of them, in one place
 * (ROADMAP_USB_PLAYER.md, decision 8.9). Without CONFIG_GATEWAY_USB_PLAYER
 * these are the radio's own functions, inline: the build is as before.
 *
 * At most one of them holds the speakers: each stops the other before it
 * takes them, so neither ever waits in a yield for the other's task. */

#include <stdbool.h>

#include "apple_clock.h"
#include "gw_radio.h"
#include "sdkconfig.h"
#if CONFIG_GATEWAY_USB_PLAYER
#include "gw_player.h"
#endif

/* gw_proxy.c, at the start of every sound_engine call: an AirPlay sender's
 * call (or the other local sender's) stops the local sender that holds the
 * speakers first. */
static inline void gw_local_src_yield(void) {
  gw_radio_yield();
#if CONFIG_GATEWAY_USB_PLAYER
  gw_player_yield();
#endif
}

/* gw_speakers.c: the clock line TIME carries while a local sender holds
 * the speakers (the Gateway's own clock); false otherwise. */
static inline bool gw_local_src_clock_line(apple_clock_line_t *out) {
  if (gw_radio_clock_line(out)) return true;
#if CONFIG_GATEWAY_USB_PLAYER
  if (gw_player_clock_line(out)) return true;
#endif
  return false;
}

/* gw_calib.c: the room calibration needs the speakers free. */
static inline void gw_local_src_stop(void) {
  gw_radio_stop();
#if CONFIG_GATEWAY_USB_PLAYER
  gw_player_stop();
#endif
}
