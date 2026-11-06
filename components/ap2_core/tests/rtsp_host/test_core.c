/* SPDX-License-Identifier: Apache-2.0 */
/* The request rules against the spec's examples, and broken requests. */
#include <stdlib.h>

#include "ap2_hap.h"
#include "rtsp_msg.h"
#include "test.h"

#define RT_STREAM "type=i96,audioFormat=i262144,shk=d32,controlPort=i6002,spf=i352,ct=i2,sr=i44100"
#define AAC71_STREAM "type=i103,audioFormat=i0x10000000000,shk=d32,spf=i1024,ct=i8,sr=r48000.0"
#define AAC_STREAM "type=i103,audioFormat=i8388608,shk=d32,spf=i1024,ct=i8,sr=i48000"

static char hdr[256];

/* A paired sender with a session. */
static client_t *sender_with_session(uint8_t last_ip_byte) {
  client_t *c = client_new(192, 168, 1, last_ip_byte);
  client_pair_transient(c);
  client_setup_session(c);
  return c;
}

static void test_options(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  CHECK_INT(client_request(c, "OPTIONS", "*", NULL, NULL, 0), 0);
  CHECK(client_reply(c, &r));
  CHECK_INT(r.code, 200);
  CHECK_STR(reply_header(&r, "Public", hdr, sizeof(hdr)),
            "SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, OPTIONS, POST, GET, "
            "SET_PARAMETER, GET_PARAMETER, SETPEERS, SETPEERSX, SETRATEANCHORTIME, "
            "LOUDNESSNORMALIZATION");
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "1");
  CHECK_STR(reply_header(&r, "Server", hdr, sizeof(hdr)), "AirTunes/377.40.00");
  CHECK(reply_header(&r, "Content-Length", hdr, sizeof(hdr)) == NULL);
  CHECK(strncmp(r.head, "RTSP/1.0 200 OK\r\nCSeq: 1\r\nServer: AirTunes/377.40.00\r\n", 52) == 0);
  client_free(c);
}

