/* SPDX-License-Identifier: Apache-2.0 */
/* Remote commands to the sender over the event channel (spec "Event
 * channel"). */
#include "rtsp_remote.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ap2_plist.h"
#include "rtsp_core.h"
#include "rtsp_core_internal.h"
#include "rtsp_frame.h"
#include "rtsp_log.h"
#include "rtsp_msg.h"
#include "rtsp_tables.h"

/* Replies from the sender are small. */
#define EVENT_BODY_MAX 4096
#define COMMAND_BODY_MAX 256
#define COMMAND_HEAD_MAX 192

struct rtsp_evconn {
  const rtsp_conn_t *conn;    /* whose event port; NULL once done */
  rtsp_evconn_t *next;        /* the list of live event connections */
  rtsp_cipher_t out;          /* Gateway to sender */
  rtsp_frame_reader_t in;     /* sender to Gateway */
  rtsp_reader_t reader;
  uint8_t body[COMMAND_BODY_MAX];
  char head[COMMAND_HEAD_MAX];
  uint8_t frame[RTSP_FRAME_MAX + RTSP_FRAME_OVERHEAD];
};

static struct {
  rtsp_evconn_t *live; /* every event connection not yet freed */
  uint32_t last_id;
  rtsp_remote_status_t status;
  uint32_t sent_ms;
  double fraction; /* the volume a volume command sent */
} r;

/* Commands go only to the session holder's event connection (spec "Event
 * channel"). After a take-over that is the new holder's, already open. */
static rtsp_evconn_t *target(void) {
  rtsp_conn_t *holder = rtsp_core_holder();
  if (holder == NULL) return NULL;
  for (rtsp_evconn_t *e = r.live; e != NULL; e = e->next) {
    if (e->conn == holder) return e;
  }
  return NULL;
}

static bool pending(void) {
  return r.status.result == RTSP_REMOTE_QUEUED ||
         r.status.result == RTSP_REMOTE_SENT;
}

static void end_pending_disconnected(void) {
  if (pending()) {
    r.status.result = RTSP_REMOTE_DISCONNECTED;
    RTSP_LOGW("remote command %" PRIu32 ": disconnected", r.status.id);
  }
}

esp_err_t rtsp_remote_enqueue(uint8_t command, uint32_t *id) {
  if (command > RTSP_REMOTE_VOLUME_UP) return ESP_ERR_INVALID_ARG;
  rtsp_port_lock();
  if (target() == NULL) {
    rtsp_port_unlock();
    return ESP_ERR_NOT_FOUND;
  }
  if (pending()) {
    rtsp_port_unlock();
    return ESP_ERR_INVALID_STATE;
  }
  r.last_id++;
  if (r.last_id == 0) r.last_id = 1;
  r.status.id = r.last_id;
  r.status.command = command;
  r.status.result = RTSP_REMOTE_QUEUED;
  r.status.response_code = 0;
  if (id != NULL) *id = r.last_id;
  RTSP_LOGI("remote command %" PRIu32 " (%u) queued", r.last_id, command);
  rtsp_port_unlock();
  return ESP_OK;
}

void rtsp_remote_get_status(rtsp_remote_status_t *status) {
  if (status == NULL) return;
  rtsp_port_lock();
  *status = r.status;
  status->connected = target() != NULL;
  rtsp_port_unlock();
}

