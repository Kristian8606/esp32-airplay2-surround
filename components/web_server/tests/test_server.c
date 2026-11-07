/* SPDX-License-Identifier: Apache-2.0 */

/* The web server run against the fake HTTP server and fake firmware: every
 * example and rule of spec/spec.md that a request can reach. Built twice:
 * without an admin password and with WEB_TEST_PASSWORD. */

#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "check.h"
#include "esp_system.h"
#include "fake.h"
#include "web_server.h"
#include "web_server_internal.h"

int check_failures;
int check_count;

extern uint8_t page_index_start[] asm("_binary_web_index_html_start");
extern uint8_t page_index_end[] asm("_binary_web_index_html_end");
extern uint8_t page_logs_start[] asm("_binary_web_logs_html_start");
extern uint8_t page_logs_end[] asm("_binary_web_logs_html_end");

static char auth_header[256] __attribute__((unused));

/* ---- Requests ---- */

static fake_request_t get(const char *uri) {
  fake_request_t r = {.method = HTTP_GET, .uri = uri};
  return r;
}

static fake_request_t post(const char *uri, const char *body) {
  fake_request_t r = {.method = HTTP_POST, .uri = uri, .body = body};
  r.body_len = r.content_len = body != NULL ? strlen(body) : 0;
#ifdef WEB_TEST_PASSWORD
  r.headers[r.nheaders++] = (fake_hdr_t){"Authorization", auth_header};
#endif
  return r;
}

static const fake_response_t *run(fake_request_t r) {
  static fake_request_t keep; /* the fake reads the request while it runs */
  keep = r;
  return fake_run(&keep);
}

static bool is_json(const fake_response_t *r, const char *want) {
  return strcmp(r->type, "application/json") == 0 &&
         strcmp(fake_body_str(r), want) == 0;
}

#define CHECK_JSON(resp, want)                                    \
  do {                                                            \
    const fake_response_t *r_ = (resp);                           \
    CHECK_STR(r_->status, "200 OK");                              \
    CHECK_STR(r_->type, "application/json");                      \
    CHECK_STR(fake_body_str(r_), want);                           \
    CHECK_EQ_INT(r_->sends, 1);                                   \
  } while (0)

/* ---- Test handlers, registered after the server started ---- */

static const uint8_t *page_start, *page_end;
static esp_err_t page_ret;
static esp_err_t test_page(httpd_req_t *req) {
  page_ret = web_serve_page(req, page_start, page_end);
  return page_ret;
}

static size_t recv_cap;
static esp_err_t recv_ret;
static char recv_text[1024];
static esp_err_t test_recv(httpd_req_t *req) {
  char *buf = malloc(recv_cap); /* exactly cap: ASan sees any overrun */
  recv_ret = web_recv_json(req, buf, recv_cap);
  recv_text[0] = '\0';
  if (recv_ret == ESP_OK) {
    snprintf(recv_text, sizeof(recv_text), "%s", buf);
  }
  free(buf);
  return ESP_OK; /* sends nothing */
}

static esp_err_t test_json_null(httpd_req_t *req) {
  web_send_json(req, NULL);
  return ESP_OK;
}

static bool admin_result;
static esp_err_t test_admin(httpd_req_t *req) {
  admin_result = web_admin_ok(req);
  return ESP_OK;
}

static bool pause_paused;
static esp_err_t pause_ret;
static esp_err_t test_pause(httpd_req_t *req) {
  pause_paused = true;
  pause_ret = web_audio_pause(req, "Latency test", &pause_paused);
  return ESP_OK;
}

static void register_test_handlers(void) {
  static const httpd_uri_t handlers[] = {
      {.uri = "/test/page", .method = HTTP_GET, .handler = test_page},
      {.uri = "/test/recv", .method = HTTP_POST, .handler = test_recv},
      {.uri = "/test/json_null", .method = HTTP_GET, .handler = test_json_null},
      {.uri = "/test/admin", .method = HTTP_GET, .handler = test_admin},
      {.uri = "/test/pause", .method = HTTP_GET, .handler = test_pause},
  };
  for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
    CHECK_EQ_INT(httpd_register_uri_handler(&fake_httpd, &handlers[i]), ESP_OK);
  }
}

/* ---- Start ---- */

static void test_start(void) {
  fake_reset();
  fake_fw.role_handlers = 12;
  fake_httpd.start_err = ESP_ERR_HTTPD_TASK;
  CHECK_EQ_INT(web_server_start(80), ESP_ERR_HTTPD_TASK);
  CHECK(!fake_called("web_role_register"));
  CHECK(strstr(fake_logs, "E web_server: httpd_start failed") != NULL);

  fake_reset();
  fake_fw.role_handlers = 12;
  fake_fw.log_stream_err = ESP_ERR_NO_MEM; /* logged, not returned */
  fake_httpd.start_err = ESP_OK;
  CHECK_EQ_INT(web_server_start(80), ESP_OK);

  const httpd_config_t *c = &fake_httpd.config;
  CHECK_EQ_INT(c->server_port, 80);
  CHECK_EQ_INT(c->task_priority, 3);
  CHECK_EQ_INT(c->stack_size, 8192);
  CHECK_EQ_INT(c->max_uri_handlers, 40);
  CHECK(c->lru_purge_enable);
  CHECK_EQ_INT(c->max_open_sockets, 7);
  CHECK(c->uri_match_fn == NULL);
  CHECK_EQ_INT(c->task_caps, FAKE_HTTPD_DEFAULT_TASK_CAPS);
  CHECK_EQ_INT(c->ctrl_port, 32768);

  /* Order: role handlers, shared handlers, 404, log stream. */
  CHECK(fake_in_order("httpd_start", "web_role_register"));
  CHECK(fake_in_order("register(/api/role/11)", "register(/)"));
  CHECK(fake_in_order("register(/connecttest.txt)", "register_err(6)"));
  CHECK(fake_in_order("register_err(6)", "log_stream_register"));
  CHECK(fake_in_order("log_stream_register", "register(/ws/logs)"));
  CHECK_EQ_INT(fake_httpd.nhandlers, 12 + 17 + 1);
  CHECK(fake_httpd.err_handlers[HTTPD_404_NOT_FOUND] != NULL);
  CHECK(strstr(fake_logs, "E web_server: log_stream_register failed "
                          "(ESP_ERR_NO_MEM)") != NULL);
  CHECK(strstr(fake_logs, "I web_server: Web UI started on port 80\n") != NULL);
  /* No shared handler failed to register. */
  CHECK(strstr(fake_logs, "Registering") == NULL);

  static const char *const shared[] = {
      "/", "/logs", "/speedtest", "/favicon.ico", "/api/system/info",
      "/api/wifi/scan", "/api/wifi/config", "/api/device/name",
      "/api/system/restart", "/api/ota/update", "/api/speedtest/ping",
      "/api/speedtest/download", "/api/speedtest/upload",
      "/hotspot-detect.html", "/library/test/success.html", "/generate_204",
      "/connecttest.txt",
  };
  static const int methods[] = {
      HTTP_GET, HTTP_GET, HTTP_GET, HTTP_GET, HTTP_GET, HTTP_GET, HTTP_POST,
      HTTP_POST, HTTP_POST, HTTP_POST, HTTP_GET, HTTP_GET, HTTP_POST,
      HTTP_GET, HTTP_GET, HTTP_GET, HTTP_GET,
  };
  for (size_t i = 0; i < 17; i++) {
    const httpd_uri_t *h = &fake_httpd.handlers[12 + i];
    CHECK_STR(h->uri, shared[i]);
    CHECK_EQ_INT(h->method, methods[i]);
    CHECK(!h->is_websocket);
  }

  fake_reset();
  CHECK_EQ_INT(web_server_start(80), ESP_ERR_INVALID_STATE);
  CHECK_EQ_INT(web_server_start(8080), ESP_ERR_INVALID_STATE);
  CHECK(!fake_called("httpd_start"));

  register_test_handlers();
}

