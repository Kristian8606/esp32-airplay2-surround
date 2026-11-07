/* SPDX-License-Identifier: Apache-2.0 */
/* Framing, message reader and the tables. */
#include <math.h>
#include <sodium.h>
#include <stdlib.h>

#include "rtsp_msg.h"
#include "rtsp_tables.h"
#include "test.h"

static void test_key(uint8_t key[32]) {
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
}

/* ---- Frames ---- */

static void test_frame_layout(void) {
  uint8_t key[32], frame[200], expect_ct[100], expect_tag[16], nonce[12] = {0};
  uint8_t msg[100];
  rtsp_cipher_t c;
  test_key(key);
  for (int i = 0; i < 100; i++) msg[i] = (uint8_t)i;
  rtsp_cipher_init(&c, key);
  c.counter = 0x0102030405060708ULL;
  CHECK_INT(rtsp_frame_seal(&c, msg, 100, frame), 118);
  CHECK_INT(c.counter, 0x0102030405060709ULL);
  /* length little-endian, then the sealed bytes; nonce = 4 zero bytes and
   * the counter little-endian; the length is the associated data */
  CHECK_INT(frame[0], 100);
  CHECK_INT(frame[1], 0);
  for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(0x08 - i);
  crypto_aead_chacha20poly1305_ietf_encrypt_detached(expect_ct, expect_tag, NULL, msg, 100,
                                                     frame, 2, NULL, nonce, key);
  CHECK(memcmp(frame + 2, expect_ct, 100) == 0);
  CHECK(memcmp(frame + 102, expect_tag, 16) == 0);
}

/* spec "Encryption": one frame up to 1024 bytes, else frames of 1024 with
 * the rest in the last, cut across head and body. */
static void test_frame_gather(void) {
  static uint8_t head[1100], body[3000], out[RTSP_FRAME_MAX], all[4100];
  size_t sizes[][2] = {{10, 0}, {10, 20}, {1000, 24}, {1000, 25}, {100, 2400},
                       {1100, 0}, {1100, 3000}, {0, 1024}};
  for (size_t i = 0; i < sizeof(head); i++) head[i] = (uint8_t)i;
  for (size_t i = 0; i < sizeof(body); i++) body[i] = (uint8_t)(i * 3 + 1);
  for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
    size_t hl = sizes[k][0], bl = sizes[k][1], total = hl + bl, off = 0, n;
    int frames = 0;
    memcpy(all, head, hl);
    memcpy(all + hl, body, bl);
    while ((n = rtsp_frame_gather(head, hl, body, bl, off, out)) > 0) {
      /* every frame but the last is full */
      CHECK_INT(n, total - off < RTSP_FRAME_MAX ? total - off : RTSP_FRAME_MAX);
      CHECK(memcmp(out, all + off, n) == 0);
      off += n;
      frames++;
    }
    CHECK_INT(off, total);
    CHECK_INT(frames, (int)((total + RTSP_FRAME_MAX - 1) / RTSP_FRAME_MAX));
  }
  CHECK_INT(rtsp_frame_gather(head, 10, body, 20, 30, out), 0);
  CHECK_INT(rtsp_frame_gather(head, 10, body, 20, 31, out), 0);
}

static void test_frame_seal_in_place(void) {
  uint8_t key[32], a[RTSP_FRAME_MAX + RTSP_FRAME_OVERHEAD], b[sizeof(a)], msg[300];
  rtsp_cipher_t c1, c2;
  test_key(key);
  for (int i = 0; i < 300; i++) msg[i] = (uint8_t)(i ^ 0x5a);
  rtsp_cipher_init(&c1, key);
  rtsp_cipher_init(&c2, key);
  CHECK_INT(rtsp_frame_seal(&c1, msg, 300, a), 318);
  memcpy(b + 2, msg, 300);
  CHECK_INT(rtsp_frame_seal(&c2, b + 2, 300, b), 318);
  CHECK(memcmp(a, b, 318) == 0);
}