const char *rtsp_remote_result_name(rtsp_remote_result_t result) {
  switch (result) {
    case RTSP_REMOTE_IDLE: return "idle";
    case RTSP_REMOTE_QUEUED: return "queued";
    case RTSP_REMOTE_SENT: return "sent";
    case RTSP_REMOTE_ACKNOWLEDGED: return "acknowledged";
    case RTSP_REMOTE_REJECTED: return "rejected";
    case RTSP_REMOTE_TIMEOUT: return "timeout";
    case RTSP_REMOTE_FAILED: return "failed";
    case RTSP_REMOTE_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

void rtsp_remote_detach(void) { end_pending_disconnected(); }

void rtsp_remote_conn_gone(const rtsp_conn_t *conn) {
  for (rtsp_evconn_t *e = r.live; e != NULL; e = e->next) {
    if (e->conn != conn) continue;
    if (e == target()) end_pending_disconnected();
    e->conn = NULL;
  }
}

void rtsp_remote_test_reset(void) { memset(&r, 0, sizeof(r)); }

rtsp_evconn_t *rtsp_evconn_new(const rtsp_conn_t *conn,
                               const uint8_t write_key[32],
                               const uint8_t read_key[32]) {
  rtsp_evconn_t *ev = rtsp_port_malloc(sizeof(*ev));
  if (ev == NULL) return NULL;
  memset(ev, 0, sizeof(*ev));
  rtsp_cipher_init(&ev->out, write_key);
  rtsp_frame_reader_init(&ev->in, read_key);
  rtsp_reader_init(&ev->reader, EVENT_BODY_MAX);
  rtsp_port_lock();
  /* It replaces an earlier one on the same port. A command sent on the old
   * one gets no reply on this one. */
  for (rtsp_evconn_t *e = r.live; e != NULL; e = e->next) {
    if (e->conn != conn) continue;
    if (e == target() && r.status.result == RTSP_REMOTE_SENT) {
      end_pending_disconnected();
    }
    e->conn = NULL;
  }
  ev->conn = conn;
  ev->next = r.live;
  r.live = ev;
  rtsp_port_unlock();
  RTSP_LOGI("event connection open");
  return ev;
}

void rtsp_evconn_free(rtsp_evconn_t *ev) {
  if (ev == NULL) return;
  rtsp_port_lock();
  if (ev == target()) end_pending_disconnected();
  for (rtsp_evconn_t **p = &r.live; *p != NULL; p = &(*p)->next) {
    if (*p == ev) {
      *p = ev->next;
      break;
    }
  }
  rtsp_port_unlock();
  rtsp_reader_next(&ev->reader);
  rtsp_cipher_wipe(&ev->out);
  rtsp_frame_reader_wipe(&ev->in);
  rtsp_port_free(ev);
  RTSP_LOGI("event connection closed");
}

/* Head and body as one message: one frame when it fits (spec "Encryption";
 * a command in two frames got no reply from an Apple TV). */
static int send_sealed(rtsp_evconn_t *ev, const uint8_t *head, size_t head_len,
                       const uint8_t *body, size_t body_len, rtsp_send_fn send,
                       void *ctx) {
  size_t off = 0, n;
  while ((n = rtsp_frame_gather(head, head_len, body, body_len, off,
                                ev->frame + 2)) > 0) {
    size_t frame_len = rtsp_frame_seal(&ev->out, ev->frame + 2, n, ev->frame);
    if (send(ctx, ev->frame, frame_len) != 0) return -1;
    off += n;
  }
  return 0;
}

int rtsp_evconn_poll(rtsp_evconn_t *ev, uint32_t now_ms, rtsp_send_fn send,
                     void *ctx) {
  uint32_t id;
  size_t body_len, head_len;
  int w;

  rtsp_port_lock();
  if (ev->conn == NULL) {
    /* replaced, or its port closed */
    rtsp_port_unlock();
    return -1;
  }
  if (ev != target()) {
    /* not the holder's: it stays open and gets nothing */
    rtsp_port_unlock();
    return 0;
  }
  if (r.status.result == RTSP_REMOTE_SENT &&
      (uint32_t)(now_ms - r.sent_ms) >= RTSP_REMOTE_TIMEOUT_MS) {
    /* The event connection stays open for the next command; a late reply
     * no longer matches (spec "Event channel"). */
    r.status.result = RTSP_REMOTE_TIMEOUT;
    RTSP_LOGW("remote command %" PRIu32 ": no reply, timeout", r.status.id);
    rtsp_port_unlock();
    return 0;
  }
  if (r.status.result != RTSP_REMOTE_QUEUED) {
    rtsp_port_unlock();
    return 0;
  }

  id = r.status.id;
  if (r.status.command >= RTSP_REMOTE_VOLUME_DOWN) {
    r.fraction = rtsp_volume_step(rtsp_core_current_volume(),
                                  r.status.command == RTSP_REMOTE_VOLUME_UP);
    body_len = ap2_event_volume(ev->body, sizeof(ev->body), r.fraction);
  } else {
    body_len = ap2_event_command(ev->body, sizeof(ev->body), r.status.command);
  }
  w = snprintf(ev->head, sizeof(ev->head),
               "POST /command RTSP/1.0\r\nCSeq: %" PRIu32
               "\r\nContent-Type: application/x-apple-binary-plist\r\n"
               "Content-Length: %zu\r\n\r\n",
               id, body_len);
  if (body_len == 0 || w < 0 || (size_t)w >= sizeof(ev->head)) {
    r.status.result = RTSP_REMOTE_FAILED;
    RTSP_LOGW("remote command %" PRIu32 ": body not written", id);
    rtsp_port_unlock();
    return 0;
  }
  head_len = (size_t)w;
  r.status.result = RTSP_REMOTE_SENT;
  r.sent_ms = now_ms;
  rtsp_port_unlock();

  if (send_sealed(ev, (const uint8_t *)ev->head, head_len, ev->body, body_len,
                  send, ctx) != 0) {
    rtsp_port_lock();
    if (r.status.id == id && r.status.result == RTSP_REMOTE_SENT) {
      r.status.result = RTSP_REMOTE_FAILED;
    }
    rtsp_port_unlock();
    RTSP_LOGW("remote command %" PRIu32 ": send failed", id);
    return -1;
  }
  RTSP_LOGI("remote command %" PRIu32 " sent", id);
  return 0;
}

/* "RTSP/1.0 200 OK" with a CSeq: the result of the command. Requests from
 * the sender are not expected and are ignored. */
static void take_message(rtsp_evconn_t *ev, const rtsp_msg_t *m) {
  const char *cseq_text = rtsp_msg_header(m, "CSeq");
  const char *code_text = m->start[1];
  uint64_t cseq = 0;
  int code = 0;

  if (strncmp(m->start[0], "RTSP/", 5) != 0) {
    RTSP_LOGW("event channel: request %s ignored", m->start[0]);
    return;
  }
  if (strlen(code_text) != 3) return;
  for (int i = 0; i < 3; i++) {
    if (code_text[i] < '0' || code_text[i] > '9') return;
    code = code * 10 + (code_text[i] - '0');
  }
  if (cseq_text == NULL || *cseq_text == '\0') return;
  for (const char *p = cseq_text; *p; p++) {
    if (*p < '0' || *p > '9') return;
    cseq = cseq * 10 + (uint64_t)(*p - '0');
    if (cseq > UINT32_MAX) return;
  }

  rtsp_port_lock();
  if (ev == target() && r.status.result == RTSP_REMOTE_SENT &&
      cseq == r.status.id) {
    r.status.response_code = code;
    if (code == 200) {
      r.status.result = RTSP_REMOTE_ACKNOWLEDGED;
      if (r.status.command >= RTSP_REMOTE_VOLUME_DOWN) {
        rtsp_core_remote_volume(rtsp_fraction_to_volume(r.fraction));
      }
    } else {
      r.status.result = RTSP_REMOTE_REJECTED;
    }
    RTSP_LOGI("remote command %" PRIu32 ": %d, %s", (uint32_t)cseq, code,
              rtsp_remote_result_name(r.status.result));
  }
  rtsp_port_unlock();
}

int rtsp_evconn_receive(rtsp_evconn_t *ev, const uint8_t *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    size_t used = 0, plain_len = 0, plain_off = 0;
    const uint8_t *plain = NULL;
    rtsp_frame_result_t fr =
        rtsp_frame_feed(&ev->in, data + off, len - off, &used, &plain,
                        &plain_len);
    off += used;
    if (fr == RTSP_FRAME_ERROR) {
      RTSP_LOGW("event channel: frame does not open, closing");
      return -1;
    }
    if (fr == RTSP_FRAME_MORE) break;
    while (plain_off < plain_len) {
      size_t n = 0;
      rtsp_read_result_t rr = rtsp_reader_feed(
          &ev->reader, plain + plain_off, plain_len - plain_off, &n);
      plain_off += n;
      if (rr == RTSP_READ_BAD) {
        RTSP_LOGW("event channel: malformed message, closing");
        return -1;
      }
      if (rr == RTSP_READ_MORE) break;
      take_message(ev, &ev->reader.msg);
      rtsp_reader_next(&ev->reader);
    }
  }
  return 0;
}