/* ---- Pages ---- */

static void check_page(const char *uri, uint8_t *start, size_t len) {
  for (size_t i = 0; i < len; i++) {
    start[i] = (uint8_t)(i * 7 + 3);
  }
  const fake_response_t *r = run(get(uri));
  CHECK(r->found);
  CHECK_EQ_INT(r->handler_ret, ESP_OK);
  CHECK_STR(r->status, "200 OK");
  CHECK_STR(r->type, "text/html; charset=utf-8");
  CHECK_STR(fake_resp_hdr(r, "Cache-Control"), "no-store");
  CHECK_EQ_INT(r->body_len, len);
  CHECK(memcmp(r->body, start, len) == 0);
  CHECK_EQ_INT(r->max_chunk, 1024);
  CHECK_EQ_INT(r->chunks, (len + 1023) / 1024);
  CHECK(r->chunk_end);
  CHECK_EQ_INT(r->sends, 0);
  CHECK_EQ_INT(r->after_done, 0);
}

static void test_pages(void) {
  fake_reset();
  /* [board]: / is 20000 bytes */
  check_page("/", page_index_start, (size_t)(page_index_end - page_index_start));
  CHECK_EQ_INT(page_index_end - page_index_start, 20000);
  check_page("/logs", page_logs_start, (size_t)(page_logs_end - page_logs_start));
  CHECK_EQ_INT(page_logs_end - page_logs_start, 10000);

  const fake_response_t *r = run(get("/speedtest"));
  CHECK_EQ_INT(r->body_len, 1025);
  CHECK_EQ_INT(r->chunks, 2);
  CHECK(r->chunk_end);
  CHECK(r->body[0] == 'c' && r->body[1024] == 'c');
  CHECK_STR(r->type, "text/html; charset=utf-8");

  /* A query does not change the page. */
  r = run(get("/?x=1"));
  CHECK_EQ_INT(r->body_len, 20000);
}

static void test_serve_page(void) {
  fake_reset();
  static const size_t sizes[] = {1, 1023, 1024, 1025, 2048, 10000};
  for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    size_t n = sizes[s];
    uint8_t *buf = malloc(n); /* exactly n: ASan sees any overread */
    for (size_t i = 0; i < n; i++) {
      buf[i] = (uint8_t)(i ^ (i >> 8));
    }
    page_start = buf;
    page_end = buf + n;
    const fake_response_t *r = run(get("/test/page"));
    CHECK_EQ_INT(page_ret, ESP_OK);
    CHECK_EQ_INT(r->body_len, n);
    CHECK(memcmp(r->body, buf, n) == 0);
    CHECK_EQ_INT(r->chunks, (n + 1023) / 1024);
    CHECK(r->max_chunk <= 1024);
    CHECK(r->chunk_end);
    free(buf);
  }
}

static void test_serve_page_edges(void) {
  fake_reset();
  static const uint8_t one[1] = {0};
  page_start = one;
  page_end = one;
  const fake_response_t *r = run(get("/test/page"));
  CHECK_EQ_INT(page_ret, ESP_OK);
  CHECK_EQ_INT(r->body_len, 0);
  CHECK_EQ_INT(r->chunks, 0);
  CHECK(r->chunk_end);
  CHECK_STR(r->type, "text/html; charset=utf-8");

  page_start = NULL;
  page_end = NULL;
  r = run(get("/test/page"));
  CHECK_EQ_INT(page_ret, ESP_OK);
  CHECK_EQ_INT(r->body_len, 0);
  CHECK(r->chunk_end);

  page_start = one + 1; /* end before start */
  page_end = one;
  r = run(get("/test/page"));
  CHECK_EQ_INT(r->body_len, 0);
  CHECK(r->chunk_end);

  /* A send error is returned; the page is not finished. */
  uint8_t *buf = calloc(1, 10000);
  page_start = buf;
  page_end = buf + 10000;
  fake_request_t q = get("/test/page");
  q.fail_chunk_from = 3;
  r = run(q);
  CHECK_EQ_INT(page_ret, ESP_ERR_HTTPD_RESP_SEND);
  CHECK_EQ_INT(r->handler_ret, ESP_ERR_HTTPD_RESP_SEND);
  CHECK_EQ_INT(r->chunks, 2);
  CHECK(!r->chunk_end);
  free(buf);

  /* The shared page handler returns the send error too. */
  q = get("/logs");
  q.fail_chunk_from = 1;
  r = run(q);
  CHECK(r->handler_ret != ESP_OK);
  CHECK(!r->chunk_end);
}

static void test_favicon(void) {
  fake_reset();
  const fake_response_t *r = run(get("/favicon.ico"));
  CHECK_STR(r->status, "204 No Content");
  CHECK_EQ_INT(r->body_len, 0);
  CHECK_EQ_INT(r->sends, 1);
  CHECK_EQ_INT(r->handler_ret, ESP_OK);
}

/* ---- System info ---- */

static void set_board_ap(void) {
  wifi_ap_record_t ap;
  memset(&ap, 0, sizeof(ap));
  memcpy(ap.ssid, "UniFi", 6);
  const uint8_t bssid[6] = {0x8a, 0x78, 0x48, 0x7e, 0xd0, 0x97};
  memcpy(ap.bssid, bssid, 6);
  ap.rssi = -35;
  ap.primary = 11;
  ap.phy_11b = 1;
  ap.phy_11g = 1;
  ap.phy_11n = 1;
  fake_fw.ap_info = ap;
}

