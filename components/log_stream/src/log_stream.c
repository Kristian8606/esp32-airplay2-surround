/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF side of log_stream: the logging hook, the browser list and the
 * sending. The history and text logic is in ls_core.c.
 *
 * Who runs what:
 *  - any task, in the logging call: ls_vprintf. Prints to the UART through
 *    the previous hook, formats into a free scratch buffer and copies the
 *    text into the history under s_ring_mux. Nothing else.
 *  - the web server task: on_upgrade (the post-handshake callback of
 *    /ws/logs: a new browser), sess_free (a socket closed: forget it),
 *    ws_handler (frames from browsers are read and dropped) and send_work
 *    (one frame per browser). The web server API is not thread-safe, so
 *    every call into it and every change of the browser list is made here.
 *  - the module's task (stack in PSRAM): sleeps while no browser is known
 *    (on_upgrade wakes it); otherwise checks every LS_POLL_MS whether a
 *    browser has text to get, and then queues send_work and waits for it.
 *    It calls nothing of the web server but httpd_queue_work.
 *
 * In ESP-IDF v6.1 the WebSocket handler is not called for the handshake,
 * hence the post-handshake callback; the firmware must set
 * CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y.
 */
#include "log_stream.h"

#include "sdkconfig.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "ls_core.h"

#ifndef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "log_stream needs CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y in sdkconfig"
#endif

#define LS_URI         "/ws/logs"
#define LS_SLOTS       4      /* formatting buffers for concurrent loggers */
#define LS_CLIENTS     7      /* the web server has at most 7 sockets */
#define LS_POLL_MS     50     /* latency budget is 200 ms */
#define LS_DRAIN_SIZE  512    /* longest frame a browser may send */
#define LS_TASK_STACK  4096
#define LS_TASK_PRIO   2
#define LS_DONE_WAIT_MS 60000 /* send work not done by then: give up on it */

#define LS_BIT_WAKE 1u
#define LS_BIT_DONE 2u

static const char *TAG = "log_stream";

typedef struct {
    bool active;
    int fd;
    uint32_t gen;
    ls_reader_t rd;
} ls_client_t;

static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_cli_mux = portMUX_INITIALIZER_UNLOCKED;

static ls_ring_t s_ring;
static uint8_t *s_slots;                  /* LS_SLOTS * (LS_MAX_OUTPUT + 1) */
static volatile uint32_t s_slot_busy;     /* bit i: slot i in use */
static vprintf_like_t s_prev;
static volatile bool s_capturing;

static httpd_handle_t s_server;
static TaskHandle_t s_task;
/* Task running send_work right now; its logs are not captured. */
static volatile TaskHandle_t s_send_task;
static ls_client_t s_clients[LS_CLIENTS];
static uint32_t s_next_gen;
static int s_rr;                          /* round-robin start */
static bool s_sent_any;
static uint8_t *s_raw;                    /* LS_FRAME_MAX */
static uint8_t *s_frame;                  /* LS_FRAME_MAX */
static uint8_t *s_drain;                  /* LS_DRAIN_SIZE */

/* ---- capture ---------------------------------------------------------- */

static void ring_lock(void *arg)
{
    (void)arg;
    portENTER_CRITICAL_SAFE(&s_ring_mux);
}

static void ring_unlock(void *arg)
{
    (void)arg;
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
}

static const ls_lock_t s_ring_lock = { ring_lock, ring_unlock, NULL };

static int slot_take(void)
{
    for (int i = 0; i < LS_SLOTS; i++) {
        uint32_t bit = 1u << i;
        if (!(__atomic_fetch_or(&s_slot_busy, bit, __ATOMIC_ACQUIRE) & bit)) {
            return i;
        }
    }
    return -1;
}

static void slot_give(int i)
{
    __atomic_fetch_and(&s_slot_busy, ~(1u << i), __ATOMIC_RELEASE);
}

static bool in_send_context(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    return self != NULL && (self == s_task || self == s_send_task);
}

static int ls_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    vprintf_like_t prev = s_prev ? s_prev : vprintf;
    int ret = prev(fmt, ap);

    if (s_capturing && !in_send_context()) {
        int slot = slot_take();
        if (slot >= 0) {
            char *buf = (char *)s_slots + slot * (LS_MAX_OUTPUT + 1);
            int n = vsnprintf(buf, LS_MAX_OUTPUT + 1, fmt, copy);
            if (n > 0) {
                ring_lock(NULL);
                ls_capture(&s_ring, (const uint8_t *)buf, (size_t)n);
                ring_unlock(NULL);
            }
            slot_give(slot);
        } else {
            /* no buffer free: the output is kept as one '?' (spec, Capture) */
            ring_lock(NULL);
            ls_ring_write(&s_ring, (const uint8_t *)"?", 1);
            ring_unlock(NULL);
        }
    }
    va_end(copy);
    return ret;
}

