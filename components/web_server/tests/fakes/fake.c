/* SPDX-License-Identifier: Apache-2.0 */

/* Host fakes of esp_http_server, the ESP-IDF calls and the firmware
 * functions the web server uses. Everything is recorded for the tests. */

#include "fake.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sound_engine.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "firmware_id.h"
#include "freertos/task.h"
#include "log_stream.h"
#include "mbedtls/base64.h"
#include "ota.h"
#include "settings.h"
#include "web_server_internal.h"
#include "wifi.h"

char fake_calls[65536];
char fake_logs[65536];
fake_httpd_t fake_httpd;
fake_fw_t fake_fw;

static void append(char *dst, size_t cap, const char *fmt, va_list ap) {
  size_t used = strlen(dst);
  if (used + 1 < cap) {
    vsnprintf(dst + used, cap - used, fmt, ap);
  }
}

void fake_call(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  append(fake_calls, sizeof(fake_calls), fmt, ap);
  va_end(ap);
  size_t used = strlen(fake_calls);
  if (used + 2 < sizeof(fake_calls)) {
    fake_calls[used] = ';';
    fake_calls[used + 1] = '\0';
  }
}

bool fake_called(const char *needle) { return strstr(fake_calls, needle); }

bool fake_in_order(const char *first, const char *second) {
  const char *a = strstr(fake_calls, first);
  return a != NULL && strstr(a + strlen(first), second) != NULL;
}

void fake_log(char level, const char *tag, const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  size_t used = strlen(fake_logs);
  snprintf(fake_logs + used, sizeof(fake_logs) - used, "%c %s: %s\n", level,
           tag, line);
}

int64_t fake_now_ms(void) { return fake_fw.now_us / 1000; }

void fake_reset(void) {
  fake_calls[0] = '\0';
  fake_logs[0] = '\0';
  free(fake_fw.scan_records);
  memset(&fake_fw, 0, sizeof(fake_fw));
  fake_fw.ip = "192.168.1.98";
  fake_fw.mac = "14:C1:9F:27:BE:C8";
  fake_fw.connected = true;
  fake_fw.device_name = "ESP32 AirPlay EC8";
  fake_fw.role = "gateway";
  fake_fw.firmware_id = "v4.5.61 29a5ab1d";
  memcpy(fake_fw.app_version, "v4.5.61", 8);
  fake_fw.free_heap = 2956072;
  fake_fw.reset_reason = ESP_RST_SW;
  fake_fw.now_us = 1013LL * 1000000 + 999999;
  fake_fw.source_idle_after_ms = 0;
}

/* ================= HTTP server ================= */

typedef struct {
  const fake_request_t *rq;
  fake_response_t resp;
  size_t pos;        /* body bytes given out */
  size_t script_pos;
  const char *status, *type;
  const char *hdr_name[FAKE_MAX_HDRS], *hdr_value[FAKE_MAX_HDRS];
  size_t nhdr;
  bool flushed, done;
} fake_conn_t;

