/* SPDX-License-Identifier: Apache-2.0 */
/* Remote commands on the event channel (spec "Event channel"). */
#include <stdlib.h>

#include "rtsp_remote.h"
#include "test.h"

/* The sender's side of the event connection. */
typedef struct {
  rtsp_evconn_t *ev;
  rtsp_cipher_t to_gateway;           /* Events-Read key */
  rtsp_frame_reader_t from_gateway;   /* Events-Write key */
  uint8_t out[8192];
  size_t out_len;
  char plain[8192];
  size_t plain_len;
  bool fail_send;
  int frames; /* frames the Gateway sent */
} evsender_t;

static int ev_sink(void *ctx, const uint8_t *data, size_t len) {
  evsender_t *s = ctx;
  if (s->fail_send) return -1;
  memcpy(s->out + s->out_len, data, len);
  s->out_len += len;
  return 0;
}

/* Connects to the event port of control's session SETUP. */
static void ev_open(evsender_t *s, client_t *control) {
  uint8_t write_key[32], read_key[32];
  memset(s, 0, sizeof(*s));
  /* the keys rtsp_port_event_open got from the session SETUP */
  CHECK(fake_event_port(control->conn, write_key, read_key, NULL)); /* it listens */
  rtsp_cipher_init(&s->to_gateway, read_key);
  rtsp_frame_reader_init(&s->from_gateway, write_key);
  s->ev = rtsp_evconn_new(control->conn, write_key, read_key);
}

/* Polls once and opens what was sent. */
static int ev_poll(evsender_t *s) {
  int rc = rtsp_evconn_poll(s->ev, fake.now_ms, ev_sink, s);
  size_t off = 0;
  while (off < s->out_len) {
    size_t used, n;
    const uint8_t *p;
    rtsp_frame_result_t r =
        rtsp_frame_feed(&s->from_gateway, s->out + off, s->out_len - off, &used, &p, &n);
    off += used;
    if (r != RTSP_FRAME_DONE) break;
    s->frames++;
    memcpy(s->plain + s->plain_len, p, n);
    s->plain_len += n;
  }
  s->out_len = 0;
  s->plain[s->plain_len] = 0;
  return rc;
}

static int ev_reply(evsender_t *s, const char *text) {
  uint8_t frame[1100];
  size_t n = rtsp_frame_seal(&s->to_gateway, (const uint8_t *)text, strlen(text), frame);
  return rtsp_evconn_receive(s->ev, frame, n);
}

static void status(rtsp_remote_status_t *st) { rtsp_remote_get_status(st); }

static client_t *session(void) {
  client_t *c = client_new(192, 168, 1, 40);
  client_pair_transient(c);
  client_setup_session(c);
  return c;
}

static void test_remote_without_connection(void) {
  uint32_t id = 99;
  rtsp_remote_status_t st;
  CHECK_INT(rtsp_remote_enqueue(0, &id), ESP_ERR_NOT_FOUND);
  CHECK_INT(id, 99);
  CHECK_INT(rtsp_remote_enqueue(8, &id), ESP_ERR_INVALID_ARG);
  CHECK_INT(rtsp_remote_enqueue(255, &id), ESP_ERR_INVALID_ARG);
  status(&st);
  CHECK(!st.connected);
  CHECK_INT(st.id, 0);
  CHECK_INT(st.result, RTSP_REMOTE_IDLE);
  CHECK_INT(st.response_code, 0);
  rtsp_remote_get_status(NULL);
}

static void test_remote_names(void) {
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_IDLE), "idle");
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_QUEUED), "queued");
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_SENT), "sent");
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_ACKNOWLEDGED), "acknowledged");
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_REJECTED), "rejected");
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_TIMEOUT), "timeout");
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_FAILED), "failed");
  CHECK_STR(rtsp_remote_result_name(RTSP_REMOTE_DISCONNECTED), "disconnected");
  CHECK_STR(rtsp_remote_result_name((rtsp_remote_result_t)42), "unknown");
}