esp_err_t log_stream_init(void)
{
    if (s_capturing) {
        return ESP_OK;
    }
    uint8_t *hist = heap_caps_malloc(LS_HISTORY_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *slots = heap_caps_malloc(LS_SLOTS * (LS_MAX_OUTPUT + 1),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hist || !slots) {
        heap_caps_free(hist);
        heap_caps_free(slots);
        return ESP_ERR_NO_MEM;
    }
    ls_ring_init(&s_ring, hist, LS_HISTORY_SIZE);
    s_slots = slots;
    s_capturing = true;
    s_prev = esp_log_set_vprintf(ls_vprintf);
    return ESP_OK;
}

/* ---- browsers ---------------------------------------------------------- */

/* The session context handed to the web server is not a pointer: it encodes
 * slot and generation so that a late close of a forgotten socket cannot
 * remove a newer browser in the same slot. Never 0. */
static void *ctx_encode(int idx, uint32_t gen)
{
    return (void *)(uintptr_t)(((gen & 0x0FFFFFFFu) << 4) | (uint32_t)(idx + 1));
}

static void forget(int idx)
{
    portENTER_CRITICAL(&s_cli_mux);
    s_clients[idx].active = false;
    portEXIT_CRITICAL(&s_cli_mux);
}

static void forget_fd(int fd)
{
    portENTER_CRITICAL(&s_cli_mux);
    for (int i = 0; i < LS_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            s_clients[i].active = false;
        }
    }
    portEXIT_CRITICAL(&s_cli_mux);
}

static void sess_free(void *ctx)
{
    uintptr_t v = (uintptr_t)ctx;
    int idx = (int)(v & 0xF) - 1;
    uint32_t gen = (uint32_t)(v >> 4);
    if (idx < 0 || idx >= LS_CLIENTS) {
        return;
    }
    portENTER_CRITICAL(&s_cli_mux);
    if (s_clients[idx].active && (s_clients[idx].gen & 0x0FFFFFFFu) == gen) {
        s_clients[idx].active = false;
    }
    portEXIT_CRITICAL(&s_cli_mux);
}

/* Web server task: forgets browsers whose socket is no longer a WebSocket
 * (in case the web server did not free their session context). */
static void forget_closed(void)
{
    for (int i = 0; i < LS_CLIENTS; i++) {
        portENTER_CRITICAL(&s_cli_mux);
        bool active = s_clients[i].active;
        int fd = s_clients[i].fd;
        portEXIT_CRITICAL(&s_cli_mux);
        if (active && httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            forget(i);
        }
    }
}

/* Post-handshake callback of /ws/logs, web server task: a new browser,
 * starting at the whole history. Failing refuses it: the web server closes
 * the connection right after the upgrade. */
static esp_err_t on_upgrade(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    forget_closed();
    forget_fd(fd); /* an old browser on a socket with the same number */

    ls_reader_t rd;
    ls_reader_start(&rd, &s_ring, &s_ring_lock);
    int idx = -1;
    portENTER_CRITICAL(&s_cli_mux);
    for (int i = 0; i < LS_CLIENTS && idx < 0; i++) {
        if (!s_clients[i].active) {
            idx = i;
        }
    }
    if (idx >= 0) {
        s_clients[idx].fd = fd;
        s_clients[idx].gen = ++s_next_gen;
        s_clients[idx].rd = rd;
        s_clients[idx].active = true;
    }
    portEXIT_CRITICAL(&s_cli_mux);
    if (idx < 0) {
        return ESP_FAIL; /* 8th browser */
    }
    req->sess_ctx = ctx_encode(idx, s_clients[idx].gen);
    req->free_ctx = sess_free;
    xTaskNotify(s_task, LS_BIT_WAKE, eSetBits);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK; /* not called for the handshake in v6.1 */
    }

    /* Read whatever the browser sent and drop it. The page never sends
     * data, so a long frame ends the connection: returning an error makes
     * the web server close the socket. */
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof(f));
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK || f.len == 0) {
        return err;
    }
    if (f.len > LS_DRAIN_SIZE) {
        forget_fd(httpd_req_to_sockfd(req));
        return ESP_FAIL;
    }
    f.payload = s_drain;
    return httpd_ws_recv_frame(req, &f, f.len);
}