static void test_system_info(void) {
  fake_reset();
  set_board_ap();
  /* The example of the spec [black box, board]. */
  CHECK_JSON(
      run(get("/api/system/info")),
      "{\"info\":{\"ip\":\"192.168.1.98\",\"mac\":\"14:C1:9F:27:BE:C8\","
      "\"device_name\":\"ESP32 AirPlay EC8\",\"wifi_connected\":true,"
      "\"role\":\"gateway\",\"free_heap\":2956072,\"wifi_ssid\":\"UniFi\","
      "\"wifi_bssid\":\"8a:78:48:7e:d0:97\",\"wifi_rssi\":-35,"
      "\"wifi_channel\":11,\"wifi_phy\":\"11n\","
      "\"firmware_version\":\"v4.5.61\","
      "\"firmware_build\":\"v4.5.61 29a5ab1d\","
      "\"reset_reason\":\"software\",\"uptime_s\":1013},\"success\":true}");

  /* Not associated, IP and name failing, another role and reset reason. */
  fake_reset();
  fake_fw.ap_info_err = ESP_ERR_WIFI_NOT_CONNECT;
  fake_fw.ip_err = ESP_FAIL;
  fake_fw.get_name_err = ESP_ERR_NOT_FOUND;
  fake_fw.connected = false;
  fake_fw.role = "speaker";
  fake_fw.reset_reason = ESP_RST_USB;
  fake_fw.now_us = 0;
  CHECK_JSON(run(get("/api/system/info")),
             "{\"info\":{\"ip\":\"\",\"mac\":\"14:C1:9F:27:BE:C8\","
             "\"device_name\":\"\",\"wifi_connected\":false,"
             "\"role\":\"speaker\",\"free_heap\":2956072,"
             "\"firmware_version\":\"v4.5.61\","
             "\"firmware_build\":\"v4.5.61 29a5ab1d\","
             "\"reset_reason\":\"unknown\",\"uptime_s\":0},\"success\":true}");

  /* Full buffers without NULs are cut, never read past. */
  fake_reset();
  fake_fw.name_fill_no_nul = true;
  memset(fake_fw.app_version, 'V', sizeof(fake_fw.app_version));
  wifi_ap_record_t ap;
  memset(&ap, 'S', sizeof(ap));
  ap.rssi = -90;
  ap.primary = 1;
  ap.phy_11n = 0;
  ap.phy_11g = 1;
  ap.phy_11b = 0;
  fake_fw.ap_info = ap;
  fake_fw.reset_reason = ESP_RST_BROWNOUT;
  const fake_response_t *r = run(get("/api/system/info"));
  cJSON *j = cJSON_Parse(fake_body_str(r));
  cJSON *info = cJSON_GetObjectItemCaseSensitive(j, "info");
  CHECK_EQ_INT(strlen(cJSON_GetObjectItemCaseSensitive(info, "device_name")
                          ->valuestring),
               64);
  CHECK_EQ_INT(strlen(cJSON_GetObjectItemCaseSensitive(info, "wifi_ssid")
                          ->valuestring),
               32);
  CHECK_EQ_INT(strlen(cJSON_GetObjectItemCaseSensitive(info,
                                                       "firmware_version")
                          ->valuestring),
               32);
  CHECK_STR(cJSON_GetObjectItemCaseSensitive(info, "wifi_phy")->valuestring,
            "11g");
  CHECK_STR(cJSON_GetObjectItemCaseSensitive(info, "wifi_bssid")->valuestring,
            "53:53:53:53:53:53");
  CHECK_STR(cJSON_GetObjectItemCaseSensitive(info, "reset_reason")->valuestring,
            "brownout");
  cJSON_Delete(j);

  /* PHY "11b" and "?" */
  fake_reset();
  set_board_ap();
  fake_fw.ap_info.phy_11n = 0;
  fake_fw.ap_info.phy_11g = 0;
  r = run(get("/api/system/info"));
  CHECK(strstr(fake_body_str(r), "\"wifi_phy\":\"11b\"") != NULL);
  fake_fw.ap_info.phy_11b = 0;
  r = run(get("/api/system/info"));
  CHECK(strstr(fake_body_str(r), "\"wifi_phy\":\"?\"") != NULL);
}

/* ---- Wi-Fi scan ---- */

static void set_scan(uint16_t count) {
  free(fake_fw.scan_records);
  fake_fw.scan_records = calloc(count > 0 ? count : 1, sizeof(wifi_ap_record_t));
  for (uint16_t i = 0; i < count; i++) {
    wifi_ap_record_t *a = &fake_fw.scan_records[i];
    snprintf((char *)a->ssid, sizeof(a->ssid), "net%u", (unsigned)i);
    a->rssi = (int8_t)(-30 - i);
    a->primary = (uint8_t)(1 + i % 13);
  }
  fake_fw.scan_count = count;
}

static void test_scan_plain(void) {
  /* Audio engine not initialised: no pause. */
  fake_reset();
  set_scan(0);
  CHECK_JSON(run(get("/api/wifi/scan")), "{\"networks\":[],\"success\":true}");
  CHECK(!fake_called("source_stop"));
  CHECK(!fake_called("audio_init"));

  fake_reset();
  set_scan(1);
  memcpy(fake_fw.scan_records[0].ssid, "HomeNet", 8);
  fake_fw.scan_records[0].rssi = -40;
  fake_fw.scan_records[0].primary = 1;
  CHECK_JSON(run(get("/api/wifi/scan")),
             "{\"networks\":[{\"ssid\":\"HomeNet\",\"rssi\":-40,"
             "\"channel\":1}],\"success\":true}");

  /* 40 networks, in scan order, an empty SSID among them. */
  fake_reset();
  set_scan(40);
  fake_fw.scan_records[7].ssid[0] = '\0';
  memset(fake_fw.scan_records[8].ssid, 'X', 33);
  const fake_response_t *r = run(get("/api/wifi/scan"));
  CHECK_STR(r->type, "application/json");
  cJSON *j = cJSON_Parse(fake_body_str(r));
  cJSON *nets = cJSON_GetObjectItemCaseSensitive(j, "networks");
  CHECK_EQ_INT(cJSON_GetArraySize(nets), 40);
  for (int i = 0; i < 40 && i < cJSON_GetArraySize(nets); i++) {
    cJSON *n = cJSON_GetArrayItem(nets, i);
    char want[40];
    snprintf(want, sizeof(want), "net%d", i);
    const char *ssid = cJSON_GetObjectItemCaseSensitive(n, "ssid")->valuestring;
    if (i == 7) {
      CHECK_STR(ssid, "");
    } else if (i == 8) {
      CHECK_EQ_INT(strlen(ssid), 32);
    } else {
      CHECK_STR(ssid, want);
    }
    CHECK_EQ_INT(cJSON_GetObjectItemCaseSensitive(n, "rssi")->valueint, -30 - i);
    CHECK_EQ_INT(cJSON_GetObjectItemCaseSensitive(n, "channel")->valueint,
                 1 + i % 13);
  }
  CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "success")));
  cJSON_Delete(j);

  /* An error. */
  fake_reset();
  fake_fw.scan_err = ESP_ERR_WIFI_NOT_STARTED;
  CHECK_JSON(run(get("/api/wifi/scan")),
             "{\"success\":false,\"error\":\"ESP_ERR_WIFI_NOT_STARTED\"}");

  /* ESP_OK with no list. */
  fake_reset();
  fake_fw.scan_null_list = true;
  fake_fw.scan_count = 5;
  CHECK_JSON(run(get("/api/wifi/scan")), "{\"networks\":[],\"success\":true}");
}