static void test_remote_play_acknowledged(void) {
  client_t *c = session();
  evsender_t s;
  rtsp_remote_status_t st;
  uint32_t id = 0;
  ev_open(&s, c);
  status(&st);
  CHECK(st.connected);
  CHECK_INT(rtsp_remote_enqueue(0, &id), ESP_OK);
  CHECK_INT(id, 1);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_QUEUED);
  CHECK_INT(st.command, 0);
  /* one at a time */
  CHECK_INT(rtsp_remote_enqueue(1, NULL), ESP_ERR_INVALID_STATE);
  CHECK_INT(ev_poll(&s), 0);
  CHECK_STR(s.plain,
            "POST /command RTSP/1.0\r\nCSeq: 1\r\n"
            "Content-Type: application/x-apple-binary-plist\r\nContent-Length: 5\r\n\r\nCMD 0");
  CHECK_INT(s.frames, 1); /* head and body in one frame (spec "Encryption") */
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_SENT);
  CHECK_INT(rtsp_remote_enqueue(1, NULL), ESP_ERR_INVALID_STATE);
  /* sent once only */
  s.plain_len = 0;
  CHECK_INT(ev_poll(&s), 0);
  CHECK_INT(s.plain_len, 0);
  /* a reply with another CSeq is not ours */
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 7\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_SENT);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 1\r\nServer: AirTunes/770.8.1\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_ACKNOWLEDGED);
  CHECK_INT(st.response_code, 200);
  CHECK_INT(st.id, 1);
  /* every command number 0 to 5 */
  for (uint8_t cmd = 1; cmd <= 5; cmd++) {
    char expect[64], reply[64];
    s.plain_len = 0;
    CHECK_INT(rtsp_remote_enqueue(cmd, &id), ESP_OK);
    CHECK_INT(id, 1 + cmd);
    ev_poll(&s);
    snprintf(expect, sizeof(expect), "CSeq: %u\r\n", 1 + cmd);
    CHECK(strstr(s.plain, expect) != NULL);
    snprintf(expect, sizeof(expect), "\r\n\r\nCMD %u", cmd);
    CHECK(strstr(s.plain, expect) != NULL);
    snprintf(reply, sizeof(reply), "RTSP/1.0 200 OK\r\nCSeq: %u\r\n\r\n", 1 + cmd);
    ev_reply(&s, reply);
  }
  rtsp_evconn_free(s.ev);
  client_free(c);
}

static void test_remote_rejected(void) {
  client_t *c = session();
  evsender_t s;
  rtsp_remote_status_t st;
  ev_open(&s, c);
  rtsp_remote_enqueue(3, NULL);
  ev_poll(&s);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 501 Not Implemented\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_REJECTED);
  CHECK_INT(st.response_code, 501);
  rtsp_evconn_free(s.ev);
  client_free(c);
}

static void test_remote_volume(void) {
  client_t *c = session(); /* saved volume -15.0, i.e. 0.5 */
  evsender_t s;
  rtsp_remote_status_t st;
  reply_t r;
  ev_open(&s, c);
  CHECK_INT(rtsp_remote_enqueue(RTSP_REMOTE_VOLUME_UP, NULL), ESP_OK);
  calls_clear();
  ev_poll(&s);
  CHECK(strstr(s.plain, "\r\n\r\nVOL 0.5625") != NULL);
  CHECK_INT(s.frames, 1);
  CHECK_INT(calls_count(), 0); /* not before the reply */
  ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n");
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_ACKNOWLEDGED);
  CHECK_STR(calls_get(0), "audio_set_volume_q15(10368)"); /* -13.125 */
  CHECK_STR(calls_get(1), "settings_set_volume(-13.1250)"); /* a change like SET_PARAMETER */
  CHECK_INT(calls_with("settings_persist_volume"), 0);
  client_request_text(c, "GET_PARAMETER", "rtsp://x/1", NULL, "volume\r\n");
  CHECK(client_reply(c, &r));
  CHECK_STR(reply_text(&r), "volume: -13.12\r\n");
  /* down from there: 0.5 */
  s.plain_len = 0;
  rtsp_remote_enqueue(RTSP_REMOTE_VOLUME_DOWN, NULL);
  ev_poll(&s);
  CHECK(strstr(s.plain, "\r\n\r\nVOL 0.5000") != NULL);
  /* rejected: the volume stays */
  calls_clear();
  ev_reply(&s, "RTSP/1.0 400 Bad Request\r\nCSeq: 2\r\n\r\n");
  CHECK_INT(calls_count(), 0);
  /* at the top, up stays 1 */
  client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                      "volume: 0.0\r\n");
  client_reply(c, &r);
  s.plain_len = 0;
  rtsp_remote_enqueue(RTSP_REMOTE_VOLUME_UP, NULL);
  ev_poll(&s);
  CHECK(strstr(s.plain, "VOL 1.0000") != NULL);
  ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 3\r\n\r\n");
  /* muted, down stays 0 */
  client_request_text(c, "SET_PARAMETER", "rtsp://x/1", "Content-Type: text/parameters\r\n",
                      "volume: -144.0\r\n");
  client_reply(c, &r);
  s.plain_len = 0;
  rtsp_remote_enqueue(RTSP_REMOTE_VOLUME_DOWN, NULL);
  ev_poll(&s);
  CHECK(strstr(s.plain, "VOL 0.0000") != NULL);
  calls_clear();
  ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 4\r\n\r\n");
  CHECK_STR(calls_get(0), "audio_set_volume_q15(0)");
  rtsp_evconn_free(s.ev);
  client_free(c);
}