static void test_frame_round_trip_split(void) {
  uint8_t key[32], buf[3 * (1024 + 18)], msg[1024];
  rtsp_cipher_t c;
  rtsp_frame_reader_t r;
  size_t n = 0, got = 0;
  test_key(key);
  for (int i = 0; i < 1024; i++) msg[i] = (uint8_t)(i * 7);
  rtsp_cipher_init(&c, key);
  n += rtsp_frame_seal(&c, msg, 1024, buf + n); /* the largest frame */
  n += rtsp_frame_seal(&c, msg, 1, buf + n);
  n += rtsp_frame_seal(&c, msg, 500, buf + n);
  CHECK_INT(rtsp_frame_seal(&c, msg, 0, buf), 0);
  rtsp_frame_reader_init(&r, key);
  /* byte by byte */
  for (size_t i = 0; i < n; i++) {
    size_t used;
    const uint8_t *p;
    size_t pl;
    rtsp_frame_result_t fr = rtsp_frame_feed(&r, buf + i, 1, &used, &p, &pl);
    CHECK_INT(used, 1);
    if (fr == RTSP_FRAME_DONE) {
      CHECK(memcmp(p, msg, pl) == 0);
      got += pl;
    }
    CHECK(fr != RTSP_FRAME_ERROR);
  }
  CHECK_INT(got, 1525);
  CHECK_INT(r.cipher.counter, 3);
}

static void test_frame_corrupt(void) {
  uint8_t key[32], buf[64], msg[10] = "abcdefghi";
  rtsp_cipher_t c;
  rtsp_frame_reader_t r;
  size_t used, pl, n;
  const uint8_t *p;
  test_key(key);

  /* a length over 1024 */
  uint8_t big[2] = {0x01, 0x04}; /* 1025 */
  rtsp_frame_reader_init(&r, key);
  CHECK(rtsp_frame_feed(&r, big, 2, &used, &p, &pl) == RTSP_FRAME_ERROR);
  uint8_t huge[2] = {0xff, 0xff};
  rtsp_frame_reader_init(&r, key);
  CHECK(rtsp_frame_feed(&r, huge, 2, &used, &p, &pl) == RTSP_FRAME_ERROR);

  /* a flipped bit in the ciphertext, the tag, the length */
  for (int at = 0; at < 3; at++) {
    rtsp_cipher_init(&c, key);
    n = rtsp_frame_seal(&c, msg, 10, buf);
    if (at == 0) buf[5] ^= 1;
    if (at == 1) buf[n - 1] ^= 1;
    if (at == 2) buf[1] ^= 0; /* unchanged length but wrong counter below */
    rtsp_frame_reader_init(&r, key);
    if (at == 2) r.cipher.counter = 1; /* replayed or skipped frame */
    CHECK(rtsp_frame_feed(&r, buf, n, &used, &p, &pl) == RTSP_FRAME_ERROR);
  }
  /* a shorter length than sealed: the tag lands elsewhere */
  rtsp_cipher_init(&c, key);
  n = rtsp_frame_seal(&c, msg, 10, buf);
  buf[0] = 9;
  rtsp_frame_reader_init(&r, key);
  CHECK(rtsp_frame_feed(&r, buf, n, &used, &p, &pl) == RTSP_FRAME_ERROR);
  /* wrong key */
  rtsp_cipher_init(&c, key);
  n = rtsp_frame_seal(&c, msg, 10, buf);
  key[3] ^= 1;
  rtsp_frame_reader_init(&r, key);
  CHECK(rtsp_frame_feed(&r, buf, n, &used, &p, &pl) == RTSP_FRAME_ERROR);
}

/* ---- Message reader ---- */

static rtsp_read_result_t feed_all(rtsp_reader_t *r, const char *text, size_t *consumed) {
  return rtsp_reader_feed(r, (const uint8_t *)text, strlen(text), consumed);
}

