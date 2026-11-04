/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake ESP-IDF for running src/log_stream.c on a computer: web server,
 * logging, heap and FreeRTOS, single-threaded with simulated time. Work
 * queued with httpd_queue_work runs at once, in the "web server task".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

#define FAKE_FD_BASE 54
#define FAKE_SOCKS   16

typedef struct {
    bool open;
    bool ws;
    void *ctx;
    httpd_free_ctx_fn_t free_fn;
    uint8_t *rx; /* all frames received, joined */
    size_t rx_len, rx_cap;
    size_t frames;
    bool frames_ok;     /* every frame: text, <= 4 KiB, valid UTF-8 alone */
    bool fail_send;     /* the next sends to it fail */
    uint32_t opened_ms;
    int64_t first_ms;   /* time of the first frame, -1 none */
} fake_sock_t;

extern fake_sock_t fake_socks[FAKE_SOCKS];
extern uint32_t fake_now_ms;
extern httpd_handle_t fake_server;

/* what the UART got */
extern uint8_t *fake_uart;
extern size_t fake_uart_len;

/* counters and switches */
extern int fake_queue_work_calls;
extern int fake_trigger_close_calls;
extern int fake_register_calls;
extern esp_err_t fake_register_err;
extern bool fake_task_create_fail;
extern int fake_task_deleted;
extern int fake_alloc_fail_at;     /* n-th heap_caps_malloc from now fails; 0 none */
extern size_t fake_internal_bytes; /* allocated without MALLOC_CAP_SPIRAM */
extern bool fake_stack_in_psram;
extern bool fake_log_on_send;      /* the web server logs during each send */
extern bool fake_ctx_not_kept;     /* the session context set in the
                                    * post-handshake callback is not kept */
extern int fake_timed_waits;       /* module task sleeps with a timeout */
extern int fake_errors;            /* misuse seen by the fake */

void fake_reset(void);
fake_sock_t *fake_sock(int fd);
int fake_ws_open(void);   /* a browser upgrades (post-handshake callback runs);
                          * returns fd */
int fake_http_open(void); /* a plain HTTP connection */
void fake_close(int fd);  /* the web server deletes the session */
/* The browser sends one data frame of len bytes; returns the handler's result
 * (the fake closes the socket if it is not ESP_OK, like the web server). */
esp_err_t fake_browser_sends(int fd, size_t len);
/* Runs fn at simulated time at_ms, while the module's task sleeps. */
void fake_at(uint32_t at_ms, void (*fn)(void));
/* One iteration of the module's task. */
void fake_step(void);
/* Runs steps for ms of simulated time. */
void fake_run_ms(uint32_t ms);
/* Logs as an application task, or as the web server outside module work. */
void fake_log(const char *fmt, ...);
void fake_log_as_httpd(const char *fmt, ...);
