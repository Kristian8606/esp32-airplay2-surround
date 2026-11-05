/* Host tests of src/fairplay: the replies against CRC-32 values taken from
 * the published tables, the echo of message 3, and damaged input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap2_fairplay.h"

static int checks, failures;

#define CHECK(cond)                                                   \
  do {                                                                \
    checks++;                                                         \
    if (!(cond)) {                                                    \
      failures++;                                                     \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                 \
  } while (0)

static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}

/* CRC-32 of the four 142-byte replies and of the 12-byte header, computed
 * from the tables as published in playfair and shairport-sync. */
static const uint32_t reply_crc[4] = {0x38491132u, 0xe9a6feedu, 0xeda8c9b2u,
                                      0x5f0e36f6u};
static const uint32_t header_crc = 0x2ff18364u;

/* A 16-byte message 1: header (4 bytes follow), then the mode at byte 14
 * (bytes 12, 13 and 15 are not read). */
static void message1(uint8_t m[16], uint8_t mode) {
  static const uint8_t base[16] = {'F', 'P', 'L', 'Y', 3, 1, 1, 0,
                                   0,   0,   0,   4,   2, 0, 0, 0xbb};
  memcpy(m, base, 16);
  m[14] = mode;
}

static void message3(uint8_t m[164]) {
  static const uint8_t head[12] = {'F', 'P', 'L', 'Y', 3, 1, 3, 0, 0, 0, 0, 0x98};
  memcpy(m, head, 12);
  for (int i = 12; i < 164; i++) m[i] = (uint8_t)(i * 37 + 11);
}

static void test_modes(void) {
  for (uint8_t mode = 0; mode < 4; mode++) {
    uint8_t m[16], r[AP2_FAIRPLAY_REPLY_MAX];
    message1(m, mode);
    CHECK(ap2_fairplay_setup(m, sizeof(m), r, sizeof(r)) == 142);
    CHECK(crc32(r, 142) == reply_crc[mode]);
    CHECK(memcmp(r, "FPLY", 4) == 0 && r[4] == 3 && r[6] == 2 &&
          r[11] == 130 && r[12] == 2 && r[13] == mode);
  }
}

static void test_message3(void) {
  uint8_t m[164], r[AP2_FAIRPLAY_REPLY_MAX];
  message3(m);
  CHECK(ap2_fairplay_setup(m, sizeof(m), r, sizeof(r)) == 32);
  CHECK(crc32(r, 12) == header_crc);
  CHECK(r[6] == 4 && r[11] == 20);
  CHECK(memcmp(r + 12, m + 144, 20) == 0);
  /* The suffix is always the last 20 bytes, whatever the length. */
  CHECK(ap2_fairplay_setup(m, 32, r, 32) == 32);
  CHECK(memcmp(r + 12, m + 12, 20) == 0);
}

static void test_refused(void) {
  uint8_t m1[16], m3[164], r[AP2_FAIRPLAY_REPLY_MAX];
  message1(m1, 0);
  message3(m3);
  CHECK(ap2_fairplay_setup(NULL, 16, r, sizeof(r)) == -1);
  CHECK(ap2_fairplay_setup(m1, 16, NULL, sizeof(r)) == -1);
  for (size_t n = 0; n <= 14; n++) {
    CHECK(ap2_fairplay_setup(m1, n, r, sizeof(r)) == -1);
  }
  for (size_t n = 0; n < 32; n++) {
    CHECK(ap2_fairplay_setup(m3, n, r, sizeof(r)) == -1);
  }
  CHECK(ap2_fairplay_setup(m1, 16, r, 141) == -1);
  CHECK(ap2_fairplay_setup(m3, 164, r, 31) == -1);
  for (int mode = 4; mode < 256; mode++) {
    message1(m1, (uint8_t)mode);
    CHECK(ap2_fairplay_setup(m1, 16, r, sizeof(r)) == -1);
  }
  message1(m1, 0);
  for (int seq = 0; seq < 256; seq++) {
    if (seq == 1 || seq == 3) continue;
    m1[6] = (uint8_t)seq;
    CHECK(ap2_fairplay_setup(m1, 16, r, sizeof(r)) == -1);
  }
  message1(m1, 0);
  for (int v = 0; v < 256; v++) {
    if (v == 3) continue;
    m1[4] = (uint8_t)v;
    CHECK(ap2_fairplay_setup(m1, 16, r, sizeof(r)) == -1);
  }
  message1(m1, 0);
  for (int i = 0; i < 4; i++) {
    uint8_t save = m1[i];
    m1[i] ^= 0x20;
    CHECK(ap2_fairplay_setup(m1, 16, r, sizeof(r)) == -1);
    m1[i] = save;
  }
}

/* Random bodies of every length up to 300 in an exact-size heap buffer:
 * the sanitizers catch any read past the end. */
static void test_random(void) {
  srand(7);
  for (size_t n = 0; n <= 300; n++) {
    for (int k = 0; k < 200; k++) {
      uint8_t *m = malloc(n ? n : 1);
      for (size_t i = 0; i < n; i++) m[i] = (uint8_t)rand();
      if (n >= 7 && (k & 1)) {
        memcpy(m, "FPLY", 4);
        m[4] = 3;
        m[6] = (k & 2) ? 1 : 3;
      }
      uint8_t r[AP2_FAIRPLAY_REPLY_MAX];
      int len = ap2_fairplay_setup(m, n, r, sizeof(r));
      CHECK(len == -1 || len == 142 || len == 32);
      free(m);
    }
  }
}

int main(void) {
  test_modes();
  test_message3();
  test_refused();
  test_random();
  printf("fairplay: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