static void test_unknown_method(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  int warnings = log_warnings();
  CHECK_INT(client_request(c, "ANNOUNCE", "rtsp://x/1", NULL, "v=0", 3), 0);
  CHECK(client_reply(c, &r));
  CHECK_INT(r.code, 501);
  CHECK(strncmp(r.head, "RTSP/1.0 501 Not Implemented\r\n", 30) == 0);
  CHECK_STR(reply_header(&r, "Content-Type", hdr, sizeof(hdr)), "text/plain");
  CHECK_STR(reply_text(&r), "Not Implemented");
  CHECK(log_warnings() > warnings);
  /* the connection stays usable */
  client_request(c, "OPTIONS", "*", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  client_free(c);
}

static void test_info(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  const char *t;
  client_request(c, "GET", "/info", NULL, NULL, 0);
  CHECK(client_reply(c, &r));
  CHECK_INT(r.code, 200);
  CHECK_STR(reply_header(&r, "Content-Type", hdr, sizeof(hdr)), "application/x-apple-binary-plist");
  t = reply_text(&r);
  CHECK(strstr(t, "deviceid=A4:CF:12:0B:FE:0D;") != NULL);
  CHECK(strstr(t, "features=496155697629696;") != NULL);
  CHECK(strstr(t, "model=AudioAccessory6,1;") != NULL);
  CHECK(strstr(t, "protovers=1.1;") != NULL);
  CHECK(strstr(t, "srcvers=377.40.00;") != NULL);
  CHECK(strstr(t, "vv=2;") != NULL);
  CHECK(strstr(t, "statusFlags=4;") != NULL);
  CHECK(strstr(t, "pk=32:ed..25;") != NULL);
  CHECK(strstr(t, "pi=0b9c2f4e-1d3a-4c5b-8e7f-6a5b4c3d2e1f;") != NULL);
  CHECK(strstr(t, "name=Gateway;") != NULL);
  CHECK(strstr(t, "initialVolume=-15.0000;") != NULL);
  CHECK(strstr(t, "audioStream=262144;") != NULL);
  CHECK(strstr(t, "bufferStream=1649282383872") != NULL);
  /* with a body naming a qualifier: the same reply */
  {
    char first[512];
    snprintf(first, sizeof(first), "%s", t);
    client_request_text(c, "GET", "/info", "Content-Type: application/x-apple-binary-plist\r\n",
                        "bplist00qualifier=stxtAirPlay");
    CHECK(client_reply(c, &r) && r.code == 200);
    CHECK_STR(reply_text(&r), first);
  }
  /* encrypted too */
  client_pair_transient(c);
  client_request(c, "GET", "/info", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK(strstr(reply_text(&r), "statusFlags=4;") != NULL);
  /* unknown paths: 501 like an unknown method */
  client_request(c, "GET", "/nothing", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 501);
  CHECK_STR(reply_header(&r, "Content-Type", hdr, sizeof(hdr)), "text/plain");
  CHECK_STR(reply_text(&r), "Not Implemented");
  client_request(c, "POST", "/nothing", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 501);
  CHECK_STR(reply_text(&r), "Not Implemented");
  CHECK_INT(c->rc, 0);
  client_free(c);
}

static void test_info_without_saved_volume(void) {
  /* spec "GET /info": -24.0 when the saved volume cannot be read */
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  fake.saved_volume_missing = true;
  client_request(c, "GET", "/info", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK(strstr(reply_text(&r), "initialVolume=-24.0000;") != NULL);
  /* and the volume a session starts with: ((-24 + 30) / 30)^2 -> 1310 */
  client_pair_transient(c);
  calls_clear();
  client_setup_session(c);
  CHECK(calls_find("audio_set_volume_q15(1310)") >= 0);
  client_request_text(c, "GET_PARAMETER", "rtsp://x/1", NULL, "volume\r\n");
  CHECK(client_reply(c, &r));
  CHECK_STR(reply_text(&r), "volume: -24.00\r\n");
  client_free(c);
}

static void test_method_case(void) {
  /* spec "Connections and messages": methods are compared exactly */
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  client_request(c, "options", "*", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 501);
  client_request(c, "Get", "/info", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 501);
  /* header names are not */
  CHECK_INT(client_raw(c, "OPTIONS * RTSP/1.0\r\ncseq: 7\r\n\r\n", 31), 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "7");
  /* any version token; the reply is RTSP/1.0 */
  CHECK_INT(client_raw(c, "OPTIONS * HTTP/1.1\r\nCSeq: 8\r\n\r\n", 32), 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK(strncmp(r.head, "RTSP/1.0 200 OK\r\n", 17) == 0);
  client_free(c);
}

static void test_cseq_and_broken_requests(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  /* no CSeq: answered without one */
  CHECK_INT(client_raw(c, "OPTIONS * RTSP/1.0\r\n\r\n", 22), 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK(reply_header(&r, "CSeq", hdr, sizeof(hdr)) == NULL);
  /* the CSeq is copied as it is */
  CHECK_INT(client_raw(c, "OPTIONS * RTSP/1.0\r\nCSeq:   42  \r\n\r\n", 36), 0);
  CHECK(client_reply(c, &r));
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "42");
  /* a body over the limit: 413, the connection goes on */
  {
    size_t n = RTSP_BODY_MAX + 1;
    char *body = malloc(n);
    memset(body, 'a', n);
    CHECK_INT(client_request(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: image/jpeg\r\n",
                             body, n), 0);
    CHECK(client_reply(c, &r) && r.code == 413);
    free(body);
    client_request(c, "OPTIONS", "*", NULL, NULL, 0);
    CHECK(client_reply(c, &r) && r.code == 200);
  }
  /* no memory for a body */
  fake.malloc_fail = true;
  client_request(c, "POST", "/command", NULL, "bplist00type=sx", 15);
  fake.malloc_fail = false;
  CHECK(client_reply(c, &r) && r.code == 500);
  /* malformed headers: 400, Connection: close, the connection ends */
  CHECK_INT(client_raw(c, "OPTIONS * RTSP/1.0\r\nCSeq 6\r\n\r\n", 30), -1);
  CHECK(client_reply(c, &r) && r.code == 400);
  CHECK_STR(reply_header(&r, "Connection", hdr, sizeof(hdr)), "close");
  client_free(c);

  c = client_new(192, 168, 1, 10);
  CHECK_INT(client_raw(c, "GET /info RTSP/1.0\r\nContent-Length: x\r\n\r\n", 41), -1);
  CHECK(client_reply(c, &r) && r.code == 400);
  client_free(c);
  /* a request line without version: a head that cannot be read */
  c = client_new(192, 168, 1, 10);
  CHECK_INT(client_raw(c, "OPTIONS *\r\nCSeq: 5\r\n\r\n", 22), -1);
  CHECK(client_reply(c, &r) && r.code == 400);
  CHECK_STR(reply_header(&r, "Connection", hdr, sizeof(hdr)), "close");
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "5");
  CHECK(!client_reply(c, &r));
  client_free(c);
  /* the same inside a session: the session ends with the connection */
  c = sender_with_session(10);
  CHECK_INT(client_raw(c, "OPTIONS *\r\nCSeq: 6\r\n\r\n", 22), -1);
  CHECK(client_reply(c, &r) && r.code == 400);
  client_free(c);
  CHECK(rtsp_core_idle());
}

static void test_pipelined_requests(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  const char *two = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\nGET /nothing RTSP/1.0\r\nCSeq: 2\r\n\r\n";
  CHECK_INT(client_raw(c, two, strlen(two)), 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "1");
  CHECK(client_reply(c, &r) && r.code == 501);
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "2");
  client_free(c);
}

static void test_pairing_transient(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  int pos = 0;
  client_pair_transient(c);
  CHECK_CALL(pos, "hap_setup_m1()");
  CHECK_CALL(pos, "hap_setup_m3()");
  /* the control channel keys are the core's, not derived again; the event
   * keys are derived over the 64-byte SRP key */
  CHECK_INT(calls_with("hkdf(Control-Salt"), 0);
  CHECK(calls_find("hkdf(Events-Salt,Events-Write-Encryption-Key,64)") >= 0);
  CHECK(calls_find("hkdf(Events-Salt,Events-Read-Encryption-Key,64)") >= 0);
  /* the next request is encrypted both ways (the test sender seals it and
   * opens the reply with the spec's keys) */
  client_request(c, "OPTIONS", "*", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_INT(c->rc, 0);
  /* one frame each way, counted in the core */
  CHECK_INT(fake.channel_counted[AP2_HAP_RECEIVE], 1);
  CHECK_INT(fake.channel_counted[AP2_HAP_SEND], 1);
  /* a plain request now is a broken frame: the connection closes */
  c->encrypted = false;
  CHECK_INT(client_request(c, "OPTIONS", "*", NULL, NULL, 0), -1);
  client_free(c);
}

static void test_pairing_verify_and_bad_frame(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  client_pair_verify(c);
  CHECK(calls_find("hap_verify_m1()") >= 0);
  CHECK_INT(calls_with("hkdf(Control-Salt"), 0);
  CHECK(calls_find("hkdf(Events-Salt,Events-Write-Encryption-Key,32)") >= 0);
  client_request(c, "OPTIONS", "*", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  /* a frame that does not open closes the connection */
  {
    uint8_t frame[64];
    size_t n = rtsp_frame_seal(&c->send_cipher, (const uint8_t *)"OPTIONS", 7, frame);
    frame[4] ^= 0x40;
    CHECK_INT(client_feed(c, frame, n), -1);
  }
  client_free(c);
  /* a frame longer than 1024 */
  c = client_new(192, 168, 1, 10);
  client_pair_verify(c);
  {
    uint8_t frame[2] = {0x01, 0x04};
    CHECK_INT(client_feed(c, frame, 2), -1);
  }
  client_free(c);
}

static void test_encryption_starts_mid_buffer(void) {
  /* the last pair-verify request and the first encrypted request arrive in
   * one read */
  client_t *c = client_new(192, 168, 1, 10);
  uint8_t buf[512], m3[3] = {0x06, 0x01, 0x03};
  size_t n;
  reply_t r;
  uint8_t to_gw[32], from_gw[32];
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x01, 0x01}, 3);
  CHECK(client_reply(c, &r) && r.code == 200);
  n = (size_t)snprintf((char *)buf, sizeof(buf),
                       "POST /pair-verify RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 3\r\n\r\n");
  memcpy(buf + n, m3, 3);
  n += 3;
  derive_test_key(false, "Control-Salt", "Control-Write-Encryption-Key", to_gw);
  derive_test_key(false, "Control-Salt", "Control-Read-Encryption-Key", from_gw);
  rtsp_cipher_init(&c->send_cipher, to_gw);
  n += rtsp_frame_seal(&c->send_cipher, (const uint8_t *)"OPTIONS * RTSP/1.0\r\nCSeq: 3\r\n\r\n", 31,
                       buf + n);
  client_free(c);

  c = client_new(192, 168, 1, 10);
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x01, 0x01}, 3);
  client_reply(c, &r);
  rtsp_cipher_init(&c->send_cipher, to_gw);
  n = (size_t)snprintf((char *)buf, sizeof(buf),
                       "POST /pair-verify RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 3\r\n\r\n");
  memcpy(buf + n, m3, 3);
  n += 3;
  n += rtsp_frame_seal(&c->send_cipher, (const uint8_t *)"OPTIONS * RTSP/1.0\r\nCSeq: 3\r\n\r\n", 31,
                       buf + n);
  CHECK_INT(client_raw(c, buf, n), 0); /* sent as plain bytes by the test */
  /* the plain reply to pair-verify M3 */
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "2");
  /* the rest of the output is one encrypted reply */
  {
    rtsp_frame_reader_t fr;
    const uint8_t *p;
    size_t used, pl;
    rtsp_frame_reader_init(&fr, from_gw);
    CHECK(rtsp_frame_feed(&fr, c->plain, c->plain_len, &used, &p, &pl) == RTSP_FRAME_DONE);
    CHECK(pl > 0 && strncmp((const char *)p, "RTSP/1.0 200 OK\r\nCSeq: 3\r\n", 26) == 0);
  }
  client_free(c);
}

