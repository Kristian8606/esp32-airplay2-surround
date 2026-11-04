/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ls_core.h"

#include <string.h>

/* Attempts to build a frame while writers keep overwriting what was read. */
#define LS_READ_TRIES 4
/* Continuation bytes a character can have. */
#define LS_UTF8_MAX_CONT 3

int ls_ring_init(ls_ring_t *r, uint8_t *buf, size_t cap)
{
    if (cap == 0 || (cap & (cap - 1)) != 0) {
        return -1;
    }
    r->buf = buf;
    r->cap = cap;
    r->head = 0;
    return 0;
}

uint64_t ls_ring_tail(const ls_ring_t *r)
{
    return r->head > r->cap ? r->head - r->cap : 0;
}

void ls_ring_write(ls_ring_t *r, const uint8_t *data, size_t len)
{
    if (len > r->cap) {
        r->head += len - r->cap;
        data += len - r->cap;
        len = r->cap;
    }
    size_t at = (size_t)(r->head & (r->cap - 1));
    size_t first = r->cap - at;
    if (first > len) {
        first = len;
    }
    memcpy(r->buf + at, data, first);
    memcpy(r->buf, data + first, len - first);
    r->head += len;
}

void ls_capture(ls_ring_t *r, const uint8_t *data, size_t full_len)
{
    if (full_len > LS_MAX_OUTPUT) {
        ls_ring_write(r, data, LS_MAX_OUTPUT);
        ls_ring_write(r, (const uint8_t *)"?", 1);
    } else {
        ls_ring_write(r, data, full_len);
    }
}

void ls_ring_copy(const ls_ring_t *r, uint64_t from, uint8_t *dst, size_t n)
{
    size_t at = (size_t)(from & (r->cap - 1));
    size_t first = r->cap - at;
    if (first > n) {
        first = n;
    }
    memcpy(dst, r->buf + at, first);
    memcpy(dst + first, r->buf, n - first);
}

/* Length of the sequence led by b and the allowed range of its second byte
 * (Unicode table 3-7); 0 if b cannot start a sequence. */
static int utf8_lead(uint8_t b, uint8_t *lo, uint8_t *hi)
{
    *lo = 0x80;
    *hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        return 2;
    }
    if (b >= 0xE0 && b <= 0xEF) {
        if (b == 0xE0) {
            *lo = 0xA0;
        } else if (b == 0xED) {
            *hi = 0x9F;
        }
        return 3;
    }
    if (b >= 0xF0 && b <= 0xF4) {
        if (b == 0xF0) {
            *lo = 0x90;
        } else if (b == 0xF4) {
            *hi = 0x8F;
        }
        return 4;
    }
    return 0;
}

size_t ls_utf8_clean(const uint8_t *in, size_t n, uint8_t *out, size_t out_cap,
                     size_t *consumed)
{
    size_t i = 0;
    size_t o = 0;
    while (i < n && o < out_cap) {
        uint8_t b = in[i];
        if (b < 0x80) {
            out[o++] = b;
            i++;
            continue;
        }
        uint8_t lo, hi;
        int len = utf8_lead(b, &lo, &hi);
        int k = 1;
        while (len && k < len && i + k < n) {
            uint8_t c = in[i + k];
            if (c < lo || c > hi) {
                break;
            }
            lo = 0x80;
            hi = 0xBF;
            k++;
        }
        if (len && k == len) {
            if (o + len > out_cap) {
                break; /* waits for the next frame */
            }
            memcpy(out + o, in + i, len);
            o += len;
            i += len;
        } else if (len && i + k == n) {
            break; /* incomplete so far: wait for the rest */
        } else {
            out[o++] = '?';
            i++;
        }
    }
    *consumed = i;
    return o;
}

static uint64_t locked_tail(ls_ring_t *r, const ls_lock_t *lk, uint64_t *head)
{
    lk->lock(lk->arg);
    uint64_t tail = ls_ring_tail(r);
    if (head) {
        *head = r->head;
    }
    lk->unlock(lk->arg);
    return tail;
}

void ls_reader_start(ls_reader_t *rd, ls_ring_t *r, const ls_lock_t *lk)
{
    rd->pos = locked_tail(r, lk, NULL);
    rd->skip = rd->pos > 0 ? LS_UTF8_MAX_CONT : 0;
}

size_t ls_read_frame(ls_ring_t *r, const ls_lock_t *lk, ls_reader_t *rd,
                     uint8_t *raw, uint8_t *out, size_t out_cap)
{
    for (int tries = 0; tries < LS_READ_TRIES; tries++) {
        uint64_t head;
        uint64_t tail = locked_tail(r, lk, &head);
        int gap = rd->pos < tail;
        uint64_t from = gap ? tail : rd->pos;
        uint8_t skip = gap ? LS_UTF8_MAX_CONT : rd->skip;
        size_t room = out_cap - (size_t)gap;
        size_t n = head - from < room ? (size_t)(head - from) : room;

        ls_ring_copy(r, from, raw, n);
        if (from < locked_tail(r, lk, NULL)) {
            continue; /* overwritten while copying: now behind, a gap */
        }

        size_t s = 0;
        while (s < n && skip > 0 && (raw[s] & 0xC0) == 0x80) {
            s++;
            skip--;
        }
        if (s < n) {
            skip = 0; /* reached text that is not a leftover */
        }

        size_t used;
        if (gap) {
            out[0] = '?';
        }
        size_t len = ls_utf8_clean(raw + s, n - s, out + gap, room, &used);
        rd->pos = from + s + used;
        rd->skip = skip;
        return len + (size_t)gap;
    }
    return 0;
}
