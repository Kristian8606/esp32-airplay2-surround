/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtsp_port.h"

/* The request rules: one object per control connection, the session state
 * shared by all of them, and the event channel's remote commands. No
 * ESP-IDF: the platform is reached through rtsp_port.h. */

/* Sends bytes on the connection's socket. 0 when all were sent. */
typedef int (*rtsp_send_fn)(void *ctx, const uint8_t *data, size_t len);

/* ---- Control connections ---- */

/* peer_ip: the sender's IPv4 address in network byte order. port_ctx is the
 * platform's own pointer for this connection. NULL when out of memory. */
rtsp_conn_t *rtsp_conn_new(uint32_t peer_ip, void *port_ctx);
void *rtsp_conn_port_ctx(const rtsp_conn_t *conn);

/* Bytes received on the connection. Replies go out through send. Returns 0
 * to keep the connection, -1 to close it. */
int rtsp_conn_receive(rtsp_conn_t *conn, const uint8_t *data, size_t len,
                      rtsp_send_fn send, void *ctx);

/* The connection was closed (by the sender, an error, or a close request):
 * ends its session as TEARDOWN does, gives back its event port, frees it. */
void rtsp_conn_end(rtsp_conn_t *conn);

/* True when no session is held. */
bool rtsp_core_idle(void);

/* ---- Event connections ---- */

typedef struct rtsp_evconn rtsp_evconn_t;

/* A sender connected to the event port of conn (from its session SETUP).
 * It replaces an earlier event connection on that port. Remote commands go
 * only to the event connection of the session holder. NULL when out of
 * memory. */
rtsp_evconn_t *rtsp_evconn_new(const rtsp_conn_t *conn,
                               const uint8_t write_key[32],
                               const uint8_t read_key[32]);
/* Bytes from the sender. -1: close the event connection. */
int rtsp_evconn_receive(rtsp_evconn_t *ev, const uint8_t *data, size_t len);
/* Sends a queued command, checks the 3 s timeout (the connection stays
 * open after it). -1: close the event connection (send error, replaced, or
 * its port closed). */
int rtsp_evconn_poll(rtsp_evconn_t *ev, uint32_t now_ms, rtsp_send_fn send,
                     void *ctx);
/* The event connection closed. A command still going ends disconnected. */
void rtsp_evconn_free(rtsp_evconn_t *ev);

/* The remote command timeout. */
#define RTSP_REMOTE_TIMEOUT_MS 3000U