static void test_channel_counters(void) {
  /* spec "Encryption": the core's counters are the nonces, and the server
   * counts each frame there. Started at 5 here, so a server counting on
   * its own from 0 would not open. */
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  fake.channel_counter_start = 5;
  client_pair_verify(c);
  for (int i = 0; i < 3; i++) {
    client_request(c, "OPTIONS", "*", NULL, NULL, 0);
    CHECK(client_reply(c, &r) && r.code == 200);
  }
  CHECK_INT(fake.channel_counted[AP2_HAP_RECEIVE], 3);
  CHECK_INT(fake.channel_counted[AP2_HAP_SEND], 3);
  /* a reply with a body of 1024 bytes or less in all: one frame, head and
   * body together (spec "Encryption") */
  c->frames_in = 0;
  client_request(c, "GET", "/info", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK(r.body_len > 0 && strlen(r.head) + r.body_len <= 1024);
  CHECK_INT(c->frames_in, 1);
  CHECK_INT(c->frame_lens[0], strlen(r.head) + r.body_len);
  CHECK_INT(fake.channel_counted[AP2_HAP_SEND], 4);
  /* longer: frames of 1024, the rest in the last, cut across head and
   * body */
  {
    static const uint8_t m1[16] = {'F', 'P', 'L', 'Y', 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, 2, 0};
    size_t total;
    fake.fp_reply_len = 2500;
    c->frames_in = 0;
    client_request(c, "POST", "/fp-setup", "Content-Type: application/octet-stream\r\n", m1, 16);
    CHECK(client_reply(c, &r) && r.code == 200);
    CHECK_INT(r.body_len, 2500);
    CHECK(r.body[0] == 0x46 && r.body[2499] == 0x46);
    total = strlen(r.head) + r.body_len;
    CHECK_INT(c->frames_in, 3);
    CHECK_INT(c->frame_lens[0], 1024);
    CHECK_INT(c->frame_lens[1], 1024);
    CHECK_INT(c->frame_lens[2], total - 2048);
    CHECK_INT(fake.channel_counted[AP2_HAP_SEND], 7);
    /* exactly 1024 in all: still one frame */
    /* the body gets 3 digits of Content-Length instead of 4 */
    fake.fp_reply_len = (int)(1024 - (strlen(r.head) - 1));
    c->frames_in = 0;
    client_request(c, "POST", "/fp-setup", "Content-Type: application/octet-stream\r\n", m1, 16);
    CHECK(client_reply(c, &r) && r.code == 200);
    CHECK_INT(strlen(r.head) + r.body_len, 1024);
    CHECK_INT(c->frames_in, 1);
    fake.fp_reply_len = 0;
  }
  /* a request over one frame: two frames in */
  {
    char body[1500];
    memset(body, 'a', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: image/jpeg\r\n", body);
    CHECK(client_reply(c, &r) && r.code == 200);
    CHECK_INT(fake.channel_counted[AP2_HAP_RECEIVE], 8); /* 6 requests before it */
  }
  CHECK_INT(c->rc, 0);
  client_free(c);

  /* the keys are taken only once pairing has ended */
  CHECK_INT(fake.channel_key_early, 0);
}

static void test_pairing_failures(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  /* refused by the core with a well-formed message: its {State, Error}
   * reply, 200 */
  fake.hap_mode = 1;
  client_request(c, "POST", "/pair-setup", NULL, (const uint8_t[]){0x06, 0x01, 0x01}, 3);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "Content-Type", hdr, sizeof(hdr)), "application/octet-stream");
  CHECK_INT(r.body_len, 6);
  CHECK(memcmp(r.body, (const uint8_t[]){0x06, 0x01, 0x02, 0x07, 0x01, 0x02}, 6) == 0);
  /* an error of the core: 470, no body, the connection stays */
  fake.hap_mode = 2;
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x01, 0x03}, 3);
  CHECK(client_reply(c, &r) && r.code == 470);
  CHECK(strncmp(r.head, "RTSP/1.0 470 Connection Authorization Required\r\n", 48) == 0);
  CHECK_INT(r.body_len, 0);
  CHECK(reply_header(&r, "Content-Type", hdr, sizeof(hdr)) == NULL);
  CHECK(reply_header(&r, "Connection", hdr, sizeof(hdr)) == NULL);
  CHECK_INT(c->rc, 0);
  fake.hap_mode = 0;
  /* no State, an unknown State, a truncated TLV, an empty body: 470 */
  calls_clear();
  client_request(c, "POST", "/pair-setup", NULL, (const uint8_t[]){0x01, 0x01, 0x01}, 3);
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  client_request(c, "POST", "/pair-setup", NULL, (const uint8_t[]){0x06, 0x01, 0x09}, 3);
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  client_request(c, "POST", "/pair-setup", NULL, (const uint8_t[]){0x06, 0x02, 0x01, 0x00}, 4);
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x05, 0x01}, 3);
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  client_request(c, "POST", "/pair-verify", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x01, 0x05}, 3);
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  CHECK_INT(calls_count(), 0); /* the core was not asked */
  /* M3 before M1: the core's error, 470 */
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x01, 0x03}, 3);
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  /* still plain, still open */
  client_request(c, "OPTIONS", "*", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  /* a refused last step does not encrypt */
  fake.hap_mode = 1;
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x01, 0x01}, 3);
  client_reply(c, &r);
  client_request(c, "POST", "/pair-verify", NULL, (const uint8_t[]){0x06, 0x01, 0x03}, 3);
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 6);
  fake.hap_mode = 0;
  client_request(c, "OPTIONS", "*", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  /* and after all that, a pairing still works */
  client_pair_verify(c);
  client_request(c, "OPTIONS", "*", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_INT(c->rc, 0);
  client_free(c);

  /* no memory for the pairing session: 500 */
  c = client_new(192, 168, 1, 10);
  fake.hap_new_fail = true;
  client_request(c, "POST", "/pair-setup", NULL, (const uint8_t[]){0x06, 0x01, 0x01}, 3);
  CHECK(client_reply(c, &r) && r.code == 500 && r.body_len == 0);
  fake.hap_new_fail = false;
  client_request(c, "POST", "/pair-setup", NULL, (const uint8_t[]){0x06, 0x01, 0x01}, 3);
  CHECK(client_reply(c, &r) && r.code == 200);
  client_free(c);
}

static void test_fp_setup(void) {
  client_t *c = client_new(192, 168, 1, 10);
  reply_t r;
  uint8_t m1[16] = {'F', 'P', 'L', 'Y', 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, 2, 0xbb};
  uint8_t m3[164] = {'F', 'P', 'L', 'Y', 3, 1, 3, 0, 0, 0, 0, 0x98};
  int warnings;
  client_pair_transient(c);
  client_request(c, "POST", "/fp-setup", NULL, m1, sizeof(m1));
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 142);
  CHECK_STR(reply_header(&r, "Content-Type", hdr, sizeof(hdr)), "application/octet-stream");
  client_request(c, "POST", "/fp-setup", NULL, m3, sizeof(m3));
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 32);
  /* unknown: one warning, one byte 0x00 */
  warnings = log_warnings();
  client_request(c, "POST", "/fp-setup", NULL, "garbage", 7);
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 1 && r.body[0] == 0);
  CHECK_INT(log_warnings(), warnings + 1);
  client_request(c, "POST", "/fp-setup", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 1 && r.body[0] == 0);
  client_free(c);
}

