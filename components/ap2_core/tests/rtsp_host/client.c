/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ap2_hap.h"
#include "ap2_tlv8.h"
#include "test.h"

int t_checks;
int t_failures;

void run_test(const char *name, void (*fn)(void)) {
  int before = t_failures;
  fake_reset();
  rtsp_core_test_reset();
  fn();
  if (fake.lock_depth != 0) {
    t_failures++;
    fprintf(stderr, "%s: lock not released (%d)\n", name, fake.lock_depth);
  }
  printf("%-48s %s\n", name, t_failures == before ? "ok" : "FAILED");
}

static int sink(void *ctx, const uint8_t *data, size_t len) {
  client_t *c = ctx;
  if (c->fail_send) return -1;
  if (c->out_len + len > sizeof(c->out)) return -1;
  memcpy(c->out + c->out_len, data, len);
  c->out_len += len;
  return 0;
}

client_t *client_new(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  client_t *client = calloc(1, sizeof(*client));
  uint8_t *ip = (uint8_t *)&client->ip;
  ip[0] = a; ip[1] = b; ip[2] = c; ip[3] = d;
  client->conn = rtsp_conn_new(client->ip, client);
  return client;
}

void client_free(client_t *client) {
  if (client->conn != NULL) rtsp_conn_end(client->conn);
  free(client);
}

/* Moves what the server sent into plain text, opening frames when the
 * channel is encrypted. */
static void take_output(client_t *c) {
  size_t off = 0;
  if (!c->encrypted) {
    memcpy(c->plain + c->plain_len, c->out, c->out_len);
    c->plain_len += c->out_len;
    c->out_len = 0;
    return;
  }
  while (off < c->out_len) {
    size_t used;
    const uint8_t *p;
    size_t n;
    rtsp_frame_result_t r =
        rtsp_frame_feed(&c->recv_frames, c->out + off, c->out_len - off, &used, &p, &n);
    off += used;
    if (r == RTSP_FRAME_ERROR) {
      fprintf(stderr, "test client: reply frame does not open\n");
      break;
    }
    if (r == RTSP_FRAME_MORE) break;
    if (c->frames_in < 16) c->frame_lens[c->frames_in] = n;
    c->frames_in++;
    memcpy(c->plain + c->plain_len, p, n);
    c->plain_len += n;
  }
  c->out_len = 0;
}

int client_feed(client_t *c, const void *data, size_t len) {
  c->rc = rtsp_conn_receive(c->conn, data, len, sink, c);
  take_output(c);
  return c->rc;
}

int client_raw(client_t *c, const void *data, size_t len) {
  if (!c->encrypted) {
    c->rc = rtsp_conn_receive(c->conn, data, len, sink, c);
  } else {
    uint8_t *frames = malloc(len + (len / RTSP_FRAME_MAX + 1) * RTSP_FRAME_OVERHEAD);
    size_t n = 0;
    const uint8_t *p = data;
    size_t left = len;
    while (left > 0) {
      size_t k = left < RTSP_FRAME_MAX ? left : RTSP_FRAME_MAX;
      n += rtsp_frame_seal(&c->send_cipher, p, k, frames + n);
      p += k;
      left -= k;
    }
    c->rc = rtsp_conn_receive(c->conn, frames, n, sink, c);
    free(frames);
  }
  take_output(c);
  return c->rc;
}

int client_request(client_t *c, const char *method, const char *uri,
                   const char *headers, const void *body, size_t body_len) {
  char head[1024];
  uint8_t *msg;
  int n, rc;
  n = snprintf(head, sizeof(head), "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s", method, uri,
               ++c->cseq, headers != NULL ? headers : "");
  if (body_len > 0) {
    n += snprintf(head + n, sizeof(head) - (size_t)n, "Content-Length: %zu\r\n", body_len);
  }
  n += snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");
  msg = malloc((size_t)n + body_len);
  memcpy(msg, head, (size_t)n);
  if (body_len > 0) memcpy(msg + n, body, body_len);
  rc = client_raw(c, msg, (size_t)n + body_len);
  free(msg);
  return rc;
}

int client_request_text(client_t *c, const char *method, const char *uri,
                        const char *headers, const char *body) {
  return client_request(c, method, uri, headers, body, body ? strlen(body) : 0);
}