static void test_reader_request(void) {
  static rtsp_reader_t r;
  size_t used;
  const char *text =
      "\r\n\r\nSETUP rtsp://10.0.0.1/123 RTSP/1.0\r\ncseq: 7\r\nCONTENT-length:  5 \r\n"
      "X-Apple: a: b\r\n\r\nhelloGET /info RTSP/1.0\r\nCSeq: 8\r\n\r\n";
  rtsp_reader_init(&r, 100);
  CHECK(feed_all(&r, text, &used) == RTSP_READ_MESSAGE);
  CHECK_STR(r.msg.start[0], "SETUP");
  CHECK_STR(r.msg.start[1], "rtsp://10.0.0.1/123");
  CHECK_STR(r.msg.start[2], "RTSP/1.0");
  CHECK_STR(rtsp_msg_header(&r.msg, "CSeq"), "7");
  CHECK_STR(rtsp_msg_header(&r.msg, "x-apple"), "a: b");
  CHECK_INT(r.msg.body_len, 5);
  CHECK(r.msg.body_state == RTSP_BODY_OK && memcmp(r.msg.body, "hello", 5) == 0);
  /* the second, pipelined request */
  {
    size_t used2;
    CHECK(rtsp_reader_feed(&r, (const uint8_t *)text + used, strlen(text) - used, &used2) ==
          RTSP_READ_MESSAGE);
    CHECK_STR(r.msg.start[0], "GET");
    CHECK_STR(rtsp_msg_header(&r.msg, "CSEQ"), "8");
    CHECK_INT(r.msg.body_len, 0);
    CHECK(r.msg.body == NULL);
    CHECK_INT(used + used2, strlen(text));
  }
  rtsp_reader_next(&r);
}

static void test_reader_split_and_lf(void) {
  static rtsp_reader_t r;
  const char *text = "POST /command RTSP/1.0\nCSeq: 3\nContent-Length: 4\n\nabcd";
  rtsp_read_result_t res = RTSP_READ_MORE;
  rtsp_reader_init(&r, 100);
  for (size_t i = 0; i < strlen(text); i++) {
    size_t used;
    res = rtsp_reader_feed(&r, (const uint8_t *)text + i, 1, &used);
    CHECK_INT(used, 1);
    if (i + 1 < strlen(text)) CHECK(res == RTSP_READ_MORE);
  }
  CHECK(res == RTSP_READ_MESSAGE);
  CHECK_STR(r.msg.start[1], "/command");
  CHECK(memcmp(r.msg.body, "abcd", 4) == 0);
  rtsp_reader_next(&r);
}

static void test_reader_bodies(void) {
  static rtsp_reader_t r;
  size_t used;
  char head[128];
  uint8_t *big = malloc(70000);
  memset(big, 'x', 70000);
  /* 7639 bytes (seen with /command) and 16 KiB are accepted */
  size_t sizes[] = {7639, 16384};
  for (int k = 0; k < 2; k++) {
    rtsp_reader_init(&r, RTSP_BODY_MAX);
    snprintf(head, sizeof(head), "POST /command RTSP/1.0\r\nContent-Length: %zu\r\n\r\n", sizes[k]);
    CHECK(feed_all(&r, head, &used) == RTSP_READ_MORE);
    CHECK(rtsp_reader_feed(&r, big, sizes[k], &used) == RTSP_READ_MESSAGE);
    CHECK_INT(used, sizes[k]);
    CHECK(r.msg.body_state == RTSP_BODY_OK);
    rtsp_reader_next(&r);
  }
  /* over the limit: read and dropped, the next message still parses */
  rtsp_reader_init(&r, 1000);
  CHECK(feed_all(&r, "SET_PARAMETER * RTSP/1.0\r\nContent-Length: 1500\r\n\r\n", &used) ==
        RTSP_READ_MORE);
  CHECK(rtsp_reader_feed(&r, big, 1000, &used) == RTSP_READ_MORE);
  CHECK(rtsp_reader_feed(&r, big, 600, &used) == RTSP_READ_MESSAGE);
  CHECK_INT(used, 500);
  CHECK(r.msg.body_state == RTSP_BODY_TOO_LARGE);
  CHECK(r.msg.body == NULL);
  CHECK(feed_all(&r, "OPTIONS * RTSP/1.0\r\n\r\n", &used) == RTSP_READ_MESSAGE);
  CHECK_STR(r.msg.start[0], "OPTIONS");
  rtsp_reader_next(&r);
  /* no memory for the body */
  rtsp_reader_init(&r, 1000);
  fake.malloc_fail = true;
  CHECK(feed_all(&r, "POST /x RTSP/1.0\r\nContent-Length: 3\r\n\r\nabc", &used) ==
        RTSP_READ_MESSAGE);
  CHECK(r.msg.body_state == RTSP_BODY_NO_MEMORY);
  fake.malloc_fail = false;
  rtsp_reader_next(&r);
  free(big);
}

