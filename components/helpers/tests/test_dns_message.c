/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* DNS message rules: the spec examples byte for byte, and damaged input.
 * Every input is copied so that it ends right before an inaccessible page,
 * so a read past the datagram crashes the test. */

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dns_message.h"
#include "test.h"

static const char *Q_A =
    "1a2b010000010000000000010763617074697665056170706c6503636f6d0000010001"
    "00002904d0000000000000";
static const char *R_A =
    "1a2b850000010001000000000763617074697665056170706c6503636f6d0000010001"
    "c00c000100010000003c0004c0a80401";
static const char *Q_AAAA =
    "1a2c010000010000000000000763617074697665056170706c6503636f6d00001c0001";
static const char *R_AAAA =
    "1a2c850000010000000000000763617074697665056170706c6503636f6d00001c0001";

static uint32_t redirect_ip;

/* Two pages: the first holds the input at its very end, the second has no
 * access at all. */
static uint8_t *guard_area;
static size_t page_size;

static void guard_init(void) {
  page_size = (size_t)sysconf(_SC_PAGESIZE);
  guard_area = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
  if (guard_area == MAP_FAILED ||
      mprotect(guard_area + page_size, page_size, PROT_NONE) != 0) {
    perror("guard page");
    exit(2);
  }
}

static size_t from_hex(const char *hex, uint8_t *out) {
  size_t n = strlen(hex) / 2;
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    sscanf(hex + 2 * i, "%2x", &v);
    out[i] = (uint8_t)v;
  }
  return n;
}

/* Runs the rules on an exact-size copy of buf[0, len). */
static size_t reply_to(const uint8_t *buf, size_t len, uint8_t *reply,
                       size_t reply_size) {
  if (len > page_size) {
    len = page_size; /* not used: inputs are at most 600 bytes */
  }
  uint8_t *copy = guard_area + page_size - len;
  memcpy(copy, buf, len);
  return dns_message_reply(copy, len, redirect_ip, reply, reply_size);
}

/* The guard page really stops a read one byte past the input. */
static void test_guard_works(void) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    volatile uint8_t *p = guard_area + page_size;
    _exit(*p == 0 ? 0 : 1);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status));
}

static size_t reply_full(const uint8_t *buf, size_t len, uint8_t *reply) {
  return reply_to(buf, len, reply, DNS_MESSAGE_MAX);
}

static void test_examples(void) {
  uint8_t q[64], want[64], got[DNS_MESSAGE_MAX];

  size_t qn = from_hex(Q_A, q);
  size_t wn = from_hex(R_A, want);
  CHECK(qn == 46 && wn == 51);
  CHECK(reply_full(q, qn, got) == wn);
  CHECK(memcmp(got, want, wn) == 0);

  qn = from_hex(Q_AAAA, q);
  wn = from_hex(R_AAAA, want);
  CHECK(qn == 35 && wn == 35);
  CHECK(reply_full(q, qn, got) == wn);
  CHECK(memcmp(got, want, wn) == 0);
}

static void test_truncations(void) {
  uint8_t q[64], got[DNS_MESSAGE_MAX], want[64];

  /* The question of the A query ends at byte 35; after that only the OPT
   * record is cut, which is ignored. */
  size_t qn = from_hex(Q_A, q);
  size_t wn = from_hex(R_A, want);
  for (size_t len = 0; len < qn; len++) {
    size_t r = reply_full(q, len, got);
    if (len < 35) {
      CHECK(r == 0);
    } else {
      CHECK(r == wn && memcmp(got, want, wn) == 0);
    }
  }

  qn = from_hex(Q_AAAA, q);
  for (size_t len = 0; len < qn; len++) {
    CHECK(reply_full(q, len, got) == 0);
  }
}