static void test_session_setup(void) {
  client_t *c = client_new(192, 168, 1, 20);
  reply_t r;
  int pos, port;
  client_pair_transient(c);
  calls_clear();
  port = client_setup_session(c);
  CHECK_INT(port, 50000);
  pos = 0;
  CHECK_CALL(pos, "apple_clock_select(192.168.1.20,0)");
  CHECK_CALL(pos, "event_open(");
  CHECK_CALL(pos, "audio_set_encryption(1,32,a0bf)"); /* the pairing's audio key */
  CHECK_CALL(pos, "audio_set_volume_q15(8192)");     /* saved -15.0 */
  CHECK_CALL(pos, "amp_connected()");
  CHECK_CALL(pos, "event(CLIENT_CONNECTED)");
  CHECK(!rtsp_core_idle());
  /* the event keys of the spec */
  {
    uint8_t w[32], rd[32];
    derive_test_key(true, "Events-Salt", "Events-Write-Encryption-Key", w);
    derive_test_key(true, "Events-Salt", "Events-Read-Encryption-Key", rd);
    CHECK(memcmp(w, fake.event_write_key, 32) == 0);
    CHECK(memcmp(rd, fake.event_read_key, 32) == 0);
  }
  /* another or no timingProtocol: one log line, then the same */
  {
    client_t *d = client_new(192, 168, 1, 23);
    int warnings;
    client_pair_transient(d);
    warnings = log_warnings();
    calls_clear();
    client_request_text(d, "SETUP", "rtsp://x/1", NULL, "bplist00timingProtocol=sNTP");
    CHECK(client_reply(d, &r) && r.code == 200);
    CHECK_INT(log_warnings(), warnings + 1);
    CHECK(log_contains("timingProtocol NTP"));
    client_request_text(d, "SETUP", "rtsp://x/1", NULL, "bplist00macAddress=sAA:BB");
    CHECK(client_reply(d, &r) && r.code == 200);
    CHECK_INT(log_warnings(), warnings + 2);
    CHECK_INT(calls_with("apple_clock_select"), 0); /* c holds the session */
    client_free(d);
  }
  /* SETUP again: the same port, which keeps listening; nothing new opens
   * or starts (spec "A sender sending it again") */
  calls_clear();
  CHECK_INT(client_setup_session(c), 50000);
  CHECK_INT(calls_with("amp_connected"), 0);
  CHECK_INT(calls_with("event_open"), 0);
  CHECK_INT(calls_with("event_close"), 0);
  CHECK_INT(calls_with("apple_clock_select"), 0);
  CHECK(fake_event_port(c->conn, NULL, NULL, NULL));
  /* RECORD, SETPEERS, ... : 200, no body, nothing changes */
  calls_clear();
  {
    const char *methods[] = {"RECORD", "SETPEERS", "SETPEERSX", "LOUDNESSNORMALIZATION"};
    for (int i = 0; i < 4; i++) {
      client_request_text(c, methods[i], "rtsp://x/1", NULL, "bplist00a=i1");
      CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 0);
    }
    client_request_text(c, "POST", "/command", NULL, "bplist00type=supdateMRSupportedCommands");
    CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 0);
    CHECK(log_contains("updateMRSupportedCommands"));
    client_request_text(c, "POST", "/command", NULL, "not a plist");
    CHECK(client_reply(c, &r) && r.code == 200);
    client_request_text(c, "POST", "/audioMode", NULL, "bplist00audioMode=sdefault");
    CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 0);
  }
  CHECK_INT(calls_count(), 0);
  client_free(c);
  CHECK(rtsp_core_idle());
}

static void test_setup_refused(void) {
  client_t *c = client_new(192, 168, 1, 20);
  reply_t r;
  /* not encrypted: 470, nothing changes; a stream SETUP too */
  client_request_text(c, "SETUP", "rtsp://x/1", NULL, "bplist00timingProtocol=sPTP");
  CHECK(client_reply(c, &r) && r.code == 470 && r.body_len == 0);
  CHECK_INT(client_setup_stream(c, RT_STREAM, &r), 470);
  client_request_text(c, "SETUP", "rtsp://x/1", NULL, "not a plist");
  CHECK(client_reply(c, &r) && r.code == 470);
  /* half a pairing is not encrypted either */
  client_request(c, "POST", "/pair-setup", NULL,
                 (const uint8_t[]){0x06, 0x01, 0x01, 0x13, 0x01, 0x10}, 6);
  CHECK(client_reply(c, &r) && r.code == 200);
  client_request_text(c, "SETUP", "rtsp://x/1", NULL, "bplist00timingProtocol=sPTP");
  CHECK(client_reply(c, &r) && r.code == 470);
  CHECK(calls_find("hap_setup_m1()") == 0);
  CHECK_INT(calls_count(), 1);
  CHECK(rtsp_core_idle());
  client_free(c);
  c = client_new(192, 168, 1, 20);
  client_pair_transient(c);
  calls_clear();
  /* not a plist */
  client_request_text(c, "SETUP", "rtsp://x/1", NULL, "{\"streams\":[]}");
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request(c, "SETUP", "rtsp://x/1", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 400);
  /* a stream before the session SETUP */
  CHECK_INT(client_setup_stream(c, RT_STREAM, &r), 455);
  CHECK_INT(calls_count(), 0);
  /* the event port does not open: 500, nothing held */
  fake.event_open_err = ESP_FAIL;
  client_request_text(c, "SETUP", "rtsp://x/1", NULL, "bplist00timingProtocol=sPTP");
  CHECK(client_reply(c, &r) && r.code == 500);
  CHECK(rtsp_core_idle());
  CHECK(calls_find("apple_clock_release()") >= 0);
  CHECK_INT(calls_with("amp_connected"), 0);
  fake.event_open_err = ESP_OK;
  client_free(c);
}

