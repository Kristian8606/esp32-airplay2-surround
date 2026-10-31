#pragma once

/* Between the shared web server (web_server.c) and each firmware's own pages
 * and APIs (main/web_gateway.c, speaker/main/web_speaker.c). */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

/* ---- Helpers of the shared web server ---- */

/* A page embedded in the firmware image, sent in chunks. */
esp_err_t web_serve_page(httpd_req_t *req, const uint8_t *start,
                         const uint8_t *end);
/* A JSON body of at most cap - 1 bytes, NUL-terminated. */
esp_err_t web_recv_json(httpd_req_t *req, char *buf, size_t cap);
/* Sends r and frees it. */
void web_send_json(httpd_req_t *req, cJSON *r);
/* The admin password check (menuconfig); on false a 401 has been sent. */
bool web_admin_ok(httpd_req_t *req);
void web_log_memory(const char *where);

/* Maintenance that needs the audio memory (Wi-Fi scan, latency test): stop
 * the audio source and release the engine. *paused is false when nothing
 * was running. Not ESP_OK: a response has been sent; return from the
 * handler. */
esp_err_t web_audio_pause(httpd_req_t *req, const char *what, bool *paused);
/* Undo web_audio_pause(). Not ESP_OK: a response has been sent and the ESP
 * reboots. */
esp_err_t web_audio_resume(httpd_req_t *req, const char *what);

/* ---- Implemented by each firmware ---- */

/* "gateway" or "speaker", for the page (/api/system/info). */
const char *web_role_name(void);
/* Its own pages and APIs. */
void web_role_register(httpd_handle_t server);
/* What feeds the audio engine (the Gateway's RTSP server, a speaker's link
 * to the Gateway), stopped before maintenance or OTA and started again. */
esp_err_t web_role_source_stop(void);
bool web_role_source_idle(void);
esp_err_t web_role_source_start(void);