static void test_header_rules(void) {
  uint8_t q[64], got[DNS_MESSAGE_MAX];
  size_t qn = from_hex(Q_A, q);

  uint8_t m[64];
  memcpy(m, q, qn);
  m[2] |= 0x80; /* QR = 1 */
  CHECK(reply_full(m, qn, got) == 0);

  for (int op = 1; op < 16; op++) {
    memcpy(m, q, qn);
    m[2] = (uint8_t)((m[2] & ~0x78) | (op << 3));
    CHECK(reply_full(m, qn, got) == 0);
  }

  memcpy(m, q, qn);
  m[5] = 0; /* QDCOUNT 0 */
  CHECK(reply_full(m, qn, got) == 0);
  m[5] = 2; /* QDCOUNT 2 */
  CHECK(reply_full(m, qn, got) == 0);
  m[4] = 1;
  m[5] = 1; /* QDCOUNT 257 */
  CHECK(reply_full(m, qn, got) == 0);

  /* RD = 0 is copied; other header bits of the query are not. */
  memcpy(m, q, qn);
  m[2] = 0x00 | 0x06; /* RD = 0, AA and TC set in the query */
  m[3] = 0xff;        /* RA, Z, RCODE set */
  CHECK(reply_full(m, qn, got) == 51);
  CHECK(got[2] == 0x84 && got[3] == 0x00);

  /* TC, AA, Z and RCODE of the query are ignored one by one too: each
   * still gets the example reply. */
  static const uint8_t ignored[][2] = {
      {0x02, 0x00}, /* TC */
      {0x04, 0x00}, /* AA */
      {0x00, 0x40}, /* Z (top bit of the 3) */
      {0x00, 0x10}, /* Z (lowest bit of the 3) */
      {0x00, 0x05}, /* RCODE 5 */
      {0x00, 0x0f}, /* RCODE 15 */
  };
  uint8_t want[64];
  size_t wn = from_hex(R_A, want);
  for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
    memcpy(m, q, qn);
    m[2] |= ignored[i][0];
    m[3] |= ignored[i][1];
    CHECK(reply_full(m, qn, got) == wn);
    CHECK(memcmp(got, want, wn) == 0);
  }

  /* ANCOUNT and NSCOUNT may be anything. */
  memcpy(m, q, qn);
  m[6] = 0x12, m[7] = 0x34, m[8] = 0xff, m[9] = 0xff;
  CHECK(reply_full(m, qn, got) == 51);
  CHECK(got[6] == 0 && got[7] == 1 && got[8] == 0 && got[9] == 0 &&
        got[10] == 0 && got[11] == 0);
}

static void test_types(void) {
  uint8_t q[64], got[DNS_MESSAGE_MAX];
  size_t qn = from_hex(Q_AAAA, q);

  /* ANY gets an answer like A. */
  q[31] = 0, q[32] = 255;
  CHECK(reply_full(q, qn, got) == 51);
  CHECK(got[7] == 1);
  CHECK(memcmp(got + 12, q + 12, 23) == 0);
  CHECK(memcmp(got + 47, "\xc0\xa8\x04\x01", 4) == 0);

  /* HTTPS (65): no answer. */
  q[31] = 0, q[32] = 65;
  CHECK(reply_full(q, qn, got) == 35);
  CHECK(got[7] == 0);

  /* A in class CH (3): no answer. */
  q[31] = 0, q[32] = 1, q[33] = 0, q[34] = 3;
  CHECK(reply_full(q, qn, got) == 35);
  CHECK(got[7] == 0 && got[3] == 0);
}

/* Writes a query with the given wire name (no checks) and type A. */
static size_t make_query(uint8_t *buf, const uint8_t *name, size_t name_len) {
  static const uint8_t hdr[12] = {0x12, 0x34, 0x01, 0, 0, 1, 0, 0, 0, 0, 0, 0};
  memcpy(buf, hdr, 12);
  memcpy(buf + 12, name, name_len);
  memcpy(buf + 12 + name_len, "\x00\x01\x00\x01", 4);
  return 12 + name_len + 4;
}

/* A wire name of labels of the given lengths, filled with 'a'. */
static size_t make_name(uint8_t *out, const size_t *labels, size_t count) {
  size_t p = 0;
  for (size_t i = 0; i < count; i++) {
    out[p++] = (uint8_t)labels[i];
    memset(out + p, 'a', labels[i]);
    p += labels[i];
  }
  out[p++] = 0;
  return p;
}

static void test_names(void) {
  uint8_t name[600], q[700], got[DNS_MESSAGE_MAX];

  /* A label of 63 bytes is fine, 64 is not. */
  size_t l63[] = {63, 3};
  size_t n = make_name(name, l63, 2);
  CHECK(reply_full(q, make_query(q, name, n), got) > 0);
  size_t l64[] = {64, 3};
  n = make_name(name, l64, 2);
  CHECK(reply_full(q, make_query(q, name, n), got) == 0);

  /* 255 bytes in wire form is fine, 256 and 257 are not. */
  size_t l255[] = {63, 63, 63, 61};
  n = make_name(name, l255, 4);
  CHECK(n == 255);
  size_t qn = make_query(q, name, n);
  size_t rn = reply_full(q, qn, got);
  CHECK(rn == qn + 16);
  CHECK(memcmp(got + 12, q + 12, n + 4) == 0);
  size_t l256[] = {63, 63, 63, 62};
  n = make_name(name, l256, 4);
  CHECK(n == 256);
  CHECK(reply_full(q, make_query(q, name, n), got) == 0);
  size_t l257[] = {63, 63, 63, 63};
  n = make_name(name, l257, 4);
  CHECK(reply_full(q, make_query(q, name, n), got) == 0);

  /* Many short labels past 255. */
  size_t many[130];
  for (size_t i = 0; i < 130; i++) {
    many[i] = 1;
  }
  n = make_name(name, many, 130);
  CHECK(n == 261);
  CHECK(reply_full(q, make_query(q, name, n), got) == 0);

  /* A compression pointer, first or after a label. */
  CHECK(reply_full(q, make_query(q, (const uint8_t *)"\xc0\x0c", 2), got) ==
        0);
  CHECK(reply_full(q, make_query(q, (const uint8_t *)"\x03" "abc\xc0\x0c", 6),
                   got) == 0);
  /* The root name (only the zero byte) is accepted like any other name. */
  qn = make_query(q, (const uint8_t *)"\x00", 1);
  CHECK(qn == 17);
  rn = reply_full(q, qn, got);
  CHECK(rn == 33);
  static const uint8_t root_reply[33] = {
      0x12, 0x34, 0x85, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x01, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 0xc0, 0xa8, 0x04, 0x01};
  CHECK(memcmp(got, root_reply, sizeof(root_reply)) == 0);
  /* Root name with QTYPE AAAA: the question only. */
  q[13] = 0, q[14] = 28;
  CHECK(reply_full(q, qn, got) == 17);
  CHECK(got[7] == 0);
  /* Root name cut before QCLASS ends. */
  CHECK(reply_full(q, 16, got) == 0);

  /* Reserved label forms 01 and 10. */
  CHECK(reply_full(q, make_query(q, (const uint8_t *)"\x40" "a\x00", 3),
                   got) == 0);
  CHECK(reply_full(q, make_query(q, (const uint8_t *)"\x80" "a\x00", 3),
                   got) == 0);

  /* A label that runs past the end of the datagram. */
  uint8_t cut[] = {0x12, 0x34, 0x01, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x3f, 'a'};
  CHECK(reply_full(cut, sizeof(cut), got) == 0);
}

