/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Capture and text logic of log_stream, without ESP-IDF: the history ring,
 * reader positions, gaps, the 1024-byte cut and the UTF-8 cleaning. Builds
 * with a plain C compiler so it can be tested on a computer.
 *
 * Positions are absolute: byte n of everything ever captured has position n.
 * The ring keeps positions [tail, head), tail = head - cap (or 0).
 *
 * Nothing here locks. The caller serialises writers and passes a lock to
 * ls_read_frame, which holds it only to read positions, never while copying.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LS_HISTORY_SIZE (128u * 1024u) /* bytes of history kept */
#define LS_MAX_OUTPUT   1024u          /* bytes kept of one output */
#define LS_FRAME_MAX    4096u          /* bytes in one WebSocket frame */

typedef struct {
    uint8_t *buf;
    size_t cap;    /* power of two */
    uint64_t head; /* bytes ever written */
} ls_ring_t;

/* One browser's place in the stream. skip > 0: the reader starts after
 * dropped text, so up to skip leading continuation bytes are leftovers of a
 * character whose first byte was dropped and are skipped. */
typedef struct {
    uint64_t pos;
    uint8_t skip;
} ls_reader_t;

typedef struct {
    void (*lock)(void *arg);
    void (*unlock)(void *arg);
    void *arg;
} ls_lock_t;

/* Returns 0, or -1 if cap is not a power of two. */
int ls_ring_init(ls_ring_t *r, uint8_t *buf, size_t cap);

/* Oldest position still kept. */
uint64_t ls_ring_tail(const ls_ring_t *r);

/* Appends bytes as they are. If len > cap only the last cap bytes stay. */
void ls_ring_write(ls_ring_t *r, const uint8_t *data, size_t len);

/* Appends one logging output of full_len bytes. data holds at least
 * min(full_len, LS_MAX_OUTPUT) bytes. Longer outputs are kept as their
 * first LS_MAX_OUTPUT bytes followed by '?'. */
void ls_capture(ls_ring_t *r, const uint8_t *data, size_t full_len);

/* Copies n bytes starting at position from (which must be kept) to dst. */
void ls_ring_copy(const ls_ring_t *r, uint64_t from, uint8_t *dst, size_t n);

/* Cleans in[0..n) into valid UTF-8 in out (at most out_cap bytes): invalid
 * bytes become '?', one per byte; ESC, tab, CR, LF and other ASCII pass.
 * A character that is still incomplete at the end of in is not consumed.
 * Sets *consumed to the input bytes used; returns the bytes written. */
size_t ls_utf8_clean(const uint8_t *in, size_t n, uint8_t *out, size_t out_cap,
                     size_t *consumed);

/* Starts a reader at the oldest kept text (the whole history). */
void ls_reader_start(ls_reader_t *rd, ls_ring_t *r, const ls_lock_t *lk);

/* Builds the next frame for rd into out (at most out_cap bytes,
 * out_cap >= 4) using raw (out_cap bytes) as scratch. If the reader fell
 * behind the history it continues at the tail and the frame starts with one
 * '?'. Continuation bytes left over after dropped text are skipped.
 * Advances rd, also when the frame is empty (skipped bytes). Returns the
 * frame length, 0 if nothing to send. */
size_t ls_read_frame(ls_ring_t *r, const ls_lock_t *lk, ls_reader_t *rd,
                     uint8_t *raw, uint8_t *out, size_t out_cap);
