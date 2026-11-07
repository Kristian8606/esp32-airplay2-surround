/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "rtsp_port.h"

/* Between rtsp_core.c and rtsp_remote.c. Call with the server lock held. */

/* The current AirPlay volume of the session (the saved one without a
 * session). */
double rtsp_core_current_volume(void);
/* An acknowledged volume command: the new volume to the engine. */
void rtsp_core_remote_volume(double volume);
/* The connection that holds the session, or NULL. */
rtsp_conn_t *rtsp_core_holder(void);
/* The session ended: no event connection any more; a command still going
 * ends disconnected. */
void rtsp_remote_detach(void);
/* The connection's event port closed: an event connection that came in on
 * it is done (its next poll returns -1). */
void rtsp_remote_conn_gone(const rtsp_conn_t *conn);