static fake_conn_t conn;
static httpd_req_t req_obj;

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
  fake_call("httpd_start");
  if (handle == NULL || config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (fake_httpd.start_err != ESP_OK) {
    return fake_httpd.start_err;
  }
  fake_httpd.config = *config;
  fake_httpd.started = true;
  *handle = &fake_httpd;
  return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle,
                                     const httpd_uri_t *h) {
  if (handle != &fake_httpd || h == NULL || h->uri == NULL ||
      h->handler == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  for (size_t i = 0; i < fake_httpd.nhandlers; i++) {
    if (strcmp(fake_httpd.handlers[i].uri, h->uri) == 0 &&
        fake_httpd.handlers[i].method == h->method) {
      return ESP_ERR_HTTPD_HANDLER_EXISTS;
    }
  }
  if (fake_httpd.nhandlers >= fake_httpd.config.max_uri_handlers ||
      fake_httpd.nhandlers >= FAKE_MAX_HANDLERS) {
    return ESP_ERR_HTTPD_HANDLERS_FULL;
  }
  fake_httpd.handlers[fake_httpd.nhandlers++] = *h;
  fake_call("register(%s)", h->uri);
  return ESP_OK;
}

esp_err_t httpd_register_err_handler(httpd_handle_t handle,
                                     httpd_err_code_t error,
                                     httpd_err_handler_func_t fn) {
  if (handle != &fake_httpd || error < 0 || error >= HTTPD_ERR_CODE_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  fake_httpd.err_handlers[error] = fn;
  fake_call("register_err(%d)", (int)error);
  return ESP_OK;
}

static bool check_req(httpd_req_t *r) { return r == &req_obj && conn.rq; }

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) {
  if (!check_req(r) || buf == NULL) {
    return HTTPD_SOCK_ERR_INVALID;
  }
  conn.resp.recv_calls++;
  size_t limit = conn.rq->recv_limit;
  if (conn.script_pos < conn.rq->recv_script_len) {
    int v = conn.rq->recv_script[conn.script_pos++];
    if (v <= 0) {
      return v;
    }
    limit = (size_t)v;
  }
  size_t avail = conn.rq->body_len < conn.rq->content_len
                     ? conn.rq->body_len
                     : conn.rq->content_len;
  if (buf_len == 0 || conn.pos >= avail) {
    return 0;
  }
  size_t n = avail - conn.pos;
  if (n > buf_len) {
    n = buf_len;
  }
  if (limit > 0 && n > limit) {
    n = limit;
  }
  memcpy(buf, conn.rq->body + conn.pos, n);
  conn.pos += n;
  conn.resp.recv_bytes += n;
  return (int)n;
}

static const fake_hdr_t *find_hdr(const char *field) {
  for (size_t i = 0; i < conn.rq->nheaders; i++) {
    if (strcasecmp(conn.rq->headers[i].name, field) == 0) {
      return &conn.rq->headers[i];
    }
  }
  return NULL;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field) {
  if (!check_req(r) || field == NULL) {
    return 0;
  }
  const fake_hdr_t *h = find_hdr(field);
  return h != NULL ? strlen(h->value) : 0;
}

static esp_err_t copy_out(const char *src, size_t len, char *val,
                          size_t val_size) {
  if (val == NULL || val_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  size_t n = len < val_size - 1 ? len : val_size - 1;
  memcpy(val, src, n);
  val[n] = '\0';
  return n < len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field,
                                      char *val, size_t val_size) {
  if (!check_req(r) || field == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  const fake_hdr_t *h = find_hdr(field);
  if (h == NULL) {
    return ESP_ERR_NOT_FOUND;
  }
  return copy_out(h->value, strlen(h->value), val, val_size);
}

static const char *query_of(void) {
  const char *q = strchr(conn.rq->uri, '?');
  return q != NULL ? q + 1 : NULL;
}

size_t httpd_req_get_url_query_len(httpd_req_t *r) {
  if (!check_req(r)) {
    return 0;
  }
  const char *q = query_of();
  return q != NULL ? strlen(q) : 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf,
                                      size_t buf_len) {
  if (!check_req(r)) {
    return ESP_ERR_INVALID_ARG;
  }
  const char *q = query_of();
  if (q == NULL) {
    return ESP_ERR_NOT_FOUND;
  }
  return copy_out(q, strlen(q), buf, buf_len);
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val,
                                size_t val_size) {
  if (qry == NULL || key == NULL || val == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  size_t klen = strlen(key);
  const char *p = qry;
  while (*p) {
    const char *end = strchr(p, '&');
    if (end == NULL) {
      end = p + strlen(p);
    }
    const char *eq = memchr(p, '=', (size_t)(end - p));
    const char *kend = eq != NULL ? eq : end;
    if ((size_t)(kend - p) == klen && strncmp(p, key, klen) == 0) {
      const char *v = eq != NULL ? eq + 1 : end;
      return copy_out(v, (size_t)(end - v), val, val_size);
    }
    p = *end ? end + 1 : end;
  }
  return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) {
  if (!check_req(r) || status == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  conn.status = status;
  return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
  if (!check_req(r) || type == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  conn.type = type;
  return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field,
                             const char *value) {
  if (!check_req(r) || field == NULL || value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (conn.nhdr >= FAKE_MAX_HDRS) {
    return ESP_ERR_HTTPD_RESP_HDR;
  }
  conn.hdr_name[conn.nhdr] = field;
  conn.hdr_value[conn.nhdr] = value;
  conn.nhdr++;
  return ESP_OK;
}

/* The strings set above are read only now, as the real server does. */
static void flush_headers(void) {
  if (conn.flushed) {
    return;
  }
  conn.flushed = true;
  snprintf(conn.resp.status, sizeof(conn.resp.status), "%s",
           conn.status != NULL ? conn.status : "200 OK");
  snprintf(conn.resp.type, sizeof(conn.resp.type), "%s",
           conn.type != NULL ? conn.type : HTTPD_TYPE_TEXT);
  for (size_t i = 0; i < conn.nhdr; i++) {
    snprintf(conn.resp.hdrs[i].name, sizeof(conn.resp.hdrs[i].name), "%s",
             conn.hdr_name[i]);
    snprintf(conn.resp.hdrs[i].value, sizeof(conn.resp.hdrs[i].value), "%s",
             conn.hdr_value[i]);
  }
  conn.resp.nhdrs = conn.nhdr;
}

static void add_body(const char *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != 0) {
      conn.resp.body_all_zero = false;
    }
    if (conn.resp.body_len + i < FAKE_BODY_KEEP) {
      conn.resp.body[conn.resp.body_len + i] = (uint8_t)buf[i];
    }
  }
  conn.resp.body_len += len;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len) {
  if (!check_req(r)) {
    return ESP_ERR_HTTPD_INVALID_REQ;
  }
  if (conn.done || conn.resp.chunks > 0) {
    conn.resp.after_done++;
    return ESP_ERR_HTTPD_INVALID_REQ;
  }
  size_t len = buf_len == HTTPD_RESP_USE_STRLEN
                   ? (buf != NULL ? strlen(buf) : 0)
                   : (size_t)buf_len;
  if (buf == NULL && len > 0) {
    return ESP_ERR_INVALID_ARG;
  }
  flush_headers();
  add_body(buf, len);
  conn.resp.sends++;
  conn.done = true;
  fake_call("send(%s)", conn.resp.status);
  return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf,
                                ssize_t buf_len) {
  if (!check_req(r)) {
    return ESP_ERR_HTTPD_INVALID_REQ;
  }
  if (conn.done) {
    conn.resp.after_done++;
    return ESP_ERR_HTTPD_INVALID_REQ;
  }
  size_t len = buf_len == HTTPD_RESP_USE_STRLEN
                   ? (buf != NULL ? strlen(buf) : 0)
                   : (size_t)buf_len;
  if (len > 0 && conn.rq->fail_chunk_from > 0 &&
      conn.resp.chunks + 1 >= conn.rq->fail_chunk_from) {
    return ESP_ERR_HTTPD_RESP_SEND;
  }
  flush_headers();
  if (len == 0) {
    conn.resp.chunk_end = true;
    conn.done = true;
    fake_call("send(%s)", conn.resp.status);
    return ESP_OK;
  }
  if (buf == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  add_body(buf, len);
  conn.resp.chunks++;
  if (len > conn.resp.max_chunk) {
    conn.resp.max_chunk = len;
  }
  return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error,
                              const char *msg) {
  const char *status;
  switch (error) {
    case HTTPD_400_BAD_REQUEST:
      status = "400 Bad Request";
      break;
    case HTTPD_401_UNAUTHORIZED:
      status = "401 Unauthorized";
      break;
    case HTTPD_404_NOT_FOUND:
      status = "404 Not Found";
      break;
    case HTTPD_500_INTERNAL_SERVER_ERROR:
      status = "500 Internal Server Error";
      break;
    default:
      status = "000 Fake";
      break;
  }
  conn.resp.send_err_calls++;
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, HTTPD_TYPE_TEXT);
  return httpd_resp_send(req, msg != NULL ? msg : status,
                         HTTPD_RESP_USE_STRLEN);
}

const fake_response_t *fake_run(const fake_request_t *rq) {
  free(conn.resp.body);
  memset(&conn, 0, sizeof(conn));
  memset(&req_obj, 0, sizeof(req_obj));
  conn.rq = rq;
  conn.resp.body = calloc(1, FAKE_BODY_KEEP + 1);
  conn.resp.body_all_zero = true;
  req_obj.handle = &fake_httpd;
  req_obj.method = rq->method;
  snprintf((char *)req_obj.uri, sizeof(req_obj.uri), "%s", rq->uri);
  req_obj.content_len = rq->content_len;

  size_t path_len = strcspn(rq->uri, "?");
  bool path_known = false;
  for (size_t i = 0; i < fake_httpd.nhandlers; i++) {
    const httpd_uri_t *h = &fake_httpd.handlers[i];
    if (strlen(h->uri) != path_len || strncmp(h->uri, rq->uri, path_len)) {
      continue;
    }
    path_known = true;
    if ((int)h->method == rq->method) {
      req_obj.user_ctx = h->user_ctx;
      conn.resp.found = true;
      conn.resp.handler_ret = h->handler(&req_obj);
      return &conn.resp;
    }
  }
  httpd_err_code_t code =
      path_known ? HTTPD_405_METHOD_NOT_ALLOWED : HTTPD_404_NOT_FOUND;
  if (fake_httpd.err_handlers[code] != NULL) {
    conn.resp.handler_ret = fake_httpd.err_handlers[code](&req_obj, code);
  } else {
    httpd_resp_send_err(&req_obj, code, NULL);
    conn.resp.handler_ret = ESP_FAIL;
  }
  return &conn.resp;
}

const char *fake_resp_hdr(const fake_response_t *r, const char *name) {
  for (size_t i = 0; i < r->nhdrs; i++) {
    if (strcasecmp(r->hdrs[i].name, name) == 0) {
      return r->hdrs[i].value;
    }
  }
  return NULL;
}

const char *fake_body_str(const fake_response_t *r) {
  size_t n = r->body_len < FAKE_BODY_KEEP ? r->body_len : FAKE_BODY_KEEP;
  r->body[n] = '\0';
  return (const char *)r->body;
}

/* ================= ESP-IDF ================= */

const char *esp_err_to_name(esp_err_t code) {
  switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_WIFI_NOT_STARTED: return "ESP_ERR_WIFI_NOT_STARTED";
    case ESP_ERR_WIFI_NOT_CONNECT: return "ESP_ERR_WIFI_NOT_CONNECT";
    case ESP_ERR_OTA_VALIDATE_FAILED: return "ESP_ERR_OTA_VALIDATE_FAILED";
    case ESP_ERR_HTTPD_HANDLERS_FULL: return "ESP_ERR_HTTPD_HANDLERS_FULL";
    case ESP_ERR_HTTPD_HANDLER_EXISTS: return "ESP_ERR_HTTPD_HANDLER_EXISTS";
    case ESP_ERR_HTTPD_TASK: return "ESP_ERR_HTTPD_TASK";
    default: return "UNKNOWN ERROR";
  }
}

void vTaskDelay(TickType_t ticks) {
  uint64_t ms = (uint64_t)ticks * 1000 / configTICK_RATE_HZ;
  fake_fw.now_us += (int64_t)ms * 1000;
  if (ms != 50) { /* the 50 ms polls would flood the log */
    fake_call("delay(%llu)", (unsigned long long)ms);
  }
}

void esp_restart(void) {
  fake_fw.restarts++;
  fake_call("restart");
}

uint32_t esp_get_free_heap_size(void) { return fake_fw.free_heap; }
esp_reset_reason_t esp_reset_reason(void) {
  return (esp_reset_reason_t)fake_fw.reset_reason;
}
int64_t esp_timer_get_time(void) { return fake_fw.now_us; }

const esp_app_desc_t *esp_app_get_description(void) {
  static esp_app_desc_t desc;
  memcpy(desc.version, fake_fw.app_version, sizeof(desc.version));
  return &desc;
}

size_t heap_caps_get_free_size(uint32_t caps) {
  fake_call("heap_free(0x%x)", (unsigned)caps);
  return (caps & MALLOC_CAP_SPIRAM) ? fake_fw.heap_psram_free
                                    : fake_fw.heap_internal_free;
}

size_t heap_caps_get_largest_free_block(uint32_t caps) {
  fake_call("heap_largest(0x%x)", (unsigned)caps);
  return (caps & MALLOC_CAP_SPIRAM) ? fake_fw.heap_psram_largest
                                    : fake_fw.heap_internal_largest;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info) {
  if (fake_fw.ap_info_err != ESP_OK) {
    return fake_fw.ap_info_err;
  }
  *ap_info = fake_fw.ap_info;
  return ESP_OK;
}

static int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen) {
  *olen = 0;
  if (slen % 4 != 0) {
    return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
  }
  size_t pad = 0;
  if (slen > 0 && src[slen - 1] == '=') pad++;
  if (slen > 1 && src[slen - 2] == '=') pad++;
  for (size_t i = 0; i < slen - pad; i++) {
    if (b64_value(src[i]) < 0) {
      return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
    }
  }
  size_t need = slen / 4 * 3 - pad;
  if (dst == NULL || dlen < need) {
    *olen = need;
    return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  }
  size_t o = 0;
  for (size_t i = 0; i < slen; i += 4) {
    uint32_t v = 0;
    for (size_t j = 0; j < 4; j++) {
      int x = src[i + j] == '=' ? 0 : b64_value(src[i + j]);
      v = (v << 6) | (uint32_t)x;
    }
    unsigned char b[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8),
                          (unsigned char)v};
    for (size_t j = 0; j < 3 && o < need; j++) {
      dst[o++] = b[j];
    }
  }
  *olen = o;
  return 0;
}

/* ================= Firmware ================= */

void wifi_get_mac_str(char *mac_str, size_t len) {
  snprintf(mac_str, len, "%s", fake_fw.mac);
}

bool wifi_is_connected(void) { return fake_fw.connected; }

esp_err_t wifi_get_ip_str(char *ip_str, size_t len) {
  if (fake_fw.ip_err != ESP_OK) {
    snprintf(ip_str, len, "%s", "999.999.999.999"); /* garbage */
    return fake_fw.ip_err;
  }
  snprintf(ip_str, len, "%s", fake_fw.ip);
  return ESP_OK;
}

esp_err_t wifi_scan(wifi_ap_record_t **ap_list, uint16_t *ap_count) {
  fake_call("wifi_scan");
  if (fake_fw.scan_err != ESP_OK) {
    return fake_fw.scan_err;
  }
  *ap_count = fake_fw.scan_count;
  if (fake_fw.scan_null_list) {
    *ap_list = NULL;
    return ESP_OK;
  }
  size_t size = sizeof(wifi_ap_record_t) * fake_fw.scan_count;
  *ap_list = malloc(size > 0 ? size : 1);
  if (size > 0) {
    memcpy(*ap_list, fake_fw.scan_records, size);
  }
  return ESP_OK;
}

void wifi_set_hostname(const char *device_name) {
  snprintf(fake_fw.hostname, sizeof(fake_fw.hostname), "%s", device_name);
  fake_call("wifi_set_hostname(%s)", device_name);
}

esp_err_t settings_get_device_name(char *name, size_t len) {
  if (fake_fw.get_name_err != ESP_OK) {
    snprintf(name, len, "%s", "junk");
    return fake_fw.get_name_err;
  }
  if (fake_fw.name_fill_no_nul) {
    memset(name, 'N', len);
    return ESP_OK;
  }
  snprintf(name, len, "%s", fake_fw.device_name);
  return ESP_OK;
}

esp_err_t settings_set_device_name(const char *name) {
  snprintf(fake_fw.set_name, sizeof(fake_fw.set_name), "%s", name);
  fake_call("settings_set_device_name(%s)", name);
  return fake_fw.set_name_err;
}

esp_err_t settings_set_wifi_credentials(const char *ssid,
                                        const char *password) {
  snprintf(fake_fw.set_ssid, sizeof(fake_fw.set_ssid), "%s", ssid);
  snprintf(fake_fw.set_password, sizeof(fake_fw.set_password), "%s", password);
  fake_call("settings_set_wifi_credentials(%s,%s)", ssid, password);
  return fake_fw.set_wifi_err;
}

const char *web_role_name(void) { return fake_fw.role; }

static esp_err_t role_handler(httpd_req_t *req) {
  return httpd_resp_sendstr(req, "role");
}

void web_role_register(httpd_handle_t server) {
  static char uris[32][32];
  fake_call("web_role_register");
  for (int i = 0; i < fake_fw.role_handlers && i < 32; i++) {
    snprintf(uris[i], sizeof(uris[i]), "/api/role/%d", i);
    httpd_uri_t h = {.uri = uris[i], .method = HTTP_GET,
                     .handler = role_handler};
    httpd_register_uri_handler(server, &h);
  }
}

esp_err_t web_role_source_stop(void) {
  fake_call("source_stop");
  fake_fw.source_stop_at_ms = fake_now_ms();
  return fake_fw.source_stop_err;
}

bool web_role_source_idle(void) {
  if (fake_fw.source_idle_after_ms < 0) {
    return false;
  }
  return fake_now_ms() - fake_fw.source_stop_at_ms >=
         fake_fw.source_idle_after_ms;
}

esp_err_t web_role_source_start(void) {
  fake_call("source_start");
  return fake_fw.source_start_err;
}

void firmware_id(char *out, size_t len) {
  snprintf(out, len, "%s", fake_fw.firmware_id);
}

esp_err_t sound_engine_init(void) {
  fake_call("audio_init");
  if (fake_fw.audio_init_err == ESP_OK) {
    fake_fw.audio_initialized = true;
  }
  return fake_fw.audio_init_err;
}

bool sound_engine_is_initialized(void) { return fake_fw.audio_initialized; }

esp_err_t sound_engine_release_for_wifi_scan(void) {
  fake_call("audio_release");
  if (fake_fw.audio_release_err == ESP_OK) {
    fake_fw.audio_initialized = false;
  }
  return fake_fw.audio_release_err;
}

esp_err_t ota_start_from_http(httpd_req_t *req) {
  fake_call("ota_start");
  char buf[256];
  size_t left = fake_fw.ota_read;
  while (left > 0) {
    int r = httpd_req_recv(req, buf, left < sizeof(buf) ? left : sizeof(buf));
    if (r <= 0) {
      return ESP_FAIL;
    }
    left -= (size_t)r;
  }
  return fake_fw.ota_err;
}

static esp_err_t ws_handler(httpd_req_t *req) {
  (void)req;
  return ESP_OK;
}

esp_err_t log_stream_register(httpd_handle_t server) {
  fake_call("log_stream_register");
  httpd_uri_t h = {.uri = "/ws/logs", .method = HTTP_GET,
                   .handler = ws_handler, .is_websocket = true};
  esp_err_t err = httpd_register_uri_handler(server, &h);
  return fake_fw.log_stream_err != ESP_OK ? fake_fw.log_stream_err : err;
}

/* ================= Embedded pages ================= */

/* The firmware's build embeds them as binary data; here they are 20000,
 * 10000 and 1025 bytes that the tests may overwrite. */
__asm__(
    ".data\n"
    ".globl _binary_web_index_html_start\n"
    "_binary_web_index_html_start:\n"
    ".space 20000, 0x61\n"
    ".globl _binary_web_index_html_end\n"
    "_binary_web_index_html_end:\n"
    ".globl _binary_web_logs_html_start\n"
    "_binary_web_logs_html_start:\n"
    ".space 10000, 0x62\n"
    ".globl _binary_web_logs_html_end\n"
    "_binary_web_logs_html_end:\n"
    ".globl _binary_web_speedtest_html_start\n"
    "_binary_web_speedtest_html_start:\n"
    ".space 1025, 0x63\n"
    ".globl _binary_web_speedtest_html_end\n"
    "_binary_web_speedtest_html_end:\n"
    ".text\n");