static void test_scan_paused(void) {
  fake_reset();
  set_scan(2);
  fake_fw.audio_initialized = true;
  fake_fw.source_idle_after_ms = 120;
  int64_t t0 = fake_now_ms();
  const fake_response_t *r = run(get("/api/wifi/scan"));
  CHECK(is_json(r, "{\"networks\":[{\"ssid\":\"net0\",\"rssi\":-30,"
                   "\"channel\":1},{\"ssid\":\"net1\",\"rssi\":-31,"
                   "\"channel\":2}],\"success\":true}"));
  CHECK(fake_in_order("source_stop", "audio_release"));
  CHECK(fake_in_order("audio_release", "wifi_scan"));
  CHECK(fake_in_order("wifi_scan", "audio_init"));
  CHECK(fake_in_order("audio_init", "source_start"));
  CHECK(fake_in_order("source_start", "send(200 OK)"));
  CHECK_EQ_INT(fake_now_ms() - t0, 150); /* polled every 50 ms */
  CHECK_EQ_INT(fake_fw.restarts, 0);

  /* An error while paused: resumed, then the error. */
  fake_reset();
  fake_fw.audio_initialized = true;
  fake_fw.scan_err = ESP_ERR_TIMEOUT;
  r = run(get("/api/wifi/scan"));
  CHECK(is_json(r, "{\"success\":false,\"error\":\"ESP_ERR_TIMEOUT\"}"));
  CHECK(fake_in_order("wifi_scan", "source_start"));

  /* Stopping the source fails: logged, waits as usual. */
  fake_reset();
  set_scan(0);
  fake_fw.audio_initialized = true;
  fake_fw.source_stop_err = ESP_FAIL;
  r = run(get("/api/wifi/scan"));
  CHECK(is_json(r, "{\"networks\":[],\"success\":true}"));
  CHECK(strstr(fake_logs, "Wi-Fi scan: stopping the audio source") != NULL);
}

static void test_scan_source_stuck(void) {
  /* A source that does not stop. */
  fake_reset();
  fake_fw.audio_initialized = true;
  fake_fw.source_idle_after_ms = -1;
  int64_t t0 = fake_now_ms();
  const fake_response_t *r = run(get("/api/wifi/scan"));
  CHECK_STR(r->status, "503 Service Unavailable");
  CHECK_STR(r->type, "text/plain");
  CHECK_STR(fake_body_str(r),
            "AirPlay clients are still stopping; recovering service");
  CHECK_EQ_INT(r->sends, 1);
  CHECK_EQ_INT(r->after_done, 0);
  CHECK(!fake_called("wifi_scan"));
  CHECK(!fake_called("audio_release"));
  CHECK(fake_in_order("send(503 Service Unavailable)", "delay(250)"));
  CHECK(fake_in_order("delay(250)", "restart"));
  CHECK_EQ_INT(fake_fw.restarts, 1);
  CHECK_EQ_INT(fake_now_ms() - t0, 30000 + 250);
  CHECK(strstr(fake_logs,
               "E web_server: Wi-Fi scan: audio source did not stop; "
               "restarting") != NULL);

  /* Idle just at 30 s still counts. */
  fake_reset();
  set_scan(0);
  fake_fw.audio_initialized = true;
  fake_fw.source_idle_after_ms = 30000;
  r = run(get("/api/wifi/scan"));
  CHECK(is_json(r, "{\"networks\":[],\"success\":true}"));
  CHECK_EQ_INT(fake_fw.restarts, 0);
}

static void test_scan_release_and_resume_errors(void) {
  /* Releasing the audio memory fails. */
  fake_reset();
  fake_fw.audio_initialized = true;
  fake_fw.audio_release_err = ESP_ERR_NO_MEM;
  const fake_response_t *r = run(get("/api/wifi/scan"));
  CHECK_STR(r->status, "500 Internal Server Error");
  CHECK_STR(fake_body_str(r), "Wi-Fi scan: audio memory not released");
  CHECK(fake_in_order("audio_release", "source_start"));
  CHECK(fake_in_order("source_start", "send(500"));
  CHECK(!fake_called("wifi_scan"));
  CHECK_EQ_INT(fake_fw.restarts, 0);
  CHECK_EQ_INT(r->sends, 1);

  /* Restoring the engine fails: 500, restart, no JSON. */
  fake_reset();
  set_scan(3);
  fake_fw.audio_initialized = true;
  fake_fw.audio_init_err = ESP_ERR_NO_MEM;
  r = run(get("/api/wifi/scan"));
  CHECK_STR(r->status, "500 Internal Server Error");
  CHECK_STR(fake_body_str(r), "Audio restore failed; restarting");
  CHECK_EQ_INT(r->sends, 1);
  CHECK_EQ_INT(r->after_done, 0);
  CHECK(fake_in_order("send(500 Internal Server Error)", "delay(250)"));
  CHECK(fake_in_order("delay(250)", "restart"));
  CHECK(strstr(fake_logs, "E web_server: Wi-Fi scan: audio restore failed "
                          "(ESP_ERR_NO_MEM); restarting") != NULL);

  /* Restarting the source fails. */
  fake_reset();
  set_scan(3);
  fake_fw.audio_initialized = true;
  fake_fw.source_start_err = ESP_FAIL;
  r = run(get("/api/wifi/scan"));
  CHECK_STR(fake_body_str(r), "Audio restore failed; restarting");
  CHECK(fake_in_order("audio_init", "source_start"));
  CHECK_EQ_INT(fake_fw.restarts, 1);
}

