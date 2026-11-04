/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for ls_core: the examples of spec/spec.md, the history wrapping
 * around, several readers, falling behind, the 1024-byte cut and damaged
 * UTF-8. Build and run: make -C tests
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_core.h"

static int s_fail;
static int s_checks;
static int s_verbose;

#define CHECK(cond)                                                          \
    do {                                                                     \
        s_checks++;                                                          \
        if (!(cond)) {                                                       \
            s_fail++;                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

/* ---- helpers ------------------------------------------------------------ */

static void no_lock(void *arg) { (void)arg; }
static const ls_lock_t NO_LOCK = { no_lock, no_lock, NULL };

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

static void buf_add(buf_t *b, const uint8_t *p, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void buf_free(buf_t *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

/* Independent check: decodes code points and rejects overlong forms,
 * surrogates and values above U+10FFFF. */
static bool valid_utf8(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t c = s[i];
        size_t len;
        uint32_t min;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2, min = 0x80, c &= 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3, min = 0x800, c &= 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4, min = 0x10000, c &= 0x07;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
            c = (c << 6) | (s[i + k] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

/* True if p[0..n) (n < 4) can still become a valid character. */
static bool valid_prefix(const uint8_t *p, size_t n)
{
    uint8_t t[4];
    memcpy(t, p, n);
    for (size_t len = n + 1; len <= 4; len++) {
        for (int second = 0x80; second <= 0xBF; second++) {
            for (size_t k = n; k < len; k++) {
                t[k] = (k == 1) ? (uint8_t)second : 0x80;
            }
            if (valid_utf8(t, len)) {
                return true;
            }
        }
    }
    return false;
}

/* Reference cleaner built on valid_utf8. Returns output length; *used is
 * the input consumed (an incomplete character at the end is not). */
static size_t ref_clean(const uint8_t *in, size_t n, uint8_t *out, size_t *used)
{
    size_t i = 0, o = 0;
    while (i < n) {
        size_t len = 0;
        for (size_t l = 1; l <= 4 && i + l <= n; l++) {
            if (valid_utf8(in + i, l)) {
                len = l;
                break;
            }
        }
        if (len) {
            memcpy(out + o, in + i, len);
            o += len, i += len;
        } else if (n - i < 4 && valid_prefix(in + i, n - i)) {
            break;
        } else {
            out[o++] = '?';
            i++;
        }
    }
    *used = i;
    return o;
}

typedef struct {
    ls_reader_t rd;
    buf_t got;
    size_t frames;
    size_t gaps;
    bool frame_ok; /* every frame <= LS_FRAME_MAX and valid UTF-8 alone */
} reader_t;

static void reader_start(reader_t *rd, ls_ring_t *r)
{
    memset(rd, 0, sizeof(*rd));
    ls_reader_start(&rd->rd, r, &NO_LOCK);
    rd->frame_ok = true;
}

/* Reads one frame; returns its length. */
static size_t reader_step(reader_t *rd, ls_ring_t *r)
{
    static uint8_t raw[LS_FRAME_MAX], out[LS_FRAME_MAX];
    bool behind = rd->rd.pos < ls_ring_tail(r);
    size_t n = ls_read_frame(r, &NO_LOCK, &rd->rd, raw, out, LS_FRAME_MAX);
    if (n) {
        rd->frames++;
        rd->gaps += behind;
        if (n > LS_FRAME_MAX || !valid_utf8(out, n)) {
            rd->frame_ok = false;
        }
        buf_add(&rd->got, out, n);
    }
    return n;
}

static void reader_drain(reader_t *rd, ls_ring_t *r)
{
    while (reader_step(rd, r)) {
    }
}

static bool got_is(const reader_t *rd, const void *s, size_t n)
{
    return rd->got.len == n && memcmp(rd->got.data, s, n) == 0;
}

static uint8_t *s_hist;

static void ring_new(ls_ring_t *r)
{
    CHECK(ls_ring_init(r, s_hist, LS_HISTORY_SIZE) == 0);
}

static void log_str(ls_ring_t *r, const char *s)
{
    ls_capture(r, (const uint8_t *)s, strlen(s));
}

static void log_bytes(ls_ring_t *r, const void *p, size_t n)
{
    ls_capture(r, p, n);
}

/* ---- spec examples ------------------------------------------------------ */

static void test_example1(void)
{
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    reader_start(&a, &r);
    const char *line = "\x1b[0;32mI (100) x: hello\x1b[0m\n";
    log_str(&r, line);
    reader_drain(&a, &r);
    CHECK(got_is(&a, line, strlen(line)));
    CHECK(a.frame_ok);
    buf_free(&a.got);
}

static void test_example2(void)
{
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    log_str(&r, "I (5) a: one\n");
    log_str(&r, "I (6) a: two\n");
    reader_start(&a, &r);
    CHECK(reader_step(&a, &r) > 0);
    CHECK(got_is(&a, "I (5) a: one\nI (6) a: two\n", 26));
    CHECK(reader_step(&a, &r) == 0);
    buf_free(&a.got);
}

static void test_example3(void)
{
    static const uint8_t hol[] = { 0xD0, 0xA5, 0xD0, 0xBE, 0xD0, 0xBB };
    /* split after the second D0 and after the first D0 */
    for (size_t cut = 1; cut <= 3; cut += 2) {
        ls_ring_t r;
        reader_t a;
        ring_new(&r);
        reader_start(&a, &r);
        log_bytes(&r, hol, cut);
        reader_drain(&a, &r);
        CHECK(a.got.len == cut - 1);
        CHECK(a.got.len == 0 || a.got.data[a.got.len - 1] != 0xD0);
        log_bytes(&r, hol + cut, sizeof(hol) - cut);
        reader_drain(&a, &r);
        CHECK(got_is(&a, hol, sizeof(hol)));
        CHECK(a.frame_ok);
        buf_free(&a.got);
    }
}

static void test_example4_5(void)
{
    struct {
        const char *in;
        const char *out;
    } cases[] = {
        { "ab\xFF" "cd", "ab?cd" },
        { "\xD0" "A", "?A" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ls_ring_t r;
        reader_t a;
        ring_new(&r);
        reader_start(&a, &r);
        log_str(&r, cases[i].in);
        reader_drain(&a, &r);
        CHECK(got_is(&a, cases[i].out, strlen(cases[i].out)));
        buf_free(&a.got);
    }
}

static void fill_pattern(uint8_t *p, size_t n, uint64_t at)
{
    for (size_t i = 0; i < n; i++) {
        uint64_t x = at + i;
        p[i] = (x % 61 == 60) ? '\n' : (uint8_t)('A' + (x * 7) % 26);
    }
}

static void test_example6(void)
{
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    reader_start(&a, &r);
    log_str(&r, "I (1) a: start\n");
    CHECK(reader_step(&a, &r) == 15); /* reader at the end */

    uint8_t chunk[1000];
    uint64_t at = r.head;
    size_t total = 0;
    while (total < 200 * 1024) {
        fill_pattern(chunk, sizeof(chunk), at);
        log_bytes(&r, chunk, sizeof(chunk));
        at += sizeof(chunk);
        total += sizeof(chunk);
    }
    size_t before = a.got.len;
    reader_drain(&a, &r);
    CHECK(a.gaps == 1);
    CHECK(a.got.len - before == 1 + LS_HISTORY_SIZE);
    CHECK(a.got.data[before] == '?');
    uint8_t *expect = malloc(LS_HISTORY_SIZE);
    fill_pattern(expect, LS_HISTORY_SIZE, ls_ring_tail(&r));
    CHECK(memcmp(a.got.data + before + 1, expect, LS_HISTORY_SIZE) == 0);
    CHECK(a.frame_ok);
    free(expect);
    buf_free(&a.got);
}

static void test_example7_cut(void)
{
    static uint8_t big[3000];
    memset(big, 'x', sizeof(big));
    size_t lens[] = { 3000, 1025, 1024, 1023 };
    for (size_t i = 0; i < 4; i++) {
        ls_ring_t r;
        ring_new(&r);
        log_bytes(&r, big, lens[i]);
        size_t keep = lens[i] > 1024 ? 1025 : lens[i];
        CHECK(r.head == keep);
        uint8_t h[1025];
        ls_ring_copy(&r, 0, h, keep);
        bool ok = true;
        for (size_t k = 0; k < keep && k < 1024; k++) {
            ok = ok && h[k] == 'x';
        }
        CHECK(ok);
        if (lens[i] > 1024) {
            CHECK(h[1024] == '?');
        }
    }

    /* a cut inside a character: the half character becomes '?' too */
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    reader_start(&a, &r);
    memset(big, 'x', 1023);
    big[1023] = 0xD0;
    big[1024] = 0xA5;
    log_bytes(&r, big, 1025);
    reader_drain(&a, &r);
    CHECK(a.got.len == 1025);
    CHECK(a.got.data[1022] == 'x' && a.got.data[1023] == '?' && a.got.data[1024] == '?');
    CHECK(valid_utf8(a.got.data, a.got.len));
    buf_free(&a.got);
}

static void test_example8(void)
{
    ls_ring_t r;
    reader_t a, b;
    ring_new(&r);
    log_str(&r, "I (6) a: two\n");
    reader_start(&a, &r);
    reader_drain(&a, &r);
    log_str(&r, "I (7) a: three\n");
    reader_start(&b, &r);
    reader_drain(&b, &r);
    reader_drain(&a, &r);
    log_str(&r, "I (8) a: four\n");
    reader_drain(&a, &r);
    reader_drain(&b, &r);
    const char *all = "I (6) a: two\nI (7) a: three\nI (8) a: four\n";
    CHECK(got_is(&a, all, strlen(all)));
    CHECK(got_is(&b, all, strlen(all)));
    buf_free(&a.got);
    buf_free(&b.got);
}

/* ---- ring and readers --------------------------------------------------- */

static void test_ring_small_wrap(void)
{
    uint8_t mem[16], out[16];
    ls_ring_t r;
    CHECK(ls_ring_init(&r, mem, 12) == -1);
    CHECK(ls_ring_init(&r, mem, 16) == 0);
    ls_ring_write(&r, (const uint8_t *)"0123456789", 10);
    ls_ring_write(&r, (const uint8_t *)"abcdefghij", 10); /* wraps */
    CHECK(r.head == 20 && ls_ring_tail(&r) == 4);
    ls_ring_copy(&r, 4, out, 16);
    CHECK(memcmp(out, "456789abcdefghij", 16) == 0);
    ls_ring_write(&r, (const uint8_t *)"ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26); /* > cap */
    CHECK(r.head == 46 && ls_ring_tail(&r) == 30);
    ls_ring_copy(&r, 30, out, 16);
    CHECK(memcmp(out, "KLMNOPQRSTUVWXYZ", 16) == 0);
}

static void test_wrap_keep_up(void)
{
    /* one reader keeps up while the history wraps around several times */
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    reader_start(&a, &r);
    buf_t sent = { 0 };
    uint8_t chunk[700];
    for (int i = 0; i < 1000; i++) {
        fill_pattern(chunk, sizeof(chunk), r.head);
        log_bytes(&r, chunk, sizeof(chunk));
        buf_add(&sent, chunk, sizeof(chunk));
        if (i % 5 == 0) {
            reader_drain(&a, &r);
        }
    }
    reader_drain(&a, &r);
    CHECK(r.head > 4 * LS_HISTORY_SIZE);
    CHECK(a.gaps == 0);
    CHECK(got_is(&a, sent.data, sent.len));
    CHECK(a.frame_ok);
    buf_free(&a.got);
    buf_free(&sent);
}

static void test_boot_log(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    buf_t file = { 0 };
    uint8_t tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        buf_add(&file, tmp, n);
    }
    fclose(f);
    CHECK(file.len > 0);

    ls_ring_t r;
    reader_t early, late;
    ring_new(&r);
    reader_start(&early, &r);
    size_t start = 0;
    for (size_t i = 0; i < file.len; i++) { /* one output per line */
        if (file.data[i] == '\n') {
            log_bytes(&r, file.data + start, i + 1 - start);
            start = i + 1;
            reader_step(&early, &r);
        }
    }
    log_bytes(&r, file.data + start, file.len - start);
    reader_drain(&early, &r);
    reader_start(&late, &r);
    reader_drain(&late, &r);
    CHECK(got_is(&early, file.data, file.len));
    CHECK(got_is(&late, file.data, file.len));
    CHECK(early.frame_ok && late.frame_ok);
    buf_free(&early.got);
    buf_free(&late.got);
    buf_free(&file);
}

static void test_control_bytes_pass(void)
{
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    reader_start(&a, &r);
    const char *s = "\x1b[0;33mW (1) t:\ta\r\nb\x01\x7f\x1b[0m\n";
    log_str(&r, s);
    reader_drain(&a, &r);
    CHECK(got_is(&a, s, strlen(s)));
    buf_free(&a.got);
}

/* ---- UTF-8 -------------------------------------------------------------- */

static void test_utf8_cases(void)
{
    struct {
        const char *in;
        size_t in_len;
        const char *out;
        size_t used;
    } cases[] = {
        { "\xC0\x80", 2, "??", 2 },             /* overlong NUL */
        { "\xC1\xBF", 2, "??", 2 },             /* overlong */
        { "\xE0\x80\x80", 3, "???", 3 },        /* overlong 3 bytes */
        { "\xED\xA0\x80", 3, "???", 3 },        /* surrogate */
        { "\xF4\x90\x80\x80", 4, "????", 4 },   /* above U+10FFFF */
        { "\xF5\x80", 2, "??", 2 },
        { "\x80" "a", 2, "?a", 2 },             /* lone continuation */
        { "\xE2\x82" "A", 3, "??A", 3 },        /* incomplete, then ASCII */
        { "\xE2\x82\xAC", 3, "\xE2\x82\xAC", 3 }, /* euro sign */
        { "\xF0\x9F\x98\x80", 4, "\xF0\x9F\x98\x80", 4 },
        { "\xF4\x8F\xBF\xBF", 4, "\xF4\x8F\xBF\xBF", 4 }, /* U+10FFFF */
        { "a\xE2\x82", 3, "a", 1 },             /* incomplete at end: waits */
        { "a\xF0\x9F\x98", 4, "a", 1 },
        { "a\xE0", 2, "a", 1 },
        { "a\xE0\x80", 3, "a??", 3 },           /* cannot be completed */
        { "\xD0\xD0\xA5", 3, "?\xD0\xA5", 3 },  /* lead after lead */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t out[16];
        size_t used;
        size_t n = ls_utf8_clean((const uint8_t *)cases[i].in, cases[i].in_len, out,
                                 sizeof(out), &used);
        bool ok = n == strlen(cases[i].out) && memcmp(out, cases[i].out, n) == 0 &&
                  used == cases[i].used;
        if (!ok) {
            fprintf(stderr, "utf8 case %zu: got %zu bytes, used %zu\n", i, n, used);
        }
        CHECK(ok);
    }

    /* a character that does not fit in the frame waits for the next one */
    uint8_t out[3];
    size_t used;
    CHECK(ls_utf8_clean((const uint8_t *)"ab\xD0\xA5", 4, out, 3, &used) == 2 && used == 2);
}

static uint32_t s_rng = 12345;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static size_t put_cp(uint8_t *p, uint32_t c)
{
    if (c < 0x80) {
        p[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800) {
        p[0] = 0xC0 | (c >> 6), p[1] = 0x80 | (c & 0x3F);
        return 2;
    }
    if (c < 0x10000) {
        p[0] = 0xE0 | (c >> 12), p[1] = 0x80 | ((c >> 6) & 0x3F), p[2] = 0x80 | (c & 0x3F);
        return 3;
    }
    p[0] = 0xF0 | (c >> 18), p[1] = 0x80 | ((c >> 12) & 0x3F);
    p[2] = 0x80 | ((c >> 6) & 0x3F), p[3] = 0x80 | (c & 0x3F);
    return 4;
}

/* Valid text in random pieces, cut anywhere, reaches the reader unchanged. */
static void test_valid_text_random_cuts(void)
{
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    reader_start(&a, &r);
    buf_t sent = { 0 };
    for (int i = 0; i < 20000; i++) {
        uint8_t piece[64];
        size_t n = 0;
        size_t want = 1 + rnd() % 40;
        while (n < want) {
            uint32_t c;
            switch (rnd() % 4) {
            case 0: c = 0x20 + rnd() % 0x5F; break;
            case 1: c = 0x80 + rnd() % 0x780; break;
            case 2: c = 0x800 + rnd() % 0xF800; break;
            default: c = 0x10000 + rnd() % 0x100000; break;
            }
            if (c >= 0xD800 && c <= 0xDFFF) {
                c = 0x416;
            }
            n += put_cp(piece + n, c);
        }
        /* log it in two calls cut at a random byte */
        size_t cut = rnd() % (n + 1);
        log_bytes(&r, piece, cut);
        if (rnd() % 3 == 0) {
            reader_step(&a, &r);
        }
        log_bytes(&r, piece + cut, n - cut);
        buf_add(&sent, piece, n);
        if (rnd() % 4 == 0) {
            reader_step(&a, &r);
        }
        if (r.head - a.rd.pos > LS_HISTORY_SIZE / 2) {
            reader_drain(&a, &r);
        }
    }
    reader_drain(&a, &r);
    CHECK(a.gaps == 0);
    CHECK(got_is(&a, sent.data, sent.len));
    CHECK(a.frame_ok);
    buf_free(&a.got);
    buf_free(&sent);
}

/* Random bytes: always valid UTF-8, never longer than input + gaps, and a
 * reader that keeps up gets what the reference cleaner gives. */
static void test_random_bytes(void)
{
    for (int round = 0; round < 6; round++) {
        ls_ring_t r;
        reader_t rd[4];
        ring_new(&r);
        buf_t sent = { 0 };
        for (int k = 0; k < 4; k++) {
            reader_start(&rd[k], &r);
        }
        /* reader 0 keeps up, 1 reads sometimes, 2 rarely (falls behind),
         * 3 connects late */
        int every[4] = { 1, 7, 400, 3 };
        for (int i = 0; i < 8000; i++) {
            uint8_t piece[1100];
            size_t n = rnd() % (round == 5 ? 1100 : 80);
            for (size_t j = 0; j < n; j++) {
                uint32_t x = rnd();
                /* mix pure noise with bytes that look like UTF-8 */
                piece[j] = (round % 2) ? (uint8_t)x
                                       : (uint8_t)((x & 1) ? 0x80 + (x >> 8) % 0x78 : x >> 8);
            }
            log_bytes(&r, piece, n);
            buf_add(&sent, piece, n > LS_MAX_OUTPUT ? LS_MAX_OUTPUT : n);
            if (n > LS_MAX_OUTPUT) {
                buf_add(&sent, (const uint8_t *)"?", 1);
            }
            if (i == 4000) {
                reader_start(&rd[3], &r);
            }
            for (int k = 0; k < 4; k++) {
                if (k == 3 && i < 4000) {
                    continue;
                }
                if (i % every[k] == 0) {
                    reader_step(&rd[k], &r);
                }
            }
            if (r.head - rd[0].rd.pos > LS_HISTORY_SIZE / 2) {
                reader_drain(&rd[0], &r);
            }
        }
        for (int k = 0; k < 4; k++) {
            reader_drain(&rd[k], &r);
            CHECK(rd[k].frame_ok);
            CHECK(valid_utf8(rd[k].got.data, rd[k].got.len));
        }
        /* reader 0: equal to the reference cleaning of everything kept */
        uint8_t *ref = malloc(sent.len + 1);
        size_t used;
        size_t ref_len = ref_clean(sent.data, sent.len, ref, &used);
        CHECK(sent.len == r.head);
        CHECK(rd[0].gaps == 0);
        CHECK(rd[0].got.len == ref_len && memcmp(rd[0].got.data, ref, ref_len) == 0);
        CHECK(rd[0].rd.pos == used);
        /* the others: never longer than what they consumed plus their gaps */
        for (int k = 1; k < 4; k++) {
            CHECK(rd[k].got.len <= sent.len + rd[k].gaps);
        }
        if (round == 0) {
            CHECK(rd[2].gaps > 0); /* the slow one did fall behind */
        }
        free(ref);
        buf_free(&sent);
        for (int k = 0; k < 4; k++) {
            buf_free(&rd[k].got);
        }
    }
}

/* ---- leftovers after dropped text --------------------------------------- */

/* Fills the history so that its tail is n_cont continuation bytes of a
 * character whose lead byte has just been dropped, followed by "OK\n". */
static void fill_with_cut_char(ls_ring_t *r, size_t n_cont)
{
    static uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    while (r->head < LS_HISTORY_SIZE) {
        size_t n = LS_HISTORY_SIZE - r->head < sizeof(chunk) ? LS_HISTORY_SIZE - r->head
                                                             : sizeof(chunk);
        ls_ring_write(r, chunk, n);
    }
    uint8_t lead = n_cont == 1 ? 0xD0 : n_cont == 2 ? 0xE2 : 0xF0;
    uint8_t cont[3] = { n_cont == 1 ? 0xA5 : n_cont == 2 ? 0x82 : 0x9F, 0x98, 0x80 };
    ls_ring_write(r, &lead, 1);
    ls_ring_write(r, cont, n_cont);
    ls_ring_write(r, (const uint8_t *)"OK\n", 3);
    /* now drop everything up to and including the lead byte */
    uint64_t lead_pos = r->head - 3 - n_cont - 1;
    while (ls_ring_tail(r) <= lead_pos) {
        size_t n = (size_t)(lead_pos + 1 - ls_ring_tail(r));
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        ls_ring_write(r, chunk, n);
    }
    CHECK(ls_ring_tail(r) == lead_pos + 1);
}

static void test_history_start_skips_leftovers(void)
{
    for (size_t n_cont = 1; n_cont <= 3; n_cont++) {
        ls_ring_t r;
        reader_t a;
        ring_new(&r);
        fill_with_cut_char(&r, n_cont);
        reader_start(&a, &r);
        reader_drain(&a, &r);
        /* no '?' at all: the leftovers are skipped, "OK\n" and the filler
         * written after it follow */
        CHECK(a.got.len >= 3 && memcmp(a.got.data, "OK\n", 3) == 0);
        CHECK(memchr(a.got.data, '?', a.got.len) == NULL);
        CHECK(a.got.len == LS_HISTORY_SIZE - n_cont);
        buf_free(&a.got);
    }
}

static void test_history_from_boot_shows_bad_bytes(void)
{
    /* nothing dropped yet: continuation bytes at the start are just bad */
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    log_bytes(&r, "\x98\x80OK\n", 5);
    reader_start(&a, &r);
    reader_drain(&a, &r);
    CHECK(got_is(&a, "??OK\n", 5));
    buf_free(&a.got);
}

static void test_gap_skips_leftovers(void)
{
    for (size_t n_cont = 1; n_cont <= 3; n_cont++) {
        ls_ring_t r;
        reader_t a;
        ring_new(&r);
        reader_start(&a, &r); /* stays at 0: falls behind */
        fill_with_cut_char(&r, n_cont);
        reader_drain(&a, &r);
        CHECK(a.gaps == 1);
        CHECK(a.got.len >= 4 && memcmp(a.got.data, "?OK\n", 4) == 0);
        CHECK(memchr(a.got.data + 1, '?', a.got.len - 1) == NULL);
        buf_free(&a.got);
    }
}

static void test_gap_skips_at_most_three(void)
{
    /* after a gap: 4 continuation bytes, only 3 can be leftovers */
    ls_ring_t r;
    reader_t a;
    ring_new(&r);
    reader_start(&a, &r);
    static uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 140; i++) {
        ls_ring_write(&r, chunk, sizeof(chunk));
    }
    uint64_t at = r.head;
    ls_ring_write(&r, (const uint8_t *)"\x80\x80\x80\x80Z", 5);
    while (ls_ring_tail(&r) < at) {
        size_t n = (size_t)(at - ls_ring_tail(&r));
        ls_ring_write(&r, chunk, n > sizeof(chunk) ? sizeof(chunk) : n);
    }
    reader_drain(&a, &r);
    CHECK(a.gaps == 1);
    CHECK(a.got.len >= 3 && memcmp(a.got.data, "??Z", 3) == 0);
    buf_free(&a.got);
}

static void test_leftovers_split_over_reads(void)
{
    /* the available text ends inside the leftovers: skipping goes on with
     * the next read, and stops at the first other byte */
    uint8_t mem[16], raw[8], out[8];
    ls_ring_t r;
    CHECK(ls_ring_init(&r, mem, 16) == 0);
    ls_ring_write(&r, (const uint8_t *)"\x9F", 1);
    ls_reader_t rd = { 0, 3 }; /* as after a gap */
    CHECK(ls_read_frame(&r, &NO_LOCK, &rd, raw, out, sizeof(out)) == 0);
    CHECK(rd.pos == 1 && rd.skip == 2);
    ls_ring_write(&r, (const uint8_t *)"\x98\x80" "A", 3);
    size_t n = ls_read_frame(&r, &NO_LOCK, &rd, raw, out, sizeof(out));
    CHECK(n == 1 && out[0] == 'A' && rd.skip == 0);
    ls_ring_write(&r, (const uint8_t *)"\x80", 1); /* a later stray byte */
    n = ls_read_frame(&r, &NO_LOCK, &rd, raw, out, sizeof(out));
    CHECK(n == 1 && out[0] == '?');

    /* text right after the gap ends the skipping at once */
    CHECK(ls_ring_init(&r, mem, 16) == 0);
    ls_ring_write(&r, (const uint8_t *)"A", 1);
    rd = (ls_reader_t){ 0, 3 };
    n = ls_read_frame(&r, &NO_LOCK, &rd, raw, out, sizeof(out));
    CHECK(n == 1 && out[0] == 'A' && rd.skip == 0);
    ls_ring_write(&r, (const uint8_t *)"\x80" "B", 2);
    n = ls_read_frame(&r, &NO_LOCK, &rd, raw, out, sizeof(out));
    CHECK(n == 2 && out[0] == '?' && out[1] == 'B');
}

/* ---- writers overwriting while a reader copies -------------------------- */

static ls_ring_t *s_race_ring;
static int s_race_locks;
static int s_race_at;

static void race_lock(void *arg)
{
    (void)arg;
    /* the second lock of ls_read_frame is after its copy: write a lot in
     * between, as another core would */
    if (++s_race_locks == s_race_at) {
        uint8_t chunk[1024];
        memset(chunk, 'z', sizeof(chunk));
        for (size_t i = 0; i < LS_HISTORY_SIZE / sizeof(chunk) + 4; i++) {
            ls_ring_write(s_race_ring, chunk, sizeof(chunk));
        }
    }
}

static void test_overwrite_while_copying(void)
{
    ls_ring_t r;
    ring_new(&r);
    uint8_t chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 200; i++) {
        ls_ring_write(&r, chunk, sizeof(chunk));
    }
    ls_reader_t rd = { ls_ring_tail(&r), 0 };
    s_race_ring = &r;
    s_race_locks = 0;
    s_race_at = 2;
    ls_lock_t lk = { race_lock, no_lock, NULL };
    static uint8_t raw[LS_FRAME_MAX], out[LS_FRAME_MAX];
    size_t n = ls_read_frame(&r, &lk, &rd, raw, out, LS_FRAME_MAX);
    /* the copied 'a's were overwritten: the reader gets the gap and 'z's */
    CHECK(n == LS_FRAME_MAX);
    CHECK(out[0] == '?');
    bool all_z = true;
    for (size_t i = 1; i < n; i++) {
        all_z = all_z && out[i] == 'z';
    }
    CHECK(all_z);
    CHECK(rd.pos == ls_ring_tail(&r) + LS_FRAME_MAX - 1);
}

#define RUN(call)                                  \
    do {                                           \
        if (s_verbose) {                           \
            fprintf(stderr, "%s\n", #call);       \
        }                                          \
        call;                                      \
    } while (0)

int main(int argc, char **argv)
{
    s_verbose = getenv("VERBOSE") != NULL;
    s_hist = malloc(LS_HISTORY_SIZE);
    RUN(test_example1());
    RUN(test_example2());
    RUN(test_example3());
    RUN(test_example4_5());
    RUN(test_example6());
    RUN(test_example7_cut());
    RUN(test_example8());
    RUN(test_ring_small_wrap());
    RUN(test_wrap_keep_up());
    RUN(test_boot_log(argc > 1 ? argv[1] : "examples/boot.log"));
    RUN(test_control_bytes_pass());
    RUN(test_utf8_cases());
    RUN(test_valid_text_random_cuts());
    RUN(test_random_bytes());
    RUN(test_history_start_skips_leftovers());
    RUN(test_history_from_boot_shows_bad_bytes());
    RUN(test_gap_skips_leftovers());
    RUN(test_gap_skips_at_most_three());
    RUN(test_leftovers_split_over_reads());
    RUN(test_overwrite_while_copying());
    free(s_hist);
    printf("%d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