static void test_stream_realtime(void) {
  client_t *c = sender_with_session(20);
  reply_t r;
  int pos = 0;
  calls_clear();
  CHECK_INT(client_setup_stream(c, RT_STREAM, &r), 200);
  CHECK_STR(reply_text(&r), "STREAM type=96 dataPort=6000 controlPort=6001 audioBufferSize=0");
  CHECK_STR(reply_header(&r, "Content-Type", hdr, sizeof(hdr)), "application/x-apple-binary-plist");
  CHECK_CALL(pos, "audio_set_stream_type(96)");
  CHECK_CALL(pos, "audio_set_format(ALAC,44100,2,16,352)");
  CHECK_CALL(pos, "audio_set_encryption(1,32,a0bf)"); /* shk */
  CHECK_CALL(pos, "apple_clock_select(192.168.1.20,1)");
  CHECK_CALL(pos, "audio_start_stream(6000,6001,0)");
  CHECK_CALL(pos, "audio_latency(11025)");
  CHECK_CALL(pos, "audio_client_control(192.168.1.20,6002)");
  CHECK_CALL(pos, "audio_set_playing(1)");
  CHECK_CALL(pos, "event(PLAYING)");
  CHECK_INT(calls_with("amp_connected"), 0); /* the session was there */
  /* FLUSH with RTP-Info */
  calls_clear();
  client_request(c, "FLUSH", "rtsp://x/1", "RTP-Info: seq=10;rtptime=123456\r\n", NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(calls_get(0), "audio_realtime_flush(123456)");
  client_request(c, "FLUSH", "rtsp://x/1", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request(c, "FLUSH", "rtsp://x/1", "RTP-Info: rtptime=abc\r\n", NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 400);
  CHECK_INT(calls_count(), 1);
  client_free(c);
}

static void test_stream_buffered(void) {
  client_t *c = sender_with_session(21);
  reply_t r;
  int pos = 0;
  calls_clear();
  CHECK_INT(client_setup_stream(c, AAC71_STREAM, &r), 200);
  CHECK_STR(reply_text(&r), "STREAM type=103 dataPort=7100 controlPort=0 audioBufferSize=6291456");
  CHECK_CALL(pos, "audio_set_stream_type(103)");
  CHECK_CALL(pos, "audio_set_format(AAC,48000,2,24,1024)");
  CHECK_CALL(pos, "audio_set_encryption(1,32,a0bf)");
  CHECK_CALL(pos, "apple_clock_select(192.168.1.21,0)");
  CHECK_CALL(pos, "audio_start_buffered(0)");
  CHECK_CALL(pos, "audio_latency(0)");
  CHECK_INT(calls_with("audio_set_playing(1)"), 0);
  CHECK_INT(calls_with("event(PLAYING)"), 0);
  /* /feedback while a stream is set up: the stream's type and rate */
  client_request(c, "POST", "/feedback", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_text(&r), "FEEDBACK type=103 sr=48000.0");
  /* a new stream replaces it (format change): stop, then the new one */
  calls_clear();
  pos = 0;
  CHECK_INT(client_setup_stream(c, "type=i103,audioFormat=i262144,shk=d32", &r), 200);
  CHECK_CALL(pos, "audio_set_playing(0)");
  CHECK_CALL(pos, "audio_stop()");
  CHECK_CALL(pos, "audio_set_stream_type(103)");
  CHECK_CALL(pos, "audio_set_format(ALAC,44100,2,16,352)");
  client_request(c, "POST", "/feedback", NULL, NULL, 0);
  CHECK(client_reply(c, &r));
  CHECK_STR(reply_text(&r), "FEEDBACK type=103 sr=44100.0");
  /* TEARDOWN with streams: the session stays, no body on /feedback */
  calls_clear();
  pos = 0;
  client_request_text(c, "TEARDOWN", "rtsp://x/1", NULL, "bplist00streams=[{type=i103}]");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK(reply_header(&r, "Connection", hdr, sizeof(hdr)) == NULL);
  CHECK_INT(c->rc, 0);
  CHECK_CALL(pos, "audio_set_playing(0)");
  CHECK_CALL(pos, "audio_stop()");
  CHECK_CALL(pos, "event(PAUSED)");
  CHECK_INT(calls_with("amp_disconnected"), 0);
  CHECK(!rtsp_core_idle());
  client_request(c, "POST", "/feedback", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 0);
  client_free(c);
}

static void test_stream_formats(void) {
  static const struct {
    const char *stream;
    int code;
    const char *format;
  } cases[] = {
      {"type=i103,audioFormat=i2097152,shk=d32", 200, "audio_set_format(ALAC,48000,2,24,352)"},
      {"type=i103,audioFormat=i4194304,shk=d32", 200, "audio_set_format(AAC,44100,2,24,1024)"},
      {AAC_STREAM, 200, "audio_set_format(AAC,48000,2,24,1024)"},
      {"type=i103,audioFormat=i0x8000000000,shk=d32", 200, "audio_set_format(AAC,48000,2,24,1024)"},
      /* not in the table: 400 and nothing changes */
      {"type=i96,audioFormat=i2097152,shk=d32,controlPort=i6002", 400, NULL},
      {"type=i103,audioFormat=i524288,shk=d32", 400, NULL},
      {"type=i110,audioFormat=i262144,shk=d32", 400, NULL},
      /* broken streams */
      {"type=i103,shk=d32", 400, NULL},
      {"audioFormat=i262144,shk=d32", 400, NULL},
      {"type=s103,audioFormat=i262144,shk=d32", 400, NULL},
      {"type=i103,audioFormat=i262144", 400, NULL},
      {"type=i103,audioFormat=i262144,shk=d16", 400, NULL},
      {"type=i103,audioFormat=i262144,shk=d33", 400, NULL},
      {"type=i96,audioFormat=i262144,shk=d32", 400, NULL},
      {"type=i96,audioFormat=i262144,shk=d32,controlPort=i0", 400, NULL},
      {"type=i96,audioFormat=i262144,shk=d32,controlPort=i70000", 400, NULL},
      {"type=i103,audioFormat=i262144,shk=d32,sr=r44100.5", 400, NULL},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    client_t *c = sender_with_session(30);
    reply_t r;
    calls_clear();
    if (client_setup_stream(c, cases[i].stream, &r) != cases[i].code) {
      t_failures++;
      fprintf(stderr, "stream case %zu: code %d\n", i, r.code);
    }
    t_checks++;
    if (cases[i].format != NULL) {
      CHECK(calls_find(cases[i].format) >= 0);
    } else {
      CHECK_INT(calls_count(), 0);
    }
    client_free(c);
  }
  /* two streams, none, a broken plist */
  {
    client_t *c = sender_with_session(30);
    reply_t r;
    calls_clear();
    client_request_text(c, "SETUP", "rtsp://x/1", NULL,
                        "bplist00streams=[{" AAC_STREAM "}{" AAC_STREAM "}]");
    CHECK(client_reply(c, &r) && r.code == 400);
    client_request_text(c, "SETUP", "rtsp://x/1", NULL, "bplist00streams=[]");
    CHECK(client_reply(c, &r) && r.code == 400);
    client_request_text(c, "SETUP", "rtsp://x/1", NULL, "bplist00streams=[{type=i103");
    CHECK(client_reply(c, &r) && r.code == 200); /* unreadable: taken as a session SETUP */
    CHECK_INT(calls_count(), 0);
    client_free(c);
  }
}

static void test_stream_start_failures(void) {
  client_t *c = sender_with_session(20);
  reply_t r;
  fake.udp_pair_err = ESP_FAIL;
  CHECK_INT(client_setup_stream(c, RT_STREAM, &r), 500);
  fake.udp_pair_err = ESP_OK;
  fake.start_stream_err = ESP_FAIL;
  CHECK_INT(client_setup_stream(c, RT_STREAM, &r), 500);
  fake.start_stream_err = ESP_OK;
  fake.start_buffered_err = ESP_FAIL;
  CHECK_INT(client_setup_stream(c, AAC_STREAM, &r), 500);
  fake.start_buffered_err = ESP_OK;
  /* the session is still there; no stream */
  CHECK(!rtsp_core_idle());
  client_request(c, "POST", "/feedback", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.body_len == 0);
  CHECK_INT(client_setup_stream(c, AAC_STREAM, &r), 200);
  client_free(c);
}

static void test_volume(void) {
  client_t *c = sender_with_session(20);
  reply_t r;
  static const struct {
    const char *text;
    int q15;
  } seen[] = {
      {"-15.000000", 8192}, {"-13.125000", 10368}, {"-18.750000", 4608},
      {"-24.338500", 1166}, {"-20.625000", 3200},  {"-20.000000", 3640},
      {"-144.000000", 0},   {"-30.000000", 0},     {"0.000000", 32768},
  };
  char body[64], expect[64];
  for (size_t i = 0; i < sizeof(seen) / sizeof(seen[0]); i++) {
    calls_clear();
    snprintf(body, sizeof(body), "volume: %s\r\n", seen[i].text);
    client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n", body);
    CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 0);
    snprintf(expect, sizeof(expect), "audio_set_volume_q15(%d)", seen[i].q15);
    CHECK_STR(calls_get(0), expect);
    /* every change to the settings (RAM); not written to flash */
    snprintf(expect, sizeof(expect), "settings_set_volume(%.4f)", (float)atof(seen[i].text));
    CHECK_STR(calls_get(1), expect);
    CHECK_INT(calls_count(), 2);
  }
  /* GET_PARAMETER volume: two decimals */
  client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                      "volume: -20.625000\r\n");
  client_reply(c, &r);
  client_request_text(c, "GET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                      "volume\r\n");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "Content-Type", hdr, sizeof(hdr)), "text/parameters");
  CHECK_STR(reply_text(&r), "volume: -20.62\r\n");
  /* another name: 200, no body */
  client_request_text(c, "GET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                      "progress\r\n");
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 0);
  client_request(c, "GET_PARAMETER", "rtsp://x/1", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200 && r.body_len == 0);
  /* progress, metadata, artwork: accepted, nothing called */
  calls_clear();
  client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                      "progress: 1146221540/1146549853/1195701740\r\n");
  CHECK(client_reply(c, &r) && r.code == 200);
  client_request_text(c, "SET_PARAMETER", "rtsp://x/1",
                      "Content-Type: application/x-dmap-tagged\r\n", "mlit\x00\x00\x00\x08");
  CHECK(client_reply(c, &r) && r.code == 200);
  client_request(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: image/jpeg\r\n",
                 "\xff\xd8\xff\xe0", 4);
  CHECK(client_reply(c, &r) && r.code == 200);
  /* broken volumes: 400, the engine keeps its gain */
  {
    const char *bad[] = {"volume: loud\r\n", "volume:\r\n", "volume: -15dB\r\n",
                         "volume: nan\r\n"};
    for (int i = 0; i < 4; i++) {
      client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                          bad[i]);
      CHECK(client_reply(c, &r) && r.code == 400);
    }
  }
  CHECK_INT(calls_count(), 0);
  client_request_text(c, "GET_PARAMETER", "rtsp://x/1", NULL, "volume\r\n");
  CHECK(client_reply(c, &r));
  CHECK_STR(reply_text(&r), "volume: -20.62\r\n");
  /* PAUSE and a stream TEARDOWN do not write it */
  client_setup_stream(c, AAC_STREAM, &r);
  client_request(c, "PAUSE", "rtsp://x/1", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  client_request_text(c, "TEARDOWN", "rtsp://x/1", NULL, "bplist00streams=[{type=i103}]");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_INT(calls_with("settings_persist_volume"), 0);
  /* it is written to flash once, when the session ends */
  calls_clear();
  client_request(c, "TEARDOWN", "rtsp://x/1", NULL, NULL, 0);
  CHECK_INT(calls_with("settings_persist_volume()"), 1);
  CHECK(fake.saved_volume == -20.625f);
  client_free(c);
  /* and is the next /info initialVolume and the next session's volume */
  c = client_new(192, 168, 1, 22);
  client_request(c, "GET", "/info", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && strstr(reply_text(&r), "initialVolume=-20.6250;") != NULL);
  client_pair_transient(c);
  calls_clear();
  client_setup_session(c);
  CHECK(calls_find("audio_set_volume_q15(3200)") >= 0);
  client_free(c);
}

