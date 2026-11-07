/* SPDX-License-Identifier: Apache-2.0 */
/* Host fake of ESP-IDF esp_http_server.h, after the v6.0 API reference. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

#define HTTPD_MAX_URI_LEN 512

typedef void *httpd_handle_t;
typedef void (*httpd_free_ctx_fn_t)(void *ctx);

typedef enum {
  HTTP_DELETE = 0,
  HTTP_GET = 1,
  HTTP_HEAD = 2,
  HTTP_POST = 3,
  HTTP_PUT = 4,
} httpd_method_t;

typedef struct httpd_req {
  httpd_handle_t handle;
  int method;
  const char uri[HTTPD_MAX_URI_LEN + 1];
  size_t content_len;
  void *aux;
  void *user_ctx;
  void *sess_ctx;
  httpd_free_ctx_fn_t free_ctx;
  bool ignore_sess_ctx_changes;
} httpd_req_t;

typedef struct httpd_uri {
  const char *uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t *r);
  void *user_ctx;
  bool is_websocket;
  bool handle_ws_control_frames;
  const char *supported_subprotocol;
} httpd_uri_t;

typedef bool (*httpd_uri_match_func_t)(const char *reference_uri,
                                       const char *uri_to_match,
                                       size_t match_upto);

typedef struct httpd_config {
  unsigned task_priority;
  size_t stack_size;
  int core_id;
  uint32_t task_caps;
  size_t max_req_hdr_len;
  size_t max_uri_len;
  uint16_t server_port;
  uint16_t ctrl_port;
  uint16_t max_open_sockets;
  uint16_t max_uri_handlers;
  uint16_t max_resp_headers;
  uint16_t backlog_conn;
  bool lru_purge_enable;
  uint16_t recv_wait_timeout;
  uint16_t send_wait_timeout;
  void *global_user_ctx;
  httpd_free_ctx_fn_t global_user_ctx_free_fn;
  void *global_transport_ctx;
  httpd_free_ctx_fn_t global_transport_ctx_free_fn;
  bool enable_so_linger;
  int linger_timeout;
  bool keep_alive_enable;
  int keep_alive_idle;
  int keep_alive_interval;
  int keep_alive_count;
  void *open_fn;
  void *close_fn;
  httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;

/* The fake's defaults; tests check that the server keeps them. */
#define FAKE_HTTPD_DEFAULT_TASK_CAPS 0x0804u
#define HTTPD_DEFAULT_CONFIG()                                              \
  {                                                                         \
      .task_priority = 5, .stack_size = 4096, .core_id = 0x7fffffff,        \
      .task_caps = FAKE_HTTPD_DEFAULT_TASK_CAPS, .max_req_hdr_len = 512,    \
      .max_uri_len = 512, .server_port = 80, .ctrl_port = 32768,            \
      .max_open_sockets = 7, .max_uri_handlers = 8, .max_resp_headers = 8,  \
      .backlog_conn = 5, .lru_purge_enable = false, .recv_wait_timeout = 5, \
      .send_wait_timeout = 5, .global_user_ctx = NULL,                      \
      .global_user_ctx_free_fn = NULL, .global_transport_ctx = NULL,        \
      .global_transport_ctx_free_fn = NULL, .enable_so_linger = false,      \
      .linger_timeout = 0, .keep_alive_enable = false,                      \
      .keep_alive_idle = 0, .keep_alive_interval = 0,                       \
      .keep_alive_count = 0, .open_fn = NULL, .close_fn = NULL,             \
      .uri_match_fn = NULL                                                  \
  }

typedef enum {
  HTTPD_500_INTERNAL_SERVER_ERROR = 0,
  HTTPD_501_METHOD_NOT_IMPLEMENTED,
  HTTPD_505_VERSION_NOT_SUPPORTED,
  HTTPD_400_BAD_REQUEST,
  HTTPD_401_UNAUTHORIZED,
  HTTPD_403_FORBIDDEN,
  HTTPD_404_NOT_FOUND,
  HTTPD_405_METHOD_NOT_ALLOWED,
  HTTPD_408_REQ_TIMEOUT,
  HTTPD_411_LENGTH_REQUIRED,
  HTTPD_413_CONTENT_TOO_LARGE,
  HTTPD_414_URI_TOO_LONG,
  HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE,
  HTTPD_ERR_CODE_MAX
} httpd_err_code_t;

typedef esp_err_t (*httpd_err_handler_func_t)(httpd_req_t *req,
                                              httpd_err_code_t error);

#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3

#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_TYPE_JSON "application/json"
#define HTTPD_TYPE_TEXT "text/html"
#define HTTPD_TYPE_OCTET "application/octet-stream"

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle,
                                     const httpd_uri_t *uri_handler);
esp_err_t httpd_register_err_handler(httpd_handle_t handle,
                                     httpd_err_code_t error,
                                     httpd_err_handler_func_t handler_fn);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field,
                                      char *val, size_t val_size);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf,
                                      size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val,
                                size_t val_size);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field,
                             const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf,
                                ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error,
                              const char *msg);

static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str) {
  return httpd_resp_send(r, str, (str == NULL) ? 0 : HTTPD_RESP_USE_STRLEN);
}
