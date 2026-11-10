/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* What the request rules (rtsp_core.c) need from the platform:
 * rtsp_server.c implements these with FreeRTOS and lwIP. */

typedef struct rtsp_conn rtsp_conn_t;

/* Buffers for bodies and connection state (PSRAM on the Gateway). */
void *rtsp_port_malloc(size_t size);
void rtsp_port_free(void *ptr);

/* The server lock. Recursive: the rules take it, and remote and status
 * calls take it too. */
void rtsp_port_lock(void);
void rtsp_port_unlock(void);

/* Resource snapshot for the bounded Pair Verify diagnostic (no allocation). */
typedef struct {
  uint32_t internal_free, internal_largest;
  uint32_t psram_free, psram_largest;
} rtsp_pairing_memory_t;
void rtsp_port_pairing_memory(rtsp_pairing_memory_t *out);

/* A millisecond clock for the remote command timeout. */
uint32_t rtsp_port_now_ms(void);

/* settings.h, each call run where a flash access is allowed (the
 * connection tasks have their stacks in PSRAM). */
esp_err_t rtsp_port_settings_get_volume(float *volume);
esp_err_t rtsp_port_settings_set_volume(float volume);
esp_err_t rtsp_port_settings_persist_volume(void);
esp_err_t rtsp_port_settings_get_name(char *name, size_t len);
/* Saves the name from an internal-stack settings worker. */
esp_err_t rtsp_port_settings_set_name(const char *name);
/* Announces the saved name: host name and both mDNS instance names. Call
 * without the lock; it waits for the mDNS task. */
esp_err_t rtsp_port_settings_announce_name(void);

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

#ifdef CONFIG_AIRPLAY_HOMEKIT
/* Notify the preallocated internal-stack reset worker after the reply. */
void rtsp_port_homekit_reset(void);
/* Last effective name, cached before streaming; no NVS worker allocation. */
esp_err_t rtsp_port_homekit_reset_name(char *name, size_t len);
#endif

/* A free UDP port pair for a realtime stream. */
esp_err_t rtsp_port_udp_pair(uint16_t *data_port, uint16_t *control_port);
