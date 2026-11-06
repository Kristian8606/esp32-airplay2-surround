/* SPDX-License-Identifier: Apache-2.0 */
#include "rtsp_msg.h"

#include <stdio.h>
#include <string.h>

#include "airplay_identity.h"
#include "rtsp_port.h"

enum {
  PHASE_HEAD,
  PHASE_BODY,
  PHASE_DISCARD,
  PHASE_DONE,
};

/* A Content-Length above this is malformed, not just too large. */
#define CONTENT_LENGTH_LIMIT 0x7fffffffUL

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool rtsp_str_ieq(const char *a, const char *b) {
  while (*a && *b) {
    if (lower(*a) != lower(*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

void rtsp_reader_init(rtsp_reader_t *reader, size_t body_max) {
  memset(reader, 0, sizeof(*reader));
  reader->body_max = body_max;
  reader->phase = PHASE_HEAD;
}

void rtsp_reader_next(rtsp_reader_t *reader) {
  if (reader->msg.body != NULL) rtsp_port_free(reader->msg.body);
  memset(&reader->msg, 0, sizeof(reader->msg));
  reader->head_len = 0;
  reader->body_got = 0;
  reader->phase = PHASE_HEAD;
}

const char *rtsp_msg_header(const rtsp_msg_t *msg, const char *name) {
  for (size_t i = 0; i < msg->header_count; i++) {
    if (rtsp_str_ieq(msg->headers[i].name, name)) return msg->headers[i].value;
  }
  return NULL;
}

static bool head_complete(const rtsp_reader_t *r) {
  size_t n = r->head_len;
  if (n >= 2 && r->head[n - 1] == '\n' && r->head[n - 2] == '\n') return true;
  if (n >= 3 && r->head[n - 1] == '\n' && r->head[n - 2] == '\r' &&
      r->head[n - 3] == '\n') {
    return true;
  }
  return false;
}

/* Digits only, at most CONTENT_LENGTH_LIMIT. */
static bool parse_length(const char *s, size_t *out) {
  unsigned long v = 0;
  if (*s == '\0') return false;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10 + (unsigned long)(*s - '0');
    if (v > CONTENT_LENGTH_LIMIT) return false;
  }
  *out = (size_t)v;
  return true;
}

static char *trim(char *s) {
  char *end;
  while (*s == ' ' || *s == '\t') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
  return s;
}

/* Splits the head in place into the start line and headers. */
static bool parse_head(rtsp_reader_t *r) {
  rtsp_msg_t *m = &r->msg;
  char *p = r->head;
  char *end = r->head + r->head_len;
  bool first = true;
  bool have_length = false;

  for (size_t i = 0; i < r->head_len; i++) {
    unsigned char c = (unsigned char)r->head[i];
    if ((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c == 0x7f) {
      return false;
    }
  }
  r->head[r->head_len] = '\0';

  while (p < end) {
    char *nl = memchr(p, '\n', (size_t)(end - p));
    char *line = p;
    if (nl == NULL) return false;
    *nl = '\0';
    if (nl > line && nl[-1] == '\r') nl[-1] = '\0';
    p = nl + 1;
    if (line[0] == '\0') break; /* the empty line */

    if (first) {
      char *sp1 = strchr(line, ' ');
      char *sp2;
      first = false;
      if (sp1 == NULL || sp1 == line) return false;
      *sp1 = '\0';
      m->start[0] = line;
      m->start[1] = sp1 + 1;
      sp2 = strchr(sp1 + 1, ' ');
      if (sp2 != NULL) {
        *sp2 = '\0';
        m->start[2] = sp2 + 1;
      } else {
        m->start[2] = "";
      }
      if (m->start[1][0] == '\0') return false;
      continue;
    }

    if (line[0] == ' ' || line[0] == '\t') return false; /* folding */
    {
      char *colon = strchr(line, ':');
      char *name_end;
      if (colon == NULL || colon == line) return false;
      for (name_end = line; name_end < colon; name_end++) {
        if (*name_end == ' ' || *name_end == '\t') return false;
      }
      *colon = '\0';
      if (m->header_count >= RTSP_MAX_HEADERS) return false;
      m->headers[m->header_count].name = line;
      m->headers[m->header_count].value = trim(colon + 1);
      if (rtsp_str_ieq(line, "Content-Length")) {
        if (have_length) return false;
        if (!parse_length(m->headers[m->header_count].value, &m->body_len)) {
          return false;
        }
        have_length = true;
      }
      m->header_count++;
    }
  }
  return !first;
}

rtsp_read_result_t rtsp_reader_feed(rtsp_reader_t *reader, const uint8_t *data,
                                    size_t len, size_t *consumed) {
  size_t used = 0;
  rtsp_msg_t *m = &reader->msg;

  if (reader->phase == PHASE_DONE) rtsp_reader_next(reader);

  if (reader->phase == PHASE_HEAD) {
    bool done = false;
    while (used < len) {
      char c = (char)data[used++];
      if (reader->head_len == 0 && (c == '\r' || c == '\n')) continue;
      if (reader->head_len >= RTSP_HEAD_MAX) {
        *consumed = used;
        return RTSP_READ_BAD;
      }
      reader->head[reader->head_len++] = c;
      if (c == '\n' && head_complete(reader)) {
        done = true;
        break;
      }
    }
    if (!done) {
      if (reader->head_len >= RTSP_HEAD_MAX) {
        *consumed = used;
        return RTSP_READ_BAD;
      }
      *consumed = used;
      return RTSP_READ_MORE;
    }
    if (!parse_head(reader)) {
      *consumed = used;
      return RTSP_READ_BAD;
    }
    m->body_state = RTSP_BODY_OK;
    reader->body_got = 0;
    if (m->body_len == 0) {
      reader->phase = PHASE_DONE;
      *consumed = used;
      return RTSP_READ_MESSAGE;
    }
    if (m->body_len > reader->body_max) {
      m->body_state = RTSP_BODY_TOO_LARGE;
      reader->phase = PHASE_DISCARD;
    } else {
      m->body = rtsp_port_malloc(m->body_len);
      if (m->body == NULL) {
        m->body_state = RTSP_BODY_NO_MEMORY;
        reader->phase = PHASE_DISCARD;
      } else {
        reader->phase = PHASE_BODY;
      }
    }
  }

  {
    size_t n = m->body_len - reader->body_got;
    if (n > len - used) n = len - used;
    if (reader->phase == PHASE_BODY) memcpy(m->body + reader->body_got, data + used, n);
    reader->body_got += n;
    used += n;
  }
  *consumed = used;
  if (reader->body_got < m->body_len) return RTSP_READ_MORE;
  reader->phase = PHASE_DONE;
  return RTSP_READ_MESSAGE;
}

const char *rtsp_reason(int code) {
  switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 413: return "Request Entity Too Large";
    case 455: return "Method Not Valid in This State";
    case 470: return "Connection Authorization Required";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Unknown";
  }
}

size_t rtsp_reply_head(char *out, size_t capacity, int code, const char *cseq,
                       const char *extra, const char *content_type,
                       size_t body_len) {
  size_t n = 0;
  int w;

#define APPEND(...)                                                  \
  do {                                                               \
    w = snprintf(out + n, capacity - n, __VA_ARGS__);                \
    if (w < 0 || (size_t)w >= capacity - n) return 0;                \
    n += (size_t)w;                                                  \
  } while (0)

  if (capacity == 0) return 0;
  APPEND("RTSP/1.0 %d %s\r\n", code, rtsp_reason(code));
  if (cseq != NULL) APPEND("CSeq: %s\r\n", cseq);
  APPEND("Server: %s\r\n", AIRPLAY_SERVER_HEADER);
  if (extra != NULL) APPEND("%s", extra);
  if (body_len > 0) {
    APPEND("Content-Type: %s\r\n", content_type != NULL
                                       ? content_type
                                       : "application/octet-stream");
    APPEND("Content-Length: %zu\r\n", body_len);
  }
  APPEND("\r\n");
#undef APPEND
  return n;
}