static void test_audio_pause_helper(void) {
  fake_reset();
  const fake_response_t *r = run(get("/test/pause"));
  CHECK_EQ_INT(pause_ret, ESP_OK);
  CHECK(!pause_paused);
  CHECK_EQ_INT(r->sends + r->chunks, 0);
  CHECK(!fake_called("source_stop"));

  fake_reset();
  fake_fw.audio_initialized = true;
  r = run(get("/test/pause"));
  CHECK_EQ_INT(pause_ret, ESP_OK);
  CHECK(pause_paused);
  CHECK_EQ_INT(r->sends + r->chunks, 0);
  CHECK(!fake_fw.audio_initialized);

  fake_reset();
  fake_fw.audio_initialized = true;
  fake_fw.source_idle_after_ms = -1;
  r = run(get("/test/pause"));
  CHECK_EQ_INT(pause_ret, ESP_FAIL);
  CHECK(!pause_paused);
  CHECK(strstr(fake_logs, "Latency test: audio source did not stop") != NULL);
}

/* ---- Admin ---- */

static void test_admin_check(void) {
  fake_reset();
#ifndef WEB_TEST_PASSWORD
  fake_request_t q = get("/test/admin");
  const fake_response_t *r = run(q);
  CHECK(admin_result);
  CHECK_EQ_INT(r->sends, 0);
  q.headers[q.nheaders++] = (fake_hdr_t){"Authorization", "Basic !!!"};
  r = run(q);
  CHECK(admin_result);
  CHECK_EQ_INT(r->sends, 0);
#else
  static const char *const bad[] = {
      NULL,
      "",
      "Basic",
      "Basic ",
      "Bearer YWRtaW46czNjcmV0",
      "Basic YWRtaW46czNjcmV",
      "Basic YWRtaW46czNj!mV0",
      "Basic YWRtaW46czNjcmV4",     /* admin:s3crex */
      "Basic cm9vdDpzM2NyZXQ=",     /* root:s3cret */
      "Basic QWRtaW46czNjcmV0",     /* Admin:s3cret */
      "Basic YWRtaW46czNjcmV0MQ==", /* admin:s3cret1 */
      "Basic YWRtaW46",             /* admin: */
      "Digest username=\"admin\"",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    fake_request_t q = get("/test/admin");
    if (bad[i] != NULL) {
      q.headers[q.nheaders++] = (fake_hdr_t){"Authorization", bad[i]};
    }
    const fake_response_t *r = run(q);
    CHECK(!admin_result);
    CHECK_STR(r->status, "401 Unauthorized");
    CHECK_STR(fake_resp_hdr(r, "WWW-Authenticate"),
              "Basic realm=\"AirPlay ESP32\"");
    CHECK_STR(fake_body_str(r), "Authentication required\n");
    CHECK_EQ_INT(r->sends, 1);
  }
  /* A header longer than any valid one. */
  char *big = malloc(5000);
  memset(big, 'A', 4999);
  big[4999] = '\0';
  memcpy(big, "Basic ", 6);
  fake_request_t q = get("/test/admin");
  q.headers[q.nheaders++] = (fake_hdr_t){"Authorization", big};
  const fake_response_t *r = run(q);
  CHECK(!admin_result);
  CHECK_STR(r->status, "401 Unauthorized");
  free(big);

  q = get("/test/admin");
  q.headers[q.nheaders++] = (fake_hdr_t){"authorization", auth_header};
  r = run(q);
  CHECK(admin_result);
  CHECK_EQ_INT(r->sends, 0);
#endif
}

/* Every admin endpoint refuses before doing anything. */
static void test_admin_endpoints(void) {
#ifdef WEB_TEST_PASSWORD
  static const char *const uris[] = {
      "/api/wifi/config", "/api/device/name", "/api/system/restart",
      "/api/ota/update",
  };
  for (size_t i = 0; i < 4; i++) {
    fake_reset();
    fake_request_t q = post(uris[i], "{\"ssid\":\"x\",\"name\":\"y\"}");
    q.nheaders = 0;
    const fake_response_t *r = run(q);
    CHECK_STR(r->status, "401 Unauthorized");
    CHECK_EQ_INT(r->handler_ret, ESP_OK);
    CHECK_EQ_INT(r->recv_calls, 0);
    CHECK(!fake_called("settings_"));
    CHECK(!fake_called("source_stop"));
    CHECK(!fake_called("ota_start"));
    CHECK_EQ_INT(fake_fw.restarts, 0);
  }
#endif
}

/* ---- Wi-Fi config ---- */

static void test_wifi_config(void) {
  fake_reset();
  const fake_response_t *r = run(
      post("/api/wifi/config", "{\"ssid\":\"HomeNet\",\"password\":\"pw123\"}"));
  CHECK_JSON(r, "{\"success\":true}");
  CHECK_STR(fake_fw.set_ssid, "HomeNet");
  CHECK_STR(fake_fw.set_password, "pw123");
  CHECK(fake_in_order("settings_set_wifi_credentials(HomeNet,pw123)",
                      "send(200 OK)"));
  CHECK(fake_in_order("send(200 OK)", "delay(500)"));
  CHECK(fake_in_order("delay(500)", "restart"));
  CHECK_EQ_INT(fake_fw.restarts, 1);

  /* password missing or not a string: "" */
  fake_reset();
  r = run(post("/api/wifi/config", "{\"ssid\":\"Open\"}"));
  CHECK_JSON(r, "{\"success\":true}");
  CHECK(fake_called("settings_set_wifi_credentials(Open,)"));
  fake_reset();
  r = run(post("/api/wifi/config", "{\"ssid\":\"Open\",\"password\":42}"));
  CHECK(fake_called("settings_set_wifi_credentials(Open,)"));

  /* Invalid SSID */
  static const char *const bad[] = {
      "{}",
      "{\"ssid\":42}",
      "{\"ssid\":null}",
      "{\"ssid\":[\"a\"]}",
      "{\"SSID\":\"a\"}",
      "hello",
      "[\"ssid\"]",
      "\"ssid\"",
      "{\"ssid\":\"a\"",
      "{\"ssid\":\"a\",}",
      "\xff\xfe",
      "",
      " ",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    fake_reset();
    fake_request_t q = post("/api/wifi/config", bad[i]);
    if (q.content_len == 0) {
      /* "" would be refused as 400; send it as a body of one NUL */
      static const char nul[1] = {0};
      q.body = nul;
      q.body_len = q.content_len = 1;
    }
    r = run(q);
    CHECK_JSON(r, "{\"success\":false,\"error\":\"Invalid SSID\"}");
    CHECK(!fake_called("settings_set_wifi_credentials"));
    CHECK_EQ_INT(fake_fw.restarts, 0);
  }

  /* Answer 7: data after the first complete JSON value is ignored. */
  static const char *const trailing[] = {
      "{\"ssid\":\"Tail\"} trailing",
      "{\"ssid\":\"Tail\"}}",
      "{\"ssid\":\"Tail\"}{\"ssid\":\"Other\"}",
      "{\"ssid\":\"Tail\"}\n\xff\xfe garbage",
  };
  for (size_t i = 0; i < sizeof(trailing) / sizeof(trailing[0]); i++) {
    fake_reset();
    r = run(post("/api/wifi/config", trailing[i]));
    CHECK_JSON(r, "{\"success\":true}");
    CHECK(fake_called("settings_set_wifi_credentials(Tail,)"));
    CHECK_EQ_INT(fake_fw.restarts, 1);
  }
  /* ...but the first value must be complete and valid. */
  fake_reset();
  r = run(post("/api/wifi/config", "garbage {\"ssid\":\"Tail\"}"));
  CHECK_JSON(r, "{\"success\":false,\"error\":\"Invalid SSID\"}");

  /* settings refuse */
  fake_reset();
  fake_fw.set_wifi_err = ESP_ERR_INVALID_ARG;
  r = run(post("/api/wifi/config", "{\"ssid\":\"x\",\"password\":\"short\"}"));
  CHECK_JSON(r, "{\"success\":false,\"error\":\"ESP_ERR_INVALID_ARG\"}");
  CHECK_EQ_INT(fake_fw.restarts, 0);
}