static void test_volume_limits(void) {
  /* spec "SET_PARAMETER": above 0 as 0, below -144 as -144, from -144 up
   * to -30 gain 0 and the value kept as it came */
  static const struct {
    const char *text;
    int q15;
    const char *kept;
  } cases[] = {
      {"3.5", 32768, "volume: 0.00\r\n"},
      {"-200", 0, "volume: -144.00\r\n"},
      {"-50", 0, "volume: -50.00\r\n"},
      {"-144", 0, "volume: -144.00\r\n"},
      {"-30.0", 0, "volume: -30.00\r\n"},
  };
  client_t *c = sender_with_session(20);
  reply_t r;
  char body[64], expect[64];
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    calls_clear();
    snprintf(body, sizeof(body), "volume: %s\r\n", cases[i].text);
    client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n", body);
    CHECK(client_reply(c, &r) && r.code == 200);
    snprintf(expect, sizeof(expect), "audio_set_volume_q15(%d)", cases[i].q15);
    CHECK_STR(calls_get(0), expect);
    client_request_text(c, "GET_PARAMETER", "rtsp://x/1", NULL, "volume\r\n");
    CHECK(client_reply(c, &r));
    CHECK_STR(reply_text(&r), cases[i].kept);
  }
  CHECK(fake.ram_volume == -30.0f);
  client_free(c);
  CHECK(fake.saved_volume == -30.0f);
}

static void test_not_holder(void) {
  /* spec "Requests from a connection that does not hold the session":
   * GET_PARAMETER volume answers the current volume */
  client_t *a = sender_with_session(20);
  client_t *b = sender_with_session(30);
  reply_t r;
  client_request_text(a, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                      "volume: -13.125\r\n");
  CHECK(client_reply(a, &r) && r.code == 200);
  calls_clear();
  client_request_text(b, "SET_PARAMETER", "rtsp://x/2", "Content-Type: text/parameters\r\n",
                      "volume: -3.0\r\n");
  CHECK(client_reply(b, &r) && r.code == 200);
  CHECK_INT(calls_count(), 0);
  client_request_text(b, "GET_PARAMETER", "rtsp://x/2", NULL, "volume\r\n");
  CHECK(client_reply(b, &r) && r.code == 200);
  CHECK_STR(reply_text(&r), "volume: -13.12\r\n");
  /* a connection without any session (plain) too */
  {
    client_t *c = client_new(192, 168, 1, 40);
    client_request_text(c, "GET_PARAMETER", "rtsp://x/3", NULL, "volume\r\n");
    CHECK(client_reply(c, &r) && r.code == 200);
    CHECK_STR(reply_text(&r), "volume: -13.12\r\n");
    client_free(c);
  }
  client_free(b);
  client_free(a);
}

static void test_rate_anchor(void) {
  client_t *c = sender_with_session(20);
  reply_t r;
  int pos = 0;
  client_setup_stream(c, AAC71_STREAM, &r);
  calls_clear();
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL,
                      "bplist00rate=r1.0;rtpTime=i3600;networkTimeSecs=i10;"
                      "networkTimeFrac=i0x8000000000000000;networkTimeTimelineID=i0x9a3bc0ffee112233");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_CALL(pos, "audio_anchor(9a3bc0ffee112233,10500000000,3600)");
  CHECK_CALL(pos, "audio_set_playing(1)");
  CHECK_CALL(pos, "event(PLAYING)");
  /* rate 0: pause */
  calls_clear();
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL,
                      "bplist00rate=r0.0;rtpTime=i0;networkTimeSecs=i0;networkTimeFrac=i0;"
                      "networkTimeTimelineID=i0");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(calls_get(0), "audio_pause()");
  CHECK_STR(calls_get(1), "event(PAUSED)");
  CHECK_INT(calls_count(), 2);
  /* an integer rate plays too; only rate given with 0 */
  calls_clear();
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL,
                      "bplist00rate=i1;rtpTime=i5;networkTimeSecs=i1;networkTimeFrac=i0;"
                      "networkTimeTimelineID=i7");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(calls_get(0), "audio_anchor(7,1000000000,5)");
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL, "bplist00rate=i0");
  CHECK(client_reply(c, &r) && r.code == 200);
  /* broken */
  calls_clear();
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL, "bplist00rtpTime=i5");
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL, "bplist00rate=r1.0;rtpTime=i5");
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL, "garbage");
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request_text(c, "SETRATEANCHORTIME", "rtsp://x/1", NULL, "bplist00rate=sone");
  CHECK(client_reply(c, &r) && r.code == 400);
  CHECK_INT(calls_count(), 0);
  /* PAUSE */
  client_request(c, "PAUSE", "rtsp://x/1", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(calls_get(0), "audio_pause()");
  CHECK_STR(calls_get(1), "event(PAUSED)");
  client_free(c);
}

