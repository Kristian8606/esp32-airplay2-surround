#pragma once

/* REW measuring mode (ROADMAP_REW_AND_ALIGNMENT.md, phase 1): REW on a Mac
 * measures through AirPlay to the Gateway as usual, and only the marked
 * speaker (or one of its drivers) plays the source's L+R; every other
 * speaker is silent but stays in the stream and in sync.
 *
 * Nothing is saved: the mode ends with stop, 15 minutes after the last
 * action, a Gateway restart, or when the web radio or the USB player
 * starts. It does not start while one of them plays or a calibration runs,
 * and a calibration does not start while it is on.
 *
 * GET /api/rew: the state. POST /api/rew (admin password):
 *   {"action":"start"}                      nobody plays yet, EQ on
 *   {"action":"select","mac":"..","way":"full|woofer|tweeter"}
 *   {"action":"select","mac":""}            nobody plays
 *   {"action":"set","eq_on":true,"invert_tweeter":false}
 *   {"action":"stop"}
 * start also takes "eq_on" and "invert_tweeter". */

#include <stdbool.h>

#include "esp_http_server.h"
#include "surround_proto.h"

#if defined(SURROUND_PROTO_MEASURE)
bool gw_rew_active(void);
/* A local sender starts (web radio, USB player): the mode ends, logged. */
void gw_rew_stop_for(const char *why);
void gw_rew_register_web(httpd_handle_t server);
#else
static inline bool gw_rew_active(void) { return false; }
static inline void gw_rew_stop_for(const char *why) { (void)why; }
static inline void gw_rew_register_web(httpd_handle_t server) { (void)server; }
#endif