/* A {"ssid":"aaa…"} body of exactly len bytes. */
static char *ssid_body(size_t len) {
  char *b = malloc(len + 1);
  memset(b, 'a', len);
  memcpy(b, "{\"ssid\":\"", 9);
  b[len - 2] = '"';
  b[len - 1] = '}';
  b[len] = '\0';
  return b;
}

static void check_invalid_body(const fake_response_t *r) {
  CHECK_STR(r->status, "400 Bad Request");
  CHECK_STR(fake_body_str(r), "Invalid body");
  CHECK_EQ_INT(r->sends, 1);
  CHECK(!fake_called("settings_"));
  CHECK_EQ_INT(fake_fw.restarts, 0);
}

static void test_wifi_config_body_sizes(void) {
  fake_reset();
  check_invalid_body(run(post("/api/wifi/config", "")));

  fake_reset();
  char *b = ssid_body(511);
  const fake_response_t *r = run(post("/api/wifi/config", b));
  CHECK_JSON(r, "{\"success\":true}");
  CHECK_EQ_INT(strlen(fake_fw.set_ssid), 511 - 11);
  free(b);

  fake_reset();
  b = ssid_body(512);
  r = run(post("/api/wifi/config", b));
  check_invalid_body(r);
  CHECK_EQ_INT(r->recv_calls, 0); /* nothing read */
  free(b);

  /* Shorter than content_len: the peer closes early. */
  fake_reset();
  fake_request_t q = post("/api/wifi/config", "{\"ssid\":\"x\"}");
  q.content_len = 100;
  check_invalid_body(run(q));

  /* Receive timeouts: two are tried again, the third fails. */
  fake_reset();
  q = post("/api/wifi/config", "{\"ssid\":\"x\"}");
  q.recv_script[0] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script[1] = 3;
  q.recv_script[2] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script_len = 3;
  CHECK_JSON(run(q), "{\"success\":true}");

  fake_reset();
  q = post("/api/wifi/config", "{\"ssid\":\"x\"}");
  q.recv_script[0] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script[1] = 2;
  q.recv_script[2] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script[3] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script_len = 4;
  r = run(q);
  check_invalid_body(r);
  CHECK_EQ_INT(r->recv_calls, 4);

  fake_reset();
  q = post("/api/wifi/config", "{\"ssid\":\"x\"}");
  q.recv_script[0] = HTTPD_SOCK_ERR_FAIL;
  q.recv_script_len = 1;
  check_invalid_body(run(q));

  /* Many short reads. */
  fake_reset();
  q = post("/api/wifi/config", "{\"ssid\":\"Slow\",\"password\":\"abc\"}");
  q.recv_limit = 1;
  CHECK_JSON(run(q), "{\"success\":true}");
  CHECK_STR(fake_fw.set_ssid, "Slow");
}

/* ---- Device name ---- */

static void test_device_name(void) {
  fake_reset();
  const fake_response_t *r =
      run(post("/api/device/name", "{\"name\":\"Kitchen\"}"));
  CHECK_JSON(r, "{\"success\":true}");
  CHECK(fake_in_order("settings_set_device_name(Kitchen)",
                      "wifi_set_hostname(Kitchen)"));
  CHECK(fake_in_order("wifi_set_hostname(Kitchen)", "send(200 OK)"));
  CHECK_EQ_INT(fake_fw.restarts, 0);

  fake_reset();
  fake_fw.set_name_err = ESP_ERR_INVALID_SIZE;
  r = run(post("/api/device/name", "{\"name\":\"Kitchen\"}"));
  CHECK_JSON(r, "{\"success\":false,\"error\":\"ESP_ERR_INVALID_SIZE\"}");
  CHECK(!fake_called("wifi_set_hostname"));

  static const char *const bad[] = {
      "{}", "{\"name\":1}", "{\"name\":true}", "{\"Name\":\"x\"}", "x",
      "{\"name\":\"x\",}", "{\"name\":{\"a\":1}}",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    fake_reset();
    r = run(post("/api/device/name", bad[i]));
    CHECK_JSON(r, "{\"success\":false,\"error\":\"Invalid name\"}");
    CHECK(!fake_called("settings_set_device_name"));
  }

  /* Answer 7: data after the first complete JSON value is ignored. */
  fake_reset();
  r = run(post("/api/device/name", "{\"name\":\"Den\"} {\"name\":\"Other\"}"));
  CHECK_JSON(r, "{\"success\":true}");
  CHECK(fake_called("settings_set_device_name(Den)"));
  CHECK(fake_called("wifi_set_hostname(Den)"));

  /* UTF-8 and escapes reach settings decoded. */
  fake_reset();
  r = run(post("/api/device/name", "{\"name\":\"K\\u00fcche \\\"1\\\"\"}"));
  CHECK_JSON(r, "{\"success\":true}");
  CHECK_STR(fake_fw.set_name, "K\xc3\xbc" "che \"1\"");

  /* Sizes: 0, 255, 256 */
  fake_reset();
  check_invalid_body(run(post("/api/device/name", "")));

  char body[300];
  memset(body, 'n', sizeof(body));
  memcpy(body, "{\"name\":\"", 9);
  body[253] = '"';
  body[254] = '}';
  body[255] = '\0';
  fake_reset();
  r = run(post("/api/device/name", body));
  CHECK_JSON(r, "{\"success\":true}");
  CHECK_EQ_INT(strlen(fake_fw.set_name), 255 - 11);

  memset(body, 'n', sizeof(body));
  memcpy(body, "{\"name\":\"", 9);
  body[254] = '"';
  body[255] = '}';
  body[256] = '\0';
  fake_reset();
  r = run(post("/api/device/name", body));
  check_invalid_body(r);
  CHECK_EQ_INT(r->recv_calls, 0);

  /* A NUL inside the body ends the JSON text. */
  fake_reset();
  fake_request_t q = post("/api/device/name", "{\"name\":\"a\"}");
  static const char with_nul[] = "{\"name\":\"a\"}\0garbage";
  q.body = with_nul;
  q.body_len = q.content_len = sizeof(with_nul) - 1;
  r = run(q);
  CHECK_JSON(r, "{\"success\":true}");
}