static void test_flush_buffered(void) {
  client_t *c = sender_with_session(20);
  reply_t r;
  client_setup_stream(c, AAC_STREAM, &r);
  calls_clear();
  client_request_text(c, "FLUSHBUFFERED", "rtsp://x/1", NULL,
                      "bplist00flushUntilSeq=i6000000;flushUntilTS=i123456789");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(calls_get(0), "audio_immediate_flush(6000000,123456789)");
  client_request_text(c, "FLUSHBUFFERED", "rtsp://x/1", NULL,
                      "bplist00flushFromSeq=i100;flushFromTS=i2000;flushUntilSeq=i200;"
                      "flushUntilTS=i4000");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(calls_get(1), "audio_deferred_flush(100,2000,200,4000,1)");
  client_request_text(c, "FLUSHBUFFERED", "rtsp://x/1", NULL,
                      "bplist00flushFromSeq=i100;flushUntilSeq=i200;flushUntilTS=i4000");
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(calls_get(2), "audio_deferred_flush(100,0,200,4000,0)");
  /* broken */
  calls_clear();
  client_request_text(c, "FLUSHBUFFERED", "rtsp://x/1", NULL, "bplist00flushUntilSeq=i5");
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request_text(c, "FLUSHBUFFERED", "rtsp://x/1", NULL, "bplist00flushUntilTS=i5");
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request(c, "FLUSHBUFFERED", "rtsp://x/1", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 400);
  client_request_text(c, "FLUSHBUFFERED", "rtsp://x/1", NULL,
                      "bplist00flushUntilSeq=sx;flushUntilTS=i5");
  CHECK(client_reply(c, &r) && r.code == 400);
  CHECK_INT(calls_count(), 0);
  client_free(c);
}

static void test_teardown_session(void) {
  client_t *c = sender_with_session(20);
  reply_t r;
  int pos = 0;
  client_setup_stream(c, AAC_STREAM, &r);
  calls_clear();
  CHECK_INT(client_request(c, "TEARDOWN", "rtsp://x/1", NULL, NULL, 0), -1);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "Connection", hdr, sizeof(hdr)), "close");
  CHECK_CALL(pos, "audio_set_playing(0)");
  CHECK_CALL(pos, "audio_stop()");
  CHECK_CALL(pos, "apple_clock_release()");
  CHECK_CALL(pos, "event_close(");
  CHECK_CALL(pos, "audio_set_stream_type(0)");
  CHECK_CALL(pos, "audio_set_encryption(0,");
  CHECK_CALL(pos, "amp_disconnected()");
  CHECK_CALL(pos, "event(DISCONNECTED)");
  CHECK_CALL(pos, "settings_persist_volume()");
  CHECK(rtsp_core_idle());
  calls_clear();
  client_free(c); /* nothing more to end */
  CHECK_INT(calls_with("amp_disconnected"), 0);
}

static void test_connection_close_ends_session(void) {
  client_t *c = sender_with_session(20);
  reply_t r;
  client_setup_stream(c, RT_STREAM, &r);
  calls_clear();
  client_free(c);
  CHECK(calls_find("apple_clock_release()") >= 0);
  CHECK(calls_find("amp_disconnected()") >= 0);
  CHECK(calls_find("event(DISCONNECTED)") >= 0);
  CHECK(calls_find("settings_persist_volume()") >= 0);
  CHECK(rtsp_core_idle());
  /* a connection without a session closes quietly */
  c = client_new(192, 168, 1, 9);
  calls_clear();
  client_free(c);
  CHECK_INT(calls_count(), 0);
}

static void test_second_sender(void) {
  client_t *a = sender_with_session(20);
  client_t *b;
  reply_t r;
  int pos = 0;
  int port_b;
  client_setup_stream(a, AAC_STREAM, &r);

  b = client_new(192, 168, 1, 30);
  client_request(b, "GET", "/info", NULL, NULL, 0);
  CHECK(client_reply(b, &r) && r.code == 200);
  client_pair_transient(b);
  calls_clear();
  /* its session SETUP: its own event port, listening at once, before the
   * reply (spec "A second sender") */
  port_b = client_setup_session(b);
  CHECK_INT(port_b, 50001);
  CHECK_INT(calls_with("event_open("), 1);
  {
    uint16_t listening = 0;
    CHECK(fake_event_port(b->conn, NULL, NULL, &listening));
    CHECK_INT(listening, 50001);
  }
  CHECK(fake_event_port(a->conn, NULL, NULL, NULL)); /* the first's too */
  CHECK_INT(calls_with("apple_clock_select"), 0);
  /* its session SETUP again: the same port, which keeps listening; nothing
   * new opens or starts (spec "A sender sending it again") */
  CHECK_INT(client_setup_session(b), 50001);
  CHECK_INT(calls_with("event_open("), 1);
  CHECK_INT(calls_with("event_close("), 0);
  CHECK_INT(calls_with("apple_clock_select"), 0);
  CHECK(fake_event_port(b->conn, NULL, NULL, NULL));
  CHECK_INT(calls_with("amp_connected"), 0);
  /* its RECORD, SET_PARAMETER, SETRATEANCHORTIME, FLUSHBUFFERED, PAUSE,
   * /feedback: nothing changes */
  calls_clear();
  client_request(b, "RECORD", "rtsp://x/2", NULL, NULL, 0);
  CHECK(client_reply(b, &r) && r.code == 200);
  client_request_text(b, "SET_PARAMETER", "rtsp://x/2", "Content-Type: text/parameters\r\n",
                      "volume: -3.0\r\n");
  CHECK(client_reply(b, &r) && r.code == 200);
  client_request_text(b, "SETRATEANCHORTIME", "rtsp://x/2", NULL, "bplist00rate=r1.0");
  CHECK(client_reply(b, &r) && r.code == 200);
  client_request_text(b, "FLUSHBUFFERED", "rtsp://x/2", NULL, "bplist00flushUntilSeq=i1;flushUntilTS=i1");
  CHECK(client_reply(b, &r) && r.code == 200);
  client_request(b, "PAUSE", "rtsp://x/2", NULL, NULL, 0);
  CHECK(client_reply(b, &r) && r.code == 200);
  client_request(b, "POST", "/feedback", NULL, NULL, 0);
  CHECK(client_reply(b, &r) && r.code == 200 && r.body_len == 0);
  client_request_text(b, "TEARDOWN", "rtsp://x/2", NULL, "bplist00streams=[{type=i103}]");
  CHECK(client_reply(b, &r) && r.code == 200);
  CHECK_INT(calls_count(), 0);
  /* the first still plays */
  client_request(a, "POST", "/feedback", NULL, NULL, 0);
  CHECK(client_reply(a, &r) && r.body_len > 0);

  /* its first stream SETUP takes the session */
  calls_clear();
  CHECK_INT(client_setup_stream(b, AAC71_STREAM, &r), 200);
  CHECK_CALL(pos, "audio_set_playing(0)");
  CHECK_CALL(pos, "audio_stop()");
  CHECK_CALL(pos, "apple_clock_release()");
  CHECK_CALL(pos, "event_close(");
  CHECK_CALL(pos, "amp_disconnected()");
  CHECK_CALL(pos, "event(DISCONNECTED)");
  CHECK_CALL(pos, "settings_persist_volume()");
  CHECK_CALL(pos, "conn_close(");
  CHECK_CALL(pos, "apple_clock_select(192.168.1.30,0)");
  CHECK_CALL(pos, "audio_set_volume_q15(8192)");
  CHECK_CALL(pos, "amp_connected()");
  CHECK_CALL(pos, "event(CLIENT_CONNECTED)");
  CHECK_CALL(pos, "audio_set_stream_type(103)");
  CHECK_CALL(pos, "audio_set_format(AAC,48000,2,24,1024)");
  CHECK_CALL(pos, "audio_start_buffered(0)");
  CHECK_INT(calls_with("amp_connected"), 1);
  CHECK_INT(calls_with("amp_disconnected"), 1);
  /* its event port was already open and stays; the first's is closed */
  CHECK_INT(calls_with("event_open("), 0);
  CHECK(fake_event_port(b->conn, NULL, NULL, NULL));
  CHECK(!fake_event_port(a->conn, NULL, NULL, NULL));
  /* the first connection is closed: anything it sends ends it */
  CHECK_INT(client_request(a, "OPTIONS", "*", NULL, NULL, 0), -1);
  calls_clear();
  client_free(a);
  CHECK_INT(calls_with("amp_disconnected"), 0); /* the session is b's */
  client_request(b, "POST", "/feedback", NULL, NULL, 0);
  CHECK(client_reply(b, &r));
  CHECK_STR(reply_text(&r), "FEEDBACK type=103 sr=48000.0");
  client_free(b);
  CHECK(rtsp_core_idle());
}

