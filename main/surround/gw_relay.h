#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Gateway side of the AirPlay 2 buffered (type 103) audio TCP stream.
 *
 * The sender connects to the listener and streams frames of
 * [2-byte big-endian length incl. itself][12-byte RTP header][encrypted body].
 * Step 1 walks the frames, counts them and discards the bytes; later steps
 * copy every frame unchanged to each speaker's buffered port. */

/* Idempotent while running: returns the port already bound. */
esp_err_t gw_relay_start(uint16_t requested_port, uint16_t *bound_port);
/* Closes the listener and the sender connection; waits up to ~1 s for the
 * relay task to exit. */
void gw_relay_stop(void);
bool gw_relay_is_idle(void);