/* ---- Restart and OTA ---- */

static void test_restart(void) {
  fake_reset();
  const fake_response_t *r = run(post("/api/system/restart", NULL));
  CHECK_STR(r->status, "200 OK");
  CHECK_STR(r->type, "text/html"); /* no type set: the server's default */
  CHECK_STR(fake_body_str(r), "Restarting\n");
  CHECK(fake_in_order("send(200 OK)", "delay(200)"));
  CHECK(fake_in_order("delay(200)", "restart"));
  CHECK_EQ_INT(fake_fw.restarts, 1);
}

static void test_ota(void) {
  static char image[4000];
  memset(image, 0xE9, sizeof(image));

  fake_reset();
  fake_fw.ota_read = sizeof(image);
  fake_request_t q = post("/api/ota/update", NULL);
  q.body = image;
  q.body_len = q.content_len = sizeof(image);
  const fake_response_t *r = run(q);
  CHECK_STR(r->status, "200 OK");
  CHECK_STR(fake_body_str(r), "Firmware update complete, rebooting now!\n");
  CHECK_EQ_INT(r->handler_ret, ESP_OK);
  CHECK(fake_in_order("source_stop", "ota_start"));
  CHECK(fake_in_order("ota_start", "send(200 OK)"));
  CHECK(fake_in_order("send(200 OK)", "delay(500)"));
  CHECK(fake_in_order("delay(500)", "restart"));
  CHECK(!fake_called("source_start"));
  CHECK_EQ_INT(r->recv_bytes, sizeof(image));

  fake_reset();
  fake_fw.ota_err = ESP_ERR_OTA_VALIDATE_FAILED;
  r = run(q);
  CHECK_STR(r->status, "500 Internal Server Error");
  CHECK_STR(fake_body_str(r), "ESP_ERR_OTA_VALIDATE_FAILED");
  CHECK_EQ_INT(r->handler_ret, ESP_FAIL);
  CHECK(fake_in_order("ota_start", "source_start"));
  CHECK(fake_in_order("source_start", "send(500"));
  CHECK_EQ_INT(fake_fw.restarts, 0);
  CHECK(strstr(fake_logs, "E web_server: OTA failed "
                          "(ESP_ERR_OTA_VALIDATE_FAILED)") != NULL);

  /* Stopping the source fails: logged, the update goes on. */
  fake_reset();
  fake_fw.source_stop_err = ESP_ERR_INVALID_STATE;
  r = run(q);
  CHECK(fake_called("ota_start"));
  CHECK_STR(fake_body_str(r), "Firmware update complete, rebooting now!\n");
  CHECK(strstr(fake_logs, "OTA: stopping the audio source") != NULL);
}

/* ---- Speed test ---- */

static void test_speedtest_ping(void) {
  fake_reset();
  const fake_response_t *r = run(get("/api/speedtest/ping"));
  CHECK_STR(r->type, "text/plain");
  CHECK_STR(fake_resp_hdr(r, "Cache-Control"), "no-store");
  CHECK_STR(fake_body_str(r), "ok");
}

static void check_download(const char *uri, size_t want) {
  fake_reset();
  const fake_response_t *r = run(get(uri));
  CHECK_EQ_INT(r->handler_ret, ESP_OK);
  CHECK_STR(r->type, "application/octet-stream");
  CHECK_STR(fake_resp_hdr(r, "Cache-Control"), "no-store");
  CHECK_EQ_INT(r->body_len, want);
  CHECK(r->body_all_zero);
  CHECK(r->max_chunk <= 2048);
  CHECK_EQ_INT(r->chunks, (want + 2047) / 2048);
  CHECK(r->chunk_end);
}

static void test_speedtest_download(void) {
  check_download("/api/speedtest/download", 1048576);
  check_download("/api/speedtest/download?bytes=1048576", 1048576);
  check_download("/api/speedtest/download?bytes=999999999&_=1731000000000",
                 16777216);
  check_download("/api/speedtest/download?_=17&bytes=2049", 2049);
  check_download("/api/speedtest/download?bytes=1", 1);
  check_download("/api/speedtest/download?bytes=0", 1048576);
  check_download("/api/speedtest/download?bytes=-1", 1048576);
  check_download("/api/speedtest/download?bytes=abc", 1048576);
  check_download("/api/speedtest/download?bytes=", 1048576);
  check_download("/api/speedtest/download?bytes", 1048576);
  check_download("/api/speedtest/download?bytesx=5", 1048576);
  check_download("/api/speedtest/download?", 1048576);
  check_download(
      "/api/speedtest/download?bytes=99999999999999999999999999999999999999",
      16777216);

  fake_reset();
  fake_request_t q = get("/api/speedtest/download?bytes=10000");
  q.fail_chunk_from = 2;
  const fake_response_t *r = run(q);
  CHECK_EQ_INT(r->handler_ret, ESP_FAIL);
  CHECK(!r->chunk_end);
}

static void test_speedtest_upload(void) {
  static char data[5000];
  memset(data, 'u', sizeof(data));

  fake_reset();
  fake_request_t q = {.method = HTTP_POST, .uri = "/api/speedtest/upload",
                      .body = data, .body_len = sizeof(data),
                      .content_len = sizeof(data), .recv_limit = 300};
  const fake_response_t *r = run(q);
  CHECK_STR(r->type, "text/plain");
  CHECK_STR(fake_body_str(r), "received=5000");
  CHECK_EQ_INT(r->recv_bytes, 5000);

  fake_reset();
  q.body_len = q.content_len = 0;
  r = run(q);
  CHECK_STR(fake_body_str(r), "received=0");

  /* The peer closes early: no answer. */
  fake_reset();
  q.body_len = 1000;
  q.content_len = 5000;
  r = run(q);
  CHECK_EQ_INT(r->handler_ret, ESP_FAIL);
  CHECK_EQ_INT(r->sends, 0);

  fake_reset();
  q.body_len = q.content_len = 5000;
  q.recv_script[0] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script[1] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script_len = 2;
  r = run(q);
  CHECK_STR(fake_body_str(r), "received=5000");

  fake_reset();
  q.recv_script[2] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script_len = 3;
  r = run(q);
  CHECK_EQ_INT(r->handler_ret, ESP_FAIL);
  CHECK_EQ_INT(r->sends, 0);
}