static void test_second_sender_teardown(void) {
  client_t *a = sender_with_session(20);
  client_t *b = sender_with_session(30);
  reply_t r;
  CHECK(fake_event_port(b->conn, NULL, NULL, NULL));
  calls_clear();
  /* an ignored TEARDOWN: 200, nothing changes, its connection closes */
  CHECK_INT(client_request(b, "TEARDOWN", "rtsp://x/2", NULL, NULL, 0), -1);
  CHECK(client_reply(b, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "Connection", hdr, sizeof(hdr)), "close");
  CHECK_INT(calls_with("amp_disconnected"), 0);
  client_free(b);
  CHECK_INT(calls_with("amp_disconnected"), 0);
  CHECK(calls_find("event_close(") >= 0); /* its event port closes with it */
  CHECK(fake_event_port(a->conn, NULL, NULL, NULL));
  CHECK(!rtsp_core_idle());
  client_free(a);
  CHECK(rtsp_core_idle());
}

static void test_second_sender_after_end(void) {
  /* the first ends; the second's stream SETUP starts its session */
  client_t *a = sender_with_session(20);
  client_t *b = sender_with_session(30);
  reply_t r;
  client_request(a, "TEARDOWN", "rtsp://x/1", NULL, NULL, 0);
  client_free(a);
  calls_clear();
  CHECK_INT(client_setup_stream(b, AAC_STREAM, &r), 200);
  CHECK_INT(calls_with("conn_close"), 0);
  CHECK_INT(calls_with("amp_connected"), 1);
  CHECK_INT(calls_with("event_open("), 0); /* listening since its session SETUP */
  CHECK(fake_event_port(b->conn, NULL, NULL, NULL));
  client_free(b);
}

static void test_second_sender_event_port_fails(void) {
  /* the second's event port does not open: 500, the first keeps its
   * session and port */
  client_t *a = sender_with_session(20);
  client_t *b = client_new(192, 168, 1, 30);
  reply_t r;
  client_pair_transient(b);
  calls_clear();
  fake.event_open_err = ESP_FAIL;
  client_request_text(b, "SETUP", "rtsp://x/2", NULL, "bplist00timingProtocol=sPTP");
  CHECK(client_reply(b, &r) && r.code == 500);
  fake.event_open_err = ESP_OK;
  CHECK_INT(calls_with("event_close("), 0);
  CHECK_INT(calls_with("amp_disconnected"), 0);
  CHECK(fake_event_port(a->conn, NULL, NULL, NULL));
  CHECK(!fake_event_port(b->conn, NULL, NULL, NULL));
  /* no session SETUP that worked: a stream SETUP is 455 */
  CHECK_INT(client_setup_stream(b, AAC_STREAM, &r), 455);
  /* a new try opens it */
  CHECK_INT(client_setup_session(b), 50001);
  CHECK(fake_event_port(b->conn, NULL, NULL, NULL));
  client_free(b);
  client_free(a);
  CHECK(rtsp_core_idle());
}

static void test_split_bytes(void) {
  /* a whole encrypted session SETUP fed one byte at a time */
  client_t *c = client_new(192, 168, 1, 20);
  const char body[] = "bplist00timingProtocol=sPTP";
  char req[256];
  uint8_t frames[512];
  size_t n, fn = 0;
  reply_t r;
  client_pair_transient(c);
  n = (size_t)snprintf(req, sizeof(req), "SETUP rtsp://x/1 RTSP/1.0\r\nCSeq: 9\r\nContent-Length: %zu\r\n\r\n%s",
                       sizeof(body) - 1, body);
  for (size_t off = 0; off < n; off += 40) {
    size_t k = n - off < 40 ? n - off : 40;
    fn += rtsp_frame_seal(&c->send_cipher, (const uint8_t *)req + off, k, frames + fn);
  }
  for (size_t i = 0; i < fn; i++) CHECK_INT(client_feed(c, frames + i, 1), 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_STR(reply_header(&r, "CSeq", hdr, sizeof(hdr)), "9");
  CHECK_STR(reply_text(&r), "SESSION eventPort=50000 timingPort=0");
  CHECK(!rtsp_core_idle());
  client_free(c);
  CHECK(rtsp_core_idle());
}

static void test_send_failure(void) {
  client_t *c = sender_with_session(20);
  c->fail_send = true;
  CHECK_INT(client_request(c, "OPTIONS", "*", NULL, NULL, 0), -1);
  client_free(c);
  CHECK(rtsp_core_idle());
}

static void test_garbage_streams(void) {
  /* random bytes, plain and encrypted, never crash and end in a close or
   * a reply */
  srand(1234);
  for (int round = 0; round < 200; round++) {
    client_t *c = client_new(10, 0, 0, (uint8_t)round);
    uint8_t junk[300];
    size_t n = (size_t)(rand() % (int)sizeof(junk));
    for (size_t i = 0; i < n; i++) junk[i] = (uint8_t)rand();
    if (round % 2) client_pair_transient(c);
    if (round % 3 == 0 && n > 20) memcpy(junk, "SETUP * RTSP/1.0\r\n", 18);
    client_raw(c, junk, n);
    client_free(c);
  }
  CHECK(rtsp_core_idle());
}

void run_core_tests(void) {
  RUN(test_options);
  RUN(test_unknown_method);
  RUN(test_info);
  RUN(test_info_without_saved_volume);
  RUN(test_method_case);
  RUN(test_cseq_and_broken_requests);
  RUN(test_pipelined_requests);
  RUN(test_pairing_transient);
  RUN(test_pairing_verify_and_bad_frame);
  RUN(test_encryption_starts_mid_buffer);
  RUN(test_channel_counters);
  RUN(test_pairing_failures);
  RUN(test_fp_setup);
  RUN(test_session_setup);
  RUN(test_setup_refused);
  RUN(test_stream_realtime);
  RUN(test_stream_buffered);
  RUN(test_stream_formats);
  RUN(test_stream_start_failures);
  RUN(test_volume);
  RUN(test_volume_limits);
  RUN(test_not_holder);
  RUN(test_rate_anchor);
  RUN(test_flush_buffered);
  RUN(test_teardown_session);
  RUN(test_connection_close_ends_session);
  RUN(test_second_sender);
  RUN(test_second_sender_teardown);
  RUN(test_second_sender_after_end);
  RUN(test_second_sender_event_port_fails);
  RUN(test_split_bytes);
  RUN(test_send_failure);
  RUN(test_garbage_streams);
}
