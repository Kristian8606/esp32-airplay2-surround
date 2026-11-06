/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* What the request rules (rtsp_core.c) need from the platform. On the
 * Gateway rtsp_server.c implements these with FreeRTOS and lwIP; the host
 * tests give fakes. */

typedef struct rtsp_conn rtsp_conn_t;

/* Buffers for bodies and connection state (PSRAM on the Gateway). */
void *rtsp_port_malloc(size_t size);
void rtsp_port_free(void *ptr);

/* The server lock. Recursive: the rules take it, and remote and status
 * calls take it too. */
void rtsp_port_lock(void);
void rtsp_port_unlock(void);

/* A millisecond clock for the remote command timeout. */
uint32_t rtsp_port_now_ms(void);

/* settings.h, each call run where a flash access is allowed (the
 * connection tasks have their stacks in PSRAM). */
esp_err_t rtsp_port_settings_get_volume(float *volume);
esp_err_t rtsp_port_settings_set_volume(float volume);
esp_err_t rtsp_port_settings_persist_volume(void);
esp_err_t rtsp_port_settings_get_name(char *name, size_t len);

/* The station MAC ("deviceid" of /info). */
void rtsp_port_device_mac(uint8_t mac[6]);

/* The event port of a connection's session SETUP. open: take a free TCP
 * port and accept on it at once (*port is its number); the event channel
 * uses these keys (Gateway to sender, sender to Gateway) and calls
 * rtsp_evconn_new(conn, ...) for each sender that connects. close: stop
 * accepting, close the event connection, give the port back. */
esp_err_t rtsp_port_event_open(rtsp_conn_t *conn, const uint8_t write_key[32],
                               const uint8_t read_key[32], uint16_t *port);
void rtsp_port_event_close(rtsp_conn_t *conn);

/* Close another control connection (take-over). Returns at once; the
 * connection's task ends it and calls rtsp_conn_end(). */
void rtsp_port_conn_close(rtsp_conn_t *conn);

/* A free UDP port pair for a realtime stream. */
esp_err_t rtsp_port_udp_pair(uint16_t *data_port, uint16_t *control_port);