static void test_reader_corrupt(void) {
  static rtsp_reader_t r;
  static const char *bad[] = {
      "GET\r\n\r\n",                                     /* one token */
      " GET /info RTSP/1.0\r\n\r\n",                     /* empty method */
      "GET  RTSP/1.0\r\n\r\n",                           /* empty URI */
      "GET /info RTSP/1.0\r\nCSeq 1\r\n\r\n",            /* no colon */
      "GET /info RTSP/1.0\r\n: 1\r\n\r\n",               /* empty name */
      "GET /info RTSP/1.0\r\nC Seq: 1\r\n\r\n",          /* space in name */
      "GET /info RTSP/1.0\r\nA: 1\r\n continued\r\n\r\n", /* folding */
      "GET /info RTSP/1.0\r\nContent-Length: 12a\r\n\r\n",
      "GET /info RTSP/1.0\r\nContent-Length: -1\r\n\r\n",
      "GET /info RTSP/1.0\r\nContent-Length: \r\n\r\n",
      "GET /info RTSP/1.0\r\nContent-Length: 99999999999\r\n\r\n",
      "GET /info RTSP/1.0\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
      "GET /info RTSP/1.0\r\nA: \x01\r\n\r\n",          /* control byte */
      "GET /info\x7f RTSP/1.0\r\n\r\n",
  };
  size_t used;
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    rtsp_reader_init(&r, 100);
    if (feed_all(&r, bad[i], &used) != RTSP_READ_BAD) {
      t_failures++;
      fprintf(stderr, "reader accepted bad input %zu\n", i);
    }
    t_checks++;
  }
  /* a NUL inside the head */
  {
    const uint8_t nul[] = "GET /info RTSP/1.0\r\nA: x\0y\r\n\r\n";
    rtsp_reader_init(&r, 100);
    CHECK(rtsp_reader_feed(&r, nul, sizeof(nul) - 1, &used) == RTSP_READ_BAD);
  }
  /* too many headers */
  {
    char text[2048] = "GET /info RTSP/1.0\r\n";
    for (int i = 0; i < RTSP_MAX_HEADERS + 1; i++) {
      snprintf(text + strlen(text), sizeof(text) - strlen(text), "H%d: v\r\n", i);
    }
    strcat(text, "\r\n");
    rtsp_reader_init(&r, 100);
    CHECK(feed_all(&r, text, &used) == RTSP_READ_BAD);
  }
  /* a head that never ends */
  {
    char *text = malloc(RTSP_HEAD_MAX + 100);
    memset(text, 'a', RTSP_HEAD_MAX + 99);
    text[RTSP_HEAD_MAX + 99] = 0;
    memcpy(text, "GET /info RTSP/1.0\r\nX: ", 23);
    rtsp_reader_init(&r, 100);
    CHECK(feed_all(&r, text, &used) == RTSP_READ_BAD);
    free(text);
  }
}