/* ---- Captive portal and 404 ---- */

static void check_redirect(const fake_response_t *r) {
  CHECK_STR(r->status, "302 Found");
  CHECK_STR(fake_resp_hdr(r, "Location"), "http://192.168.4.1/");
  CHECK_EQ_INT(r->body_len, 0);
  CHECK_EQ_INT(r->sends, 1);
  CHECK_EQ_INT(r->handler_ret, ESP_OK);
}

static void test_captive(void) {
  static const char *const uris[] = {
      "/hotspot-detect.html", "/library/test/success.html", "/generate_204",
      "/connecttest.txt",
  };
  for (int connected = 0; connected <= 1; connected++) {
    for (size_t i = 0; i < 4; i++) {
      fake_reset();
      fake_fw.connected = connected;
      const fake_response_t *r = run(get(uris[i]));
      CHECK(r->found);
      check_redirect(r);
    }
  }

  fake_reset();
  fake_fw.connected = true;
  const fake_response_t *r = run(get("/nothing/here"));
  CHECK(!r->found);
  CHECK_STR(r->status, "404 Not Found");
  CHECK_STR(r->type, "text/plain");
  CHECK_STR(fake_body_str(r), "Not Found");
  CHECK_EQ_INT(r->handler_ret, ESP_OK);

  fake_reset();
  fake_fw.connected = false;
  r = run(get("/nothing/here"));
  CHECK(!r->found);
  check_redirect(r);

  fake_reset();
  fake_fw.connected = false;
  fake_request_t q = {.method = HTTP_POST, .uri = "/some/post"};
  check_redirect(run(q));
}

/* ---- Other helpers ---- */

static void test_recv_json_helper(void) {
  struct {
    size_t cap, len, body_len;
    esp_err_t want;
  } cases[] = {
      {256, 0, 0, ESP_ERR_INVALID_SIZE},   {256, 255, 255, ESP_OK},
      {256, 256, 256, ESP_ERR_INVALID_SIZE}, {512, 511, 511, ESP_OK},
      {512, 512, 512, ESP_ERR_INVALID_SIZE}, {512, 1000, 1000, ESP_ERR_INVALID_SIZE},
      {512, 300, 299, ESP_FAIL},           {512, 300, 0, ESP_FAIL},
      {1, 1, 1, ESP_ERR_INVALID_SIZE},     {2, 1, 1, ESP_OK},
  };
  static char body[1001];
  memset(body, 'j', sizeof(body) - 1);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    fake_reset();
    recv_cap = cases[i].cap;
    fake_request_t q = {.method = HTTP_POST, .uri = "/test/recv", .body = body,
                        .body_len = cases[i].body_len,
                        .content_len = cases[i].len, .recv_limit = 64};
    const fake_response_t *r = run(q);
    CHECK_EQ_INT(recv_ret, cases[i].want);
    CHECK_EQ_INT(r->sends + r->chunks + r->send_err_calls, 0); /* no answer */
    if (cases[i].want == ESP_ERR_INVALID_SIZE) {
      CHECK_EQ_INT(r->recv_calls, 0);
    }
    if (cases[i].want == ESP_OK) {
      CHECK_EQ_INT(strlen(recv_text), cases[i].len);
      CHECK_EQ_INT(r->recv_bytes, cases[i].len);
    }
  }

  /* Timeouts. */
  fake_reset();
  recv_cap = 64;
  fake_request_t q = {.method = HTTP_POST, .uri = "/test/recv", .body = "{}",
                      .body_len = 2, .content_len = 2};
  q.recv_script[0] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script[1] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script_len = 2;
  run(q);
  CHECK_EQ_INT(recv_ret, ESP_OK);
  CHECK_STR(recv_text, "{}");

  fake_reset();
  q.recv_script[2] = HTTPD_SOCK_ERR_TIMEOUT;
  q.recv_script_len = 3;
  const fake_response_t *r = run(q);
  CHECK_EQ_INT(recv_ret, ESP_FAIL);
  CHECK_EQ_INT(r->recv_calls, 3);
  CHECK_EQ_INT(r->sends, 0);

  fake_reset();
  q.recv_script[0] = HTTPD_SOCK_ERR_INVALID;
  q.recv_script_len = 1;
  run(q);
  CHECK_EQ_INT(recv_ret, ESP_FAIL);
}

static void test_send_json_null(void) {
  fake_reset();
  CHECK_JSON(run(get("/test/json_null")), "{}");
}

static void test_log_memory(void) {
  fake_reset();
  fake_fw.heap_internal_free = 300 * 1024 + 1023;
  fake_fw.heap_internal_largest = 100 * 1024;
  fake_fw.heap_psram_free = 7000 * 1024;
  fake_fw.heap_psram_largest = 4096 * 1024;
  web_log_memory("scan");
  CHECK_STR(fake_logs,
            "I web_server: MEM scan internalFree=300 internalLargest=100 "
            "psramFree=7000 psramLargest=4096\n");
  CHECK(fake_called("heap_free(0x804)"));
  CHECK(fake_called("heap_largest(0x804)"));
  CHECK(fake_called("heap_free(0x400)"));
  CHECK(fake_called("heap_largest(0x400)"));
}

int main(void) {
#ifdef WEB_TEST_PASSWORD
  const char *plain = "admin:" WEB_TEST_PASSWORD;
  char *token = b64_encode((const unsigned char *)plain, strlen(plain));
  snprintf(auth_header, sizeof(auth_header), "Basic %s", token);
  free(token);
  printf("admin password: set\n");
#else
  printf("admin password: none\n");
#endif
  RUN(test_start);
  RUN(test_pages);
  RUN(test_serve_page);
  RUN(test_serve_page_edges);
  RUN(test_favicon);
  RUN(test_system_info);
  RUN(test_scan_plain);
  RUN(test_scan_paused);
  RUN(test_scan_source_stuck);
  RUN(test_scan_release_and_resume_errors);
  RUN(test_audio_pause_helper);
  RUN(test_admin_check);
  RUN(test_admin_endpoints);
  RUN(test_wifi_config);
  RUN(test_wifi_config_body_sizes);
  RUN(test_device_name);
  RUN(test_restart);
  RUN(test_ota);
  RUN(test_speedtest_ping);
  RUN(test_speedtest_download);
  RUN(test_speedtest_upload);
  RUN(test_captive);
  RUN(test_recv_json_helper);
  RUN(test_send_json_null);
  RUN(test_log_memory);
  fake_reset();
  CHECK_MAIN_END();
}