/* spec "Event channel": an acknowledged volume is kept like a SET_PARAMETER
 * volume, written to flash once when the session ends, and is the next
 * initialVolume. */
static void test_remote_volume_kept(void) {
  client_t *c = session(); /* saved volume -15.0 */
  evsender_t s;
  reply_t r;
  ev_open(&s, c);
  rtsp_remote_enqueue(RTSP_REMOTE_VOLUME_UP, NULL);
  ev_poll(&s);
  ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n");
  CHECK(fake.saved_volume == -15.0f); /* not in flash yet */
  calls_clear();
  client_request(c, "TEARDOWN", "rtsp://x/1", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK_INT(calls_with("settings_persist_volume()"), 1);
  CHECK(fake.saved_volume == -13.125f);
  rtsp_evconn_free(s.ev);
  client_free(c);
  c = client_new(192, 168, 1, 41);
  client_request(c, "GET", "/info", NULL, NULL, 0);
  CHECK(client_reply(c, &r) && r.code == 200);
  CHECK(strstr(reply_text(&r), "initialVolume=-13.1250;") != NULL);
  client_free(c);
}

static void test_remote_timeout(void) {
  client_t *c = session();
  evsender_t s;
  rtsp_remote_status_t st;
  fake.now_ms = 0xFFFFF000u; /* across the wrap of the clock */
  ev_open(&s, c);
  rtsp_remote_enqueue(1, NULL);
  CHECK_INT(ev_poll(&s), 0);
  fake.now_ms += 2999;
  CHECK_INT(ev_poll(&s), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_SENT);
  fake.now_ms += 1;
  CHECK_INT(ev_poll(&s), 0); /* the event connection stays open */
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_TIMEOUT);
  CHECK_INT(st.response_code, 0);
  CHECK(st.connected);
  /* a late reply changes nothing */
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_TIMEOUT);
  /* the next command goes on the same connection */
  s.plain_len = 0;
  CHECK_INT(rtsp_remote_enqueue(2, NULL), ESP_OK);
  CHECK_INT(ev_poll(&s), 0);
  CHECK(strstr(s.plain, "CSeq: 2\r\n") != NULL);
  CHECK(strstr(s.plain, "CMD 2") != NULL);
  /* a late reply to the timed-out one is not the current CSeq */
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_SENT);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_ACKNOWLEDGED);
  /* a second timeout: still open */
  rtsp_remote_enqueue(3, NULL);
  CHECK_INT(ev_poll(&s), 0);
  fake.now_ms += 3000;
  CHECK_INT(ev_poll(&s), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_TIMEOUT);
  CHECK(st.connected);
  rtsp_evconn_free(s.ev);
  status(&st);
  CHECK(!st.connected);
  CHECK_INT(st.result, RTSP_REMOTE_TIMEOUT);
  CHECK_INT(rtsp_remote_enqueue(1, NULL), ESP_ERR_NOT_FOUND);
  client_free(c);
}