static void test_reply_head(void) {
  char out[512];
  size_t n = rtsp_reply_head(out, sizeof(out), 200, "3", NULL, NULL, 0);
  CHECK_INT(n, strlen(out));
  CHECK_STR(out, "RTSP/1.0 200 OK\r\nCSeq: 3\r\nServer: AirTunes/377.40.00\r\n\r\n");
  rtsp_reply_head(out, sizeof(out), 501, "9", "Connection: close\r\n", "text/plain", 15);
  CHECK_STR(out,
            "RTSP/1.0 501 Not Implemented\r\nCSeq: 9\r\nServer: AirTunes/377.40.00\r\n"
            "Connection: close\r\nContent-Type: text/plain\r\nContent-Length: 15\r\n\r\n");
  rtsp_reply_head(out, sizeof(out), 400, NULL, NULL, NULL, 0);
  CHECK_STR(out, "RTSP/1.0 400 Bad Request\r\nServer: AirTunes/377.40.00\r\n\r\n");
  CHECK_INT(rtsp_reply_head(out, 20, 200, "3", NULL, NULL, 0), 0);
}

/* ---- Tables ---- */

static void test_volume_q15(void) {
  /* spec "SET_PARAMETER": [log] and [probe] values */
  CHECK_INT(rtsp_volume_to_q15(-15.0), 8192);
  CHECK_INT(rtsp_volume_to_q15(-13.125), 10368);
  CHECK_INT(rtsp_volume_to_q15(-18.75), 4608);
  CHECK_INT(rtsp_volume_to_q15(-24.3385), 1166);
  CHECK_INT(rtsp_volume_to_q15(-20.625), 3200);
  CHECK_INT(rtsp_volume_to_q15(-20.0), 3640);
  CHECK_INT(rtsp_volume_to_q15(-144.0), 0);
  CHECK_INT(rtsp_volume_to_q15(-30.0), 0);
  CHECK_INT(rtsp_volume_to_q15(-30.5), 0);
  CHECK_INT(rtsp_volume_to_q15(-200.0), 0);
  CHECK_INT(rtsp_volume_to_q15(0.0), 32768);
  CHECK_INT(rtsp_volume_to_q15(5.0), 32768);
  CHECK_INT(rtsp_volume_to_q15(NAN), 0);
}

static void test_volume_steps(void) {
  /* spec "Event channel": from -15.0 (0.5) up gives 0.5625; after the
   * acknowledged up (-13.125) down gives 0.5 */
  CHECK(rtsp_volume_to_fraction(-15.0) == 0.5);
  CHECK(rtsp_volume_step(-15.0, true) == 0.5625);
  CHECK(rtsp_fraction_to_volume(0.5625) == -13.125);
  CHECK(rtsp_volume_step(-13.125, false) == 0.5);
  CHECK(rtsp_volume_step(0.0, true) == 1.0);
  CHECK(rtsp_volume_step(-144.0, false) == 0.0);
  CHECK(rtsp_volume_step(-144.0, true) == 0.0625);
  CHECK(rtsp_volume_step(-30.0, false) == 0.0);
}