const char *reply_header(const reply_t *r, const char *name, char *buf, size_t cap) {
  const char *p = r->head;
  size_t n = strlen(name);
  while ((p = strstr(p, "\r\n")) != NULL) {
    p += 2;
    if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
      const char *v = p + n + 1;
      const char *e = strstr(v, "\r\n");
      size_t len;
      while (*v == ' ') v++;
      len = e ? (size_t)(e - v) : strlen(v);
      if (len + 1 > cap) return NULL;
      memcpy(buf, v, len);
      buf[len] = '\0';
      return buf;
    }
  }
  return NULL;
}

bool client_reply(client_t *c, reply_t *r) {
  char *end;
  size_t head_len;
  char len_text[32];
  size_t body_len = 0;
  memset(r, 0, sizeof(*r));
  if (c->plain_len == 0) return false;
  end = memmem(c->plain, c->plain_len, "\r\n\r\n", 4);
  if (end == NULL) return false;
  head_len = (size_t)(end - (char *)c->plain) + 4;
  if (head_len >= sizeof(r->head)) return false;
  memcpy(r->head, c->plain, head_len);
  r->head[head_len] = '\0';
  if (sscanf(r->head, "RTSP/1.0 %d", &r->code) != 1) return false;
  if (reply_header(r, "Content-Length", len_text, sizeof(len_text)) != NULL) {
    body_len = (size_t)atoi(len_text);
  }
  if (head_len + body_len > c->plain_len || body_len >= sizeof(r->body)) return false;
  memcpy(r->body, c->plain + head_len, body_len);
  r->body_len = body_len;
  memmove(c->plain, c->plain + head_len + body_len, c->plain_len - head_len - body_len);
  c->plain_len -= head_len + body_len;
  return true;
}

const char *reply_text(reply_t *r) {
  r->body[r->body_len < sizeof(r->body) ? r->body_len : sizeof(r->body) - 1] = 0;
  return (const char *)r->body;
}

void derive_test_key(bool transient, const char *salt, const char *info, uint8_t key[32]) {
  uint8_t secret[64];
  size_t n = fake_pairing_secret(transient, secret);
  fake_kdf(salt, secret, n, info, key);
}

static void start_encryption(client_t *c, bool transient) {
  uint8_t to_gateway[32], from_gateway[32];
  derive_test_key(transient, "Control-Salt", "Control-Write-Encryption-Key", to_gateway);
  derive_test_key(transient, "Control-Salt", "Control-Read-Encryption-Key", from_gateway);
  rtsp_cipher_init(&c->send_cipher, to_gateway);
  rtsp_frame_reader_init(&c->recv_frames, from_gateway);
  c->send_cipher.counter = fake.channel_counter_start;
  c->recv_frames.cipher.counter = fake.channel_counter_start;
  c->encrypted = true;
}

static void pair_step(client_t *c, const char *uri, uint8_t state, bool transient) {
  uint8_t body[16];
  ap2_tlv8_writer_t w;
  reply_t r;
  ap2_tlv8_writer_init(&w, body, sizeof(body));
  ap2_tlv8_put_byte(&w, AP2_TLV_STATE, state);
  if (transient) ap2_tlv8_put_byte(&w, AP2_TLV_FLAGS, AP2_TLV_FLAG_TRANSIENT);
  client_request(c, "POST", uri, "Content-Type: application/octet-stream\r\n", body,
                 ap2_tlv8_writer_size(&w));
  client_reply(c, &r);
}

void client_pair_transient(client_t *c) {
  pair_step(c, "/pair-setup", 1, true);
  pair_step(c, "/pair-setup", 3, false);
  start_encryption(c, true);
}

void client_pair_verify(client_t *c) {
  pair_step(c, "/pair-verify", 1, false);
  pair_step(c, "/pair-verify", 3, false);
  start_encryption(c, false);
}

int client_setup_session(client_t *c) {
  static const char body[] = "bplist00timingProtocol=sPTP;macAddress=sAA:BB";
  reply_t r;
  unsigned port = 0;
  client_request(c, "SETUP", "rtsp://192.168.1.20/1234", NULL, body, sizeof(body) - 1);
  if (!client_reply(c, &r) || r.code != 200) return 0;
  if (sscanf(reply_text(&r), "SESSION eventPort=%u", &port) != 1) return 0;
  return (int)port;
}

int client_setup_stream(client_t *c, const char *stream, reply_t *reply) {
  char body[512];
  snprintf(body, sizeof(body), "bplist00streams=[{%s}]", stream);
  client_request(c, "SETUP", "rtsp://192.168.1.20/1234", NULL, body, strlen(body));
  if (!client_reply(c, reply)) return -1;
  return reply->code;
}