static void test_remote_failures(void) {
  client_t *c = session();
  evsender_t s;
  rtsp_remote_status_t st;
  uint32_t id;
  /* a send error: failed, the connection closes */
  ev_open(&s, c);
  rtsp_remote_enqueue(2, &id);
  s.fail_send = true;
  CHECK_INT(ev_poll(&s), -1);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_FAILED);
  rtsp_evconn_free(s.ev);
  /* the event connection closes first: disconnected */
  ev_open(&s, c);
  rtsp_remote_enqueue(2, &id);
  CHECK_INT(id, 2); /* ids go on */
  rtsp_evconn_free(s.ev);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_DISCONNECTED);
  /* the session ends first: disconnected, the event connection is done */
  ev_open(&s, c);
  rtsp_remote_enqueue(4, &id);
  ev_poll(&s);
  client_request(c, "TEARDOWN", "rtsp://x/1", NULL, NULL, 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_DISCONNECTED);
  CHECK(!st.connected);
  CHECK_INT(ev_poll(&s), -1);
  rtsp_evconn_free(s.ev);
  client_free(c);
  /* ids count on across sessions */
  c = session();
  ev_open(&s, c);
  CHECK_INT(rtsp_remote_enqueue(0, &id), ESP_OK);
  CHECK_INT(id, 4);
  rtsp_evconn_free(s.ev);
  client_free(c);
}

static void test_remote_corrupt_input(void) {
  client_t *c = session();
  evsender_t s;
  rtsp_remote_status_t st;
  ev_open(&s, c);
  rtsp_remote_enqueue(0, NULL);
  ev_poll(&s);
  /* broken replies are ignored or close the connection, never acknowledge */
  CHECK_INT(ev_reply(&s, "RTSP/1.0 2000 OK\r\nCSeq: 1\r\n\r\n"), 0);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 20x OK\r\nCSeq: 1\r\n\r\n"), 0);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 1x\r\n\r\n"), 0);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq: 99999999999\r\n\r\n"), 0);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\n\r\n"), 0);
  CHECK_INT(ev_reply(&s, "POST /command RTSP/1.0\r\nCSeq: 1\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_SENT);
  CHECK_INT(ev_reply(&s, "RTSP/1.0 200 OK\r\nCSeq 1\r\n\r\n"), -1);
  rtsp_evconn_free(s.ev);
  /* a frame that does not open */
  ev_open(&s, c);
  {
    uint8_t frame[64];
    size_t n = rtsp_frame_seal(&s.to_gateway, (const uint8_t *)"RTSP/1.0", 8, frame);
    frame[3] ^= 1;
    CHECK_INT(rtsp_evconn_receive(s.ev, frame, n), -1);
  }
  rtsp_evconn_free(s.ev);
  /* a reply split over frames and bytes */
  ev_open(&s, c);
  rtsp_remote_enqueue(5, NULL);
  ev_poll(&s);
  {
    const char *text = "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 3\r\n\r\nabc";
    uint8_t frames[400];
    size_t n = 0;
    for (size_t off = 0; off < strlen(text); off += 7) {
      size_t k = strlen(text) - off < 7 ? strlen(text) - off : 7;
      n += rtsp_frame_seal(&s.to_gateway, (const uint8_t *)text + off, k, frames + n);
    }
    for (size_t i = 0; i < n; i++) CHECK_INT(rtsp_evconn_receive(s.ev, frames + i, 1), 0);
  }
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_ACKNOWLEDGED);
  rtsp_evconn_free(s.ev);
  client_free(c);
}

static void test_remote_replaced_connection(void) {
  client_t *c = session();
  evsender_t a, b;
  rtsp_remote_status_t st;
  ev_open(&a, c);
  rtsp_remote_enqueue(0, NULL);
  ev_poll(&a);
  ev_open(&b, c); /* a new event connection replaces the old */
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_DISCONNECTED);
  CHECK(st.connected);
  CHECK_INT(ev_poll(&a), -1); /* the old one closes */
  rtsp_evconn_free(a.ev);
  status(&st);
  CHECK(st.connected);
  CHECK_INT(rtsp_remote_enqueue(1, NULL), ESP_OK);
  CHECK_INT(ev_poll(&b), 0);
  CHECK(strstr(b.plain, "CMD 1") != NULL);
  rtsp_evconn_free(b.ev);
  client_free(c);
}

#define TAKE_OVER_STREAM "type=i103,audioFormat=i8388608,shk=d32,spf=i1024,ct=i8,sr=i48000"

/* A second sender: session SETUP while another holds the session. */
static client_t *second_sender(void) {
  client_t *c = client_new(192, 168, 1, 41);
  client_pair_transient(c);
  CHECK(client_setup_session(c) > 0);
  return c;
}