/* ---- sending ----------------------------------------------------------- */

/* Runs in the web server task: at most one frame per browser. */
static void send_work(void *arg)
{
    (void)arg;
    s_send_task = xTaskGetCurrentTaskHandle();
    s_sent_any = false;

    for (int k = 0; k < LS_CLIENTS; k++) {
        int idx = (s_rr + k) % LS_CLIENTS;
        portENTER_CRITICAL(&s_cli_mux);
        ls_client_t c = s_clients[idx];
        portEXIT_CRITICAL(&s_cli_mux);
        if (!c.active) {
            continue;
        }
        if (httpd_ws_get_fd_info(s_server, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            forget(idx);
            continue;
        }
        ls_reader_t rd = c.rd;
        size_t len = ls_read_frame(&s_ring, &s_ring_lock, &rd, s_raw, s_frame, LS_FRAME_MAX);
        if (len > 0) {
            httpd_ws_frame_t f;
            memset(&f, 0, sizeof(f));
            f.type = HTTPD_WS_TYPE_TEXT;
            f.final = true;
            f.payload = s_frame;
            f.len = len;
            if (httpd_ws_send_frame_async(s_server, c.fd, &f) != ESP_OK) {
                forget(idx);
                httpd_sess_trigger_close(s_server, c.fd);
                continue;
            }
            s_sent_any = true;
        }
        /* also without a frame: skipped leftover bytes move the reader */
        portENTER_CRITICAL(&s_cli_mux);
        if (s_clients[idx].active && s_clients[idx].gen == c.gen) {
            s_clients[idx].rd = rd;
        }
        portEXIT_CRITICAL(&s_cli_mux);
    }
    s_rr = (s_rr + 1) % LS_CLIENTS;

    s_send_task = NULL;
    xTaskNotify(s_task, LS_BIT_DONE, eSetBits);
}

/* 0: no browser, 1: browsers but nothing new for them, 2: something to send. */
static int client_state(void)
{
    ring_lock(NULL);
    uint64_t head = s_ring.head;
    ring_unlock(NULL);

    int state = 0;
    portENTER_CRITICAL(&s_cli_mux);
    for (int i = 0; i < LS_CLIENTS; i++) {
        if (s_clients[i].active) {
            state = 1;
            if (s_clients[i].rd.pos != head) {
                state = 2;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_cli_mux);
    return state;
}

static void wait_done(void)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(LS_DONE_WAIT_MS);
    uint32_t bits = 0;
    while (!(bits & LS_BIT_DONE)) {
        TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= limit) {
            return; /* web server gone or stuck; try again later */
        }
        xTaskNotifyWait(0, UINT32_MAX, &bits, limit - spent);
    }
}

static void sender_step(void)
{
    uint32_t bits;
    int state = client_state();
    if (state == 0) {
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY); /* until a browser comes */
        return;
    }
    if (state == 1) {
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(LS_POLL_MS));
        return;
    }
    if (httpd_queue_work(s_server, send_work, NULL) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    wait_done();
    if (!s_sent_any) {
        /* e.g. only an incomplete character is left: wait for more text */
        vTaskDelay(pdMS_TO_TICKS(LS_POLL_MS));
    }
}

static void sender_task(void *arg)
{
    (void)arg;
    for (;;) {
        sender_step();
    }
}

esp_err_t log_stream_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_capturing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server != NULL) {
        return s_server == server ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    uint8_t *bufs = heap_caps_malloc(2 * LS_FRAME_MAX + LS_DRAIN_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bufs) {
        return ESP_ERR_NO_MEM;
    }
    s_raw = bufs;
    s_frame = bufs + LS_FRAME_MAX;
    s_drain = bufs + 2 * LS_FRAME_MAX;
    s_server = server;

    if (xTaskCreateWithCaps(sender_task, "log_stream", LS_TASK_STACK, NULL, LS_TASK_PRIO,
                            &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_server = NULL;
        heap_caps_free(bufs);
        return ESP_ERR_NO_MEM;
    }

    httpd_uri_t uri = {
        .uri = LS_URI,
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .ws_post_handshake_cb = on_upgrade,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        vTaskDeleteWithCaps(s_task);
        s_task = NULL;
        s_server = NULL;
        heap_caps_free(bufs);
        return err;
    }
    ESP_LOGI(TAG, "Log streaming on %s", LS_URI);
    return ESP_OK;
}
