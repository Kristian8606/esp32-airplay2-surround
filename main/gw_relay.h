#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Gateway side of the AirPlay 2 buffered (type 103) audio TCP stream.
 *
 * The sender connects to the listener and streams frames of
 * [2-byte big-endian length incl. itself][12-byte RTP header][encrypted body].
 * The relay walks the frames for its statistics and copies every byte
 * unchanged to each speaker's buffered listener (gw_speakers_audio_targets()).
 *
 * Each speaker connection mirrors the sender connection: opened when the
 * sender connects, closed when it closes. A speaker that connects mid-stream
 * starts at the next frame boundary. The next chunk is read from the sender
 * only after every connected speaker took the current one, so the slowest
 * speaker paces the sender through TCP and the Gateway buffers nothing. While
 * a speaker is starting its listener or being connected, the sender is not
 * read for up to ~1 s so the speaker does not miss the start. */

/* Idempotent while running: returns the port already bound. */
esp_err_t gw_relay_start(uint16_t requested_port, uint16_t *bound_port);
/* Closes the listener and the sender connection; waits up to ~1 s for the
 * relay task to exit. */
void gw_relay_stop(void);
bool gw_relay_is_idle(void);

/* The sender plays (gw_proxy.c, from SET_PLAYING / PAUSE / STOP). Only while
 * it plays must every speaker take audio: then one that took none for
 * GW_RELAY_STALL_DROP_MS while the others took theirs is dropped from the
 * stream (connected again after GW_RELAY_STALL_RETRY_MS), so it does not
 * hold back the rest until their buffers run dry. While paused full FIFOs
 * are normal. */
void gw_relay_set_playing(bool playing);