/* spec "A second sender", "Event channel": the second sender's event port
 * listens at once and takes a connection; commands still go only to the
 * holder's event connection. */
static void test_remote_second_sender(void) {
  client_t *a = session();
  client_t *b = second_sender();
  evsender_t ea, eb;
  rtsp_remote_status_t st;
  uint32_t id = 0;

  /* only the second has an event connection: not the holder's */
  ev_open(&eb, b);
  CHECK(eb.ev != NULL);
  status(&st);
  CHECK(!st.connected);
  CHECK_INT(rtsp_remote_enqueue(0, NULL), ESP_ERR_NOT_FOUND);
  CHECK_INT(ev_poll(&eb), 0); /* it stays open, nothing is sent on it */
  CHECK_INT(eb.plain_len, 0);

  /* the holder connects: commands go there, not to the second */
  ev_open(&ea, a);
  status(&st);
  CHECK(st.connected);
  CHECK_INT(rtsp_remote_enqueue(1, &id), ESP_OK);
  CHECK_INT(ev_poll(&eb), 0);
  CHECK_INT(eb.plain_len, 0);
  CHECK_INT(ev_poll(&ea), 0);
  CHECK(strstr(ea.plain, "CMD 1") != NULL);
  /* a reply on the second's channel does not count */
  CHECK_INT(ev_reply(&eb, "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_SENT);
  /* a new connection to the second's port replaces only that one */
  {
    evsender_t eb2;
    ev_open(&eb2, b);
    CHECK_INT(ev_poll(&eb), -1);
    rtsp_evconn_free(eb.ev);
    status(&st);
    CHECK_INT(st.result, RTSP_REMOTE_SENT); /* the holder's command goes on */
    eb = eb2;
  }
  CHECK_INT(ev_reply(&ea, "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_ACKNOWLEDGED);

  /* the second sender's connection closes: its event connection is done,
   * the holder's stays */
  client_free(b);
  CHECK_INT(ev_poll(&eb), -1);
  rtsp_evconn_free(eb.ev);
  status(&st);
  CHECK(st.connected);
  CHECK_INT(rtsp_remote_enqueue(2, NULL), ESP_OK);
  CHECK_INT(ev_poll(&ea), 0);
  CHECK(strstr(ea.plain, "CMD 2") != NULL);
  rtsp_evconn_free(ea.ev);
  client_free(a);
}

/* After a take-over the commands go to the new holder's event connection,
 * which is already open; one still going on the old holder's ends
 * disconnected. */
static void test_remote_take_over(void) {
  client_t *a = session();
  client_t *b = second_sender();
  evsender_t ea, eb;
  rtsp_remote_status_t st;
  reply_t r;
  ev_open(&ea, a);
  ev_open(&eb, b); /* right after its SETUP reply, as an iPhone does */
  CHECK_INT(rtsp_remote_enqueue(0, NULL), ESP_OK);
  CHECK_INT(ev_poll(&ea), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_SENT);

  CHECK_INT(client_setup_stream(b, TAKE_OVER_STREAM, &r), 200);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_DISCONNECTED);
  CHECK(st.connected); /* b's, without a new connection */
  CHECK_INT(ev_poll(&ea), -1); /* the old holder's port is closed */
  rtsp_evconn_free(ea.ev);
  CHECK_INT(rtsp_remote_enqueue(4, NULL), ESP_OK);
  CHECK_INT(ev_poll(&eb), 0);
  CHECK(strstr(eb.plain, "CMD 4") != NULL);
  CHECK_INT(ev_reply(&eb, "RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\n"), 0);
  status(&st);
  CHECK_INT(st.result, RTSP_REMOTE_ACKNOWLEDGED);
  client_free(a);
  status(&st);
  CHECK(st.connected);
  rtsp_evconn_free(eb.ev);
  client_free(b);
  status(&st);
  CHECK(!st.connected);
}

void run_remote_tests(void) {
  RUN(test_remote_without_connection);
  RUN(test_remote_names);
  RUN(test_remote_play_acknowledged);
  RUN(test_remote_rejected);
  RUN(test_remote_volume);
  RUN(test_remote_volume_kept);
  RUN(test_remote_timeout);
  RUN(test_remote_failures);
  RUN(test_remote_corrupt_input);
  RUN(test_remote_replaced_connection);
  RUN(test_remote_second_sender);
  RUN(test_remote_take_over);
}
