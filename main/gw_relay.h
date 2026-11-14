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
 * starts at a frame boundary, with the slowest connected speaker.
 *
 * The sender is read whenever there is room in a 4 MiB buffer in PSRAM;
 * each speaker is sent from it at its own speed, at most twice real time
 * after a start at full speed (pacing keeps the Wi-Fi calm). At a FLUSH
 * (gw_relay_flush) the old track is in the buffer, not queued in TCP: each
 * speaker's stream skips, at its next frame boundary, to the first frame
 * from the FLUSH's sequence number, at once. A speaker that keeps old audio
 * longest holds the buffer; when it is full the sender waits. While a
 * speaker is starting its listener or being connected, the buffer keeps
 * the audio from then on for up to ~1 s so it does not miss the start. */

/* Idempotent while running: returns the port already bound. */
esp_err_t gw_relay_start(uint16_t requested_port, uint16_t *bound_port);
/* Closes the listener and the sender connection; waits up to ~1 s for the
 * relay task to exit. */
void gw_relay_stop(void);
bool gw_relay_is_idle(void);

/* The sender plays (gw_proxy.c, from SET_PLAYING / PAUSE / STOP). Only while
 * it plays must every speaker take audio: one that took none for
 * GW_RELAY_STALL_DROP_MS while the others took theirs, and whose audio fills
 * the Gateway's buffer (less than GW_RELAY_STALL_ROOM_BYTES free), is dropped
 * from the stream (connected again after GW_RELAY_STALL_RETRY_MS), so it
 * does not hold back the rest until their buffers run dry. Until the buffer
 * fills it is waited for, and a short Wi-Fi gap stays unheard. While paused
 * full FIFOs are normal. */
void gw_relay_set_playing(bool playing);

/* An immediate FLUSH (FLUSHBUFFERED without a start): the frames before
 * until_seq are not sent to the speakers any more (they drop them as well). */
void gw_relay_flush(uint32_t until_seq);
