/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Control and records of the host fakes: the HTTP server, ESP-IDF and the
 * firmware functions. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_wifi_types.h"

/* ---- Call log: every fake call in order, "name(args);" ---- */
extern char fake_calls[65536];
void fake_call(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool fake_called(const char *needle);
/* Whether the needles appear in this order in the call log. */
bool fake_in_order(const char *first, const char *second);

/* ---- Log lines ---- */
extern char fake_logs[65536];

/* ---- HTTP server ---- */
#define FAKE_MAX_HANDLERS 64
#define FAKE_MAX_HDRS 8
#define FAKE_BODY_KEEP (64 * 1024)

typedef struct {
  const char *name;
  const char *value;
} fake_hdr_t;

typedef struct {
  /* request */
  int method;
  const char *uri; /* with the query */
  fake_hdr_t headers[FAKE_MAX_HDRS];
  size_t nheaders;
  const char *body;     /* what the peer sends */
  size_t body_len;      /* may be shorter than content_len */
  size_t content_len;
  /* httpd_req_recv(): script entries first (>0: at most that many bytes,
   * <=0: returned as is), then at most recv_limit bytes per call (0: no
   * limit); after body_len bytes, 0 (peer closed). */
  int recv_script[16];
  size_t recv_script_len;
  size_t recv_limit;
  /* httpd_resp_send_chunk() fails from this chunk on (0: never). */
  int fail_chunk_from;
} fake_request_t;

typedef struct {
  bool found;             /* a URI handler ran (else the error handler) */
  esp_err_t handler_ret;
  char status[64];
  char type[64];
  struct {
    char name[64];
    char value[128];
  } hdrs[FAKE_MAX_HDRS];
  size_t nhdrs;
  uint8_t *body;          /* first FAKE_BODY_KEEP bytes */
  size_t body_len;        /* all bytes */
  bool body_all_zero;
  int sends;              /* httpd_resp_send() calls */
  int chunks;             /* non-empty chunks */
  size_t max_chunk;
  bool chunk_end;         /* the empty chunk was sent */
  int after_done;         /* sends after the response was complete */
  int send_err_calls;
  size_t recv_calls;
  size_t recv_bytes;
} fake_response_t;

typedef struct {
  esp_err_t start_err;
  bool started;
  httpd_config_t config;
  httpd_uri_t handlers[FAKE_MAX_HANDLERS];
  size_t nhandlers;
  httpd_err_handler_func_t err_handlers[HTTPD_ERR_CODE_MAX];
} fake_httpd_t;

extern fake_httpd_t fake_httpd;

/* Runs a request through the registered handlers and returns the recorded
 * response (valid until the next request). */
const fake_response_t *fake_run(const fake_request_t *rq);
const char *fake_resp_hdr(const fake_response_t *r, const char *name);
/* The body as a NUL-terminated string (first FAKE_BODY_KEEP bytes). */
const char *fake_body_str(const fake_response_t *r);

/* ---- ESP-IDF and firmware state ---- */
typedef struct {
  /* time and system */
  int64_t now_us;
  uint32_t free_heap;
  int reset_reason;
  char app_version[32]; /* not NUL-terminated when full */
  size_t heap_internal_free, heap_internal_largest;
  size_t heap_psram_free, heap_psram_largest;
  int restarts;
  /* Wi-Fi */
  const char *ip;
  esp_err_t ip_err;
  const char *mac;
  bool connected;
  esp_err_t ap_info_err;
  wifi_ap_record_t ap_info;
  esp_err_t scan_err;
  wifi_ap_record_t *scan_records;
  uint16_t scan_count;
  bool scan_null_list; /* ESP_OK with a NULL list */
  char hostname[128];
  /* settings */
  const char *device_name;
  esp_err_t get_name_err;
  bool name_fill_no_nul; /* fills the whole buffer without a NUL */
  esp_err_t set_name_err;
  char set_name[512];
  esp_err_t set_wifi_err;
  char set_ssid[512];
  char set_password[512];
  /* firmware */
  const char *role;
  const char *firmware_id;
  esp_err_t source_stop_err;
  int64_t source_idle_after_ms; /* <0: never */
  int64_t source_stop_at_ms;
  esp_err_t source_start_err;
  bool audio_initialized;
  esp_err_t audio_init_err;
  esp_err_t audio_release_err;
  esp_err_t ota_err;
  size_t ota_read; /* bytes the fake OTA reads from the body */
  esp_err_t log_stream_err;
  int role_handlers; /* registered by web_role_register() */
} fake_fw_t;

extern fake_fw_t fake_fw;

/* Clears the call log, log lines and the firmware state (not the
 * registered handlers). */
void fake_reset(void);
int64_t fake_now_ms(void);