static void test_volume_text(void) {
  char out[24];
  double v;
  rtsp_format_volume(-20.625, out, sizeof(out));
  CHECK_STR(out, "-20.62"); /* spec "GET_PARAMETER" [probe] */
  rtsp_format_volume(-15.0, out, sizeof(out));
  CHECK_STR(out, "-15.00");
  rtsp_format_volume(-144.0, out, sizeof(out));
  CHECK_STR(out, "-144.00");
  rtsp_format_volume(0.0, out, sizeof(out));
  CHECK_STR(out, "0.00");
  rtsp_format_volume(-13.125, out, sizeof(out));
  CHECK_STR(out, "-13.12");
  rtsp_format_volume(-24.3385, out, sizeof(out));
  CHECK_STR(out, "-24.34");
  /* what printf("%.2f") gives for the value as stored */
  rtsp_format_volume(-0.001, out, sizeof(out));
  CHECK_STR(out, "-0.00");
  rtsp_format_volume(-20.615, out, sizeof(out)); /* stored just above -20.615 */
  CHECK_STR(out, "-20.61");
  rtsp_format_volume(-7.875, out, sizeof(out)); /* an exact tie */
  CHECK_STR(out, "-7.88");

  /* spec "SET_PARAMETER": above 0 as 0, below -144 as -144, the rest as it
   * came */
  CHECK(rtsp_volume_clamp(3.5) == 0.0);
  CHECK(rtsp_volume_clamp(-200.0) == -144.0);
  CHECK(rtsp_volume_clamp(-144.0) == -144.0);
  CHECK(rtsp_volume_clamp(-50.0) == -50.0);
  CHECK(rtsp_volume_clamp(-20.625) == -20.625);
  CHECK(rtsp_volume_clamp(0.0) == 0.0);

  CHECK(rtsp_parse_volume("-15.000000", &v) && v == -15.0);
  CHECK(rtsp_parse_volume(" -24.3385 ", &v) && v == -24.3385);
  CHECK(rtsp_parse_volume("-144", &v) && v == -144.0);
  CHECK(!rtsp_parse_volume("", &v));
  CHECK(!rtsp_parse_volume("   ", &v));
  CHECK(!rtsp_parse_volume("abc", &v));
  CHECK(!rtsp_parse_volume("-15x", &v));
  CHECK(!rtsp_parse_volume("-15 1", &v));
  CHECK(!rtsp_parse_volume("nan", &v));
  CHECK(!rtsp_parse_volume("inf", &v));
  CHECK(!rtsp_parse_volume("1e999", &v));
}

static void test_formats(void) {
  sound_format_t f;
  /* every row of the table */
  CHECK(rtsp_format_lookup(96, 0x40000, &f));
  CHECK_STR(f.codec, "ALAC");
  CHECK_INT(f.sample_rate, 44100);
  CHECK_INT(f.channels, 2);
  CHECK_INT(f.bits_per_sample, 16);
  CHECK_INT(f.frame_size, 352);
  CHECK(rtsp_format_lookup(103, 0x40000, &f) && f.sample_rate == 44100);
  CHECK(rtsp_format_lookup(103, 0x200000, &f));
  CHECK(strcmp(f.codec, "ALAC") == 0 && f.sample_rate == 48000 && f.bits_per_sample == 24 &&
        f.frame_size == 352 && f.channels == 2);
  CHECK(rtsp_format_lookup(103, 0x400000, &f));
  CHECK(strcmp(f.codec, "AAC") == 0 && f.sample_rate == 44100 && f.bits_per_sample == 24 &&
        f.frame_size == 1024);
  CHECK(rtsp_format_lookup(103, 0x800000, &f));
  CHECK(strcmp(f.codec, "AAC") == 0 && f.sample_rate == 48000 && f.channels == 2);
  CHECK(rtsp_format_lookup(103, 0x8000000000ULL, &f) && f.channels == 2 && f.sample_rate == 48000);
  CHECK(rtsp_format_lookup(103, 0x10000000000ULL, &f) && f.channels == 2 &&
        f.frame_size == 1024);
  /* not in the table */
  CHECK(!rtsp_format_lookup(96, 0x200000, &f));
  CHECK(!rtsp_format_lookup(96, 0x10000000000ULL, &f));
  CHECK(!rtsp_format_lookup(103, 0x80000, &f));
  CHECK(!rtsp_format_lookup(103, 0, &f));
  CHECK(!rtsp_format_lookup(103, 0x40000 | 0x200000, &f));
  CHECK(!rtsp_format_lookup(100, 0x40000, &f));
  CHECK(!rtsp_format_lookup(-1, 0x40000, &f));
}