static void test_sizes(void) {
  uint8_t q[600], got[DNS_MESSAGE_MAX];
  memset(q, 0, sizeof(q));
  size_t qn = from_hex(Q_A, q);
  (void)qn;

  /* Trailing bytes up to 512 are ignored; 513 is dropped. */
  CHECK(reply_full(q, 512, got) == 51);
  CHECK(reply_full(q, 513, got) == 0);
  CHECK(reply_full(q, 600, got) == 0);

  /* A reply buffer too small: no reply, nothing written past it. */
  uint8_t small[60];
  memset(small, 0xee, sizeof(small));
  CHECK(reply_to(q, 46, small, 50) == 0);
  CHECK(reply_to(q, 46, small, 51) == 51);
  CHECK(small[51] == 0xee);

  /* NULL pointers. */
  CHECK(dns_message_reply(NULL, 46, redirect_ip, got, sizeof(got)) == 0);
  CHECK(dns_message_reply(q, 46, redirect_ip, NULL, sizeof(got)) == 0);
}

static uint32_t rng_state = 0x12345678;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

/* Checks a reply against the query it came from. */
static void check_reply_shape(const uint8_t *q, size_t qn, const uint8_t *r,
                              size_t rn) {
  CHECK(rn >= 12 + 5 && rn <= DNS_MESSAGE_MAX);
  CHECK(r[0] == q[0] && r[1] == q[1]);
  CHECK((r[2] & 0xfe) == 0x84 && r[3] == 0);
  CHECK(r[4] == 0 && r[5] == 1);
  size_t ql = rn - 12 - (r[7] ? 16 : 0);
  CHECK(ql <= qn - 12);
  CHECK(memcmp(r + 12, q + 12, ql) == 0);
}

static void test_random(void) {
  uint8_t buf[DNS_MESSAGE_MAX + 8], got[DNS_MESSAGE_MAX];
  uint8_t base[64];
  size_t bn = from_hex(Q_A, base);

  for (int iter = 0; iter < 200000; iter++) {
    size_t len = rnd() % sizeof(buf);
    for (size_t i = 0; i < len; i++) {
      buf[i] = (uint8_t)rnd();
    }
    /* Half the time, make the header pass so the name parser is reached. */
    if (len >= 12 && (iter & 1)) {
      buf[2] &= 0x07;
      buf[4] = 0;
      buf[5] = 1;
      /* Short labels make it more likely to parse. */
      if (len > 12 && (iter & 2)) {
        for (size_t i = 12; i < len; i++) {
          buf[i] &= 0x0f;
        }
      }
    }
    size_t rn = reply_full(buf, len, got);
    if (rn > 0) {
      check_reply_shape(buf, len, got, rn);
    }
  }

  /* Mutations of the example query: flip bytes, then try every length. */
  for (int iter = 0; iter < 20000; iter++) {
    memcpy(buf, base, bn);
    int flips = 1 + (int)(rnd() % 4);
    for (int f = 0; f < flips; f++) {
      buf[rnd() % bn] = (uint8_t)rnd();
    }
    size_t len = rnd() % (bn + 1);
    size_t rn = reply_full(buf, len, got);
    if (rn > 0) {
      check_reply_shape(buf, len, got, rn);
    }
  }
}

int main(void) {
  guard_init();
  redirect_ip = inet_addr("192.168.4.1");
  test_guard_works();
  test_examples();
  test_truncations();
  test_header_rules();
  test_types();
  test_names();
  test_sizes();
  test_random();
  return TEST_RESULT();
}