static void test_network_time(void) {
  CHECK_INT(rtsp_network_time_ns(0, 0), 0);
  CHECK_INT(rtsp_network_time_ns(1, 0), 1000000000ULL);
  CHECK_INT(rtsp_network_time_ns(10, 1ULL << 63), 10500000000ULL);
  CHECK_INT(rtsp_network_time_ns(0, 1ULL << 62), 250000000ULL);
  CHECK_INT(rtsp_network_time_ns(0, UINT64_MAX), 999999999ULL);
  CHECK_INT(rtsp_network_time_ns(0, 1ULL << 32), 0); /* 2^32 / 2^64 s < 1 ns */
  CHECK_INT(rtsp_network_time_ns(0, 18446744074ULL), 1); /* just over 1 ns */
}

static void test_rtp_info(void) {
  uint32_t t = 0;
  CHECK(rtsp_parse_rtp_info("rtptime=12345", &t) && t == 12345);
  CHECK(rtsp_parse_rtp_info("seq=1;rtptime=99", &t) && t == 99);
  CHECK(rtsp_parse_rtp_info("seq=1; rtptime=4294967295", &t) && t == 4294967295U);
  CHECK(rtsp_parse_rtp_info("rtptime=7;seq=3", &t) && t == 7);
  CHECK(!rtsp_parse_rtp_info("rtptime=", &t));
  CHECK(!rtsp_parse_rtp_info("rtptime=4294967296", &t));
  CHECK(!rtsp_parse_rtp_info("rtptime=12x", &t));
  CHECK(!rtsp_parse_rtp_info("rtptime=-1", &t));
  CHECK(!rtsp_parse_rtp_info("seq=5", &t));
  CHECK(!rtsp_parse_rtp_info("xrtptime=5", &t));
  CHECK(!rtsp_parse_rtp_info("", &t));
}

static void test_text_helpers(void) {
  char name[16], value[16];
  const char *body = "volume: -15.0\r\nprogress: 1/2/3\nlast";
  const char *cur = body, *line;
  size_t len;
  CHECK(rtsp_next_line(&cur, body + strlen(body), &line, &len) && len == 13);
  CHECK(rtsp_split_param(line, len, name, sizeof(name), value, sizeof(value)));
  CHECK_STR(name, "volume");
  CHECK_STR(value, "-15.0");
  CHECK(rtsp_next_line(&cur, body + strlen(body), &line, &len) && len == 15);
  CHECK(rtsp_next_line(&cur, body + strlen(body), &line, &len) && len == 4);
  CHECK(!rtsp_next_line(&cur, body + strlen(body), &line, &len));
  CHECK(!rtsp_split_param("novalue", 7, name, sizeof(name), value, sizeof(value)));
  CHECK(!rtsp_split_param("a: 0123456789abcdefgh", 21, name, sizeof(name), value,
                          sizeof(value)));

  CHECK(rtsp_content_type_is("text/parameters", "text/parameters"));
  CHECK(rtsp_content_type_is("Text/Parameters; charset=utf-8", "text/parameters"));
  CHECK(!rtsp_content_type_is("text/parametersx", "text/parameters"));
  CHECK(!rtsp_content_type_is(NULL, "text/parameters"));
  CHECK(rtsp_content_type_is_image("image/jpeg"));
  CHECK(rtsp_content_type_is_image("IMAGE/png"));
  CHECK(!rtsp_content_type_is_image("image/"));
  CHECK(!rtsp_content_type_is_image("application/x-dmap-tagged"));
}

void run_unit_tests(void) {
  RUN(test_frame_layout);
  RUN(test_frame_gather);
  RUN(test_frame_seal_in_place);
  RUN(test_frame_round_trip_split);
  RUN(test_frame_corrupt);
  RUN(test_reader_request);
  RUN(test_reader_split_and_lf);
  RUN(test_reader_bodies);
  RUN(test_reader_corrupt);
  RUN(test_reply_head);
  RUN(test_volume_q15);
  RUN(test_volume_steps);
  RUN(test_volume_text);
  RUN(test_formats);
  RUN(test_network_time);
  RUN(test_rtp_info);
  RUN(test_text_helpers);
}
