/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_fake.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

void log_stream_host_step(void);

#define TASK_APP    ((TaskHandle_t)0x100)
#define TASK_HTTPD  ((TaskHandle_t)0x200)
#define TASK_MODULE ((TaskHandle_t)0x300)

fake_sock_t fake_socks[FAKE_SOCKS];
uint32_t fake_now_ms;
static int s_server_obj;
httpd_handle_t fake_server = &s_server_obj;
uint8_t *fake_uart;
size_t fake_uart_len;
static size_t s_uart_cap;

int fake_queue_work_calls;
int fake_trigger_close_calls;
int fake_register_calls;
esp_err_t fake_register_err;
bool fake_task_create_fail;
int fake_task_deleted;
int fake_alloc_fail_at;
size_t fake_internal_bytes;
bool fake_stack_in_psram;
bool fake_log_on_send;
bool fake_ctx_not_kept;
int fake_timed_waits;
int fake_errors;

static TaskHandle_t s_current = TASK_APP;
static uint32_t s_notify_bits;
static int s_crit_depth;
static vprintf_like_t s_hook;

static void misuse(const char *what);

/* things that happen while the module's task sleeps */
typedef struct {
    uint32_t at;
    void (*fn)(void);
} fake_event_t;
static fake_event_t s_events[8];
static int s_n_events;

static void advance(uint32_t ticks)
{
    uint32_t end = fake_now_ms + ticks;
    for (;;) {
        int next = -1;
        for (int i = 0; i < s_n_events; i++) {
            if (s_events[i].at <= end && (next < 0 || s_events[i].at < s_events[next].at)) {
                next = i;
            }
        }
        if (next < 0) {
            break;
        }
        fake_event_t ev = s_events[next];
        s_events[next] = s_events[--s_n_events];
        if (ev.at > fake_now_ms) {
            fake_now_ms = ev.at;
        }
        ev.fn();
    }
    fake_now_ms = end;
}

void fake_at(uint32_t at_ms, void (*fn)(void))
{
    if (s_n_events == 8) {
        misuse("too many events");
        return;
    }
    s_events[s_n_events].at = at_ms;
    s_events[s_n_events].fn = fn;
    s_n_events++;
}
static esp_err_t (*s_handler)(httpd_req_t *r);
static esp_err_t (*s_post_handshake)(httpd_req_t *r);
static bool s_blocked; /* the module's task waits without a timeout */

static void misuse(const char *what)
{
    fake_errors++;
    fprintf(stderr, "fake: %s\n", what);
}

static void check_not_critical(const char *what)
{
    if (s_crit_depth != 0) {
        misuse(what);
    }
}

/* ---- UART and logging -------------------------------------------------- */

static void add(uint8_t **buf, size_t *len, size_t *cap, const void *p, size_t n)
{
    if (*len + n > *cap) {
        *cap = (*len + n) * 2 + 64;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, p, n);
    *len += n;
}

static int uart_vprintf(const char *fmt, va_list ap)
{
    char tmp[8192];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n > 0) {
        add(&fake_uart, &fake_uart_len, &s_uart_cap, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
    return n;
}

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    vprintf_like_t old = s_hook;
    s_hook = func;
    return old;
}

static void log_v(const char *fmt, va_list ap)
{
    s_hook(fmt, ap);
}

void esp_log_write(int level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    va_list ap;
    va_start(ap, format);
    log_v(format, ap);
    va_end(ap);
}

void fake_log(const char *fmt, ...)
{
    TaskHandle_t was = s_current;
    s_current = TASK_APP;
    va_list ap;
    va_start(ap, fmt);
    log_v(fmt, ap);
    va_end(ap);
    s_current = was;
}

void fake_log_as_httpd(const char *fmt, ...)
{
    TaskHandle_t was = s_current;
    s_current = TASK_HTTPD;
    va_list ap;
    va_start(ap, fmt);
    log_v(fmt, ap);
    va_end(ap);
    s_current = was;
}

/* ---- heap -------------------------------------------------------------- */

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if (fake_alloc_fail_at > 0 && --fake_alloc_fail_at == 0) {
        return NULL;
    }
    if (!(caps & MALLOC_CAP_SPIRAM)) {
        fake_internal_bytes += size;
    }
    return malloc(size);
}

void heap_caps_free(void *ptr)
{
    free(ptr);
}

/* ---- FreeRTOS ---------------------------------------------------------- */

void vPortEnterCritical(portMUX_TYPE *mux)
{
    (void)mux;
    s_crit_depth++;
}

void vPortExitCritical(portMUX_TYPE *mux)
{
    (void)mux;
    if (--s_crit_depth < 0) {
        misuse("critical section exited twice");
    }
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return s_current;
}

TickType_t xTaskGetTickCount(void)
{
    return fake_now_ms;
}

void vTaskDelay(const TickType_t ticks)
{
    fake_timed_waits++;
    advance(ticks);
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action)
{
    if (task != TASK_MODULE || action != eSetBits) {
        misuse("unexpected xTaskNotify");
    }
    s_notify_bits |= value;
    return pdPASS;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t *value,
                           TickType_t ticks)
{
    s_notify_bits &= ~clear_on_entry;
    if (s_notify_bits == 0) {
        if (ticks == portMAX_DELAY) {
            s_blocked = true; /* fake_run_ms waits for a notification */
        } else {
            fake_timed_waits++;
            advance(ticks); /* no notification comes in a single thread */
        }
    }
    if (value) {
        *value = s_notify_bits;
    }
    BaseType_t got = s_notify_bits != 0;
    s_notify_bits &= ~clear_on_exit;
    return got;
}

BaseType_t xTaskCreateWithCaps(TaskFunction_t code, const char *const name,
                               configSTACK_DEPTH_TYPE stack, void *const param,
                               UBaseType_t prio, TaskHandle_t *created, UBaseType_t caps)
{
    (void)code;
    (void)name;
    (void)stack;
    (void)param;
    (void)prio;
    if (fake_task_create_fail) {
        return pdFAIL;
    }
    fake_stack_in_psram = (caps & MALLOC_CAP_SPIRAM) != 0;
    *created = TASK_MODULE;
    return pdPASS;
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    if (task != TASK_MODULE) {
        misuse("deleting an unknown task");
    }
    fake_task_deleted++;
}

/* ---- web server -------------------------------------------------------- */

fake_sock_t *fake_sock(int fd)
{
    int i = fd - FAKE_FD_BASE;
    return (i >= 0 && i < FAKE_SOCKS) ? &fake_socks[i] : NULL;
}

static int sock_open(bool ws)
{
    for (int i = 0; i < FAKE_SOCKS; i++) {
        fake_sock_t *s = &fake_socks[i];
        if (!s->open) {
            free(s->rx);
            memset(s, 0, sizeof(*s));
            s->open = true;
            s->ws = ws;
            s->frames_ok = true;
            s->opened_ms = fake_now_ms;
            s->first_ms = -1;
            return FAKE_FD_BASE + i;
        }
    }
    return -1;
}

typedef struct {
    int fd;
    size_t len;
} fake_aux_t;

int fake_ws_open(void)
{
    int fd = sock_open(true);
    if (fd < 0 || !s_post_handshake) {
        return fd;
    }
    /* the web server answers the upgrade, then calls the callback */
    fake_sock_t *s = fake_sock(fd);
    fake_aux_t aux = { fd, 0 };
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.handle = fake_server;
    req.method = HTTP_GET;
    req.aux = &aux;
    TaskHandle_t was = s_current;
    s_current = TASK_HTTPD;
    esp_err_t err = s_post_handshake(&req);
    s_current = was;
    if (!fake_ctx_not_kept) {
        s->ctx = req.sess_ctx;
        s->free_fn = req.free_ctx;
    }
    if (err != ESP_OK) {
        fake_close(fd);
    }
    return fd;
}

int fake_http_open(void)
{
    return sock_open(false);
}

void fake_close(int fd)
{
    fake_sock_t *s = fake_sock(fd);
    if (!s || !s->open) {
        return;
    }
    TaskHandle_t was = s_current;
    s_current = TASK_HTTPD;
    if (s->ctx) {
        if (s->free_fn) {
            s->free_fn(s->ctx);
        } else {
            misuse("context without free function");
        }
    }
    s->ctx = NULL;
    s->free_fn = NULL;
    s->open = false;
    s_current = was;
}

static bool hd_ok(httpd_handle_t hd)
{
    check_not_critical("web server call inside a critical section");
    if (s_current == TASK_MODULE) {
        misuse("web server called from the module's task");
    }
    if (hd != fake_server) {
        misuse("unknown server handle");
        return false;
    }
    return true;
}

static bool in_httpd_task(const char *what)
{
    if (s_current != TASK_HTTPD) {
        misuse(what);
        return false;
    }
    return true;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    if (!hd_ok(handle)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fake_register_err != ESP_OK) {
        return fake_register_err;
    }
    if (strcmp(uri->uri, "/ws/logs") != 0 || uri->method != HTTP_GET || !uri->is_websocket) {
        misuse("unexpected URI registration");
    }
    fake_register_calls++;
    s_handler = uri->handler;
    s_post_handshake = uri->ws_post_handshake_cb;
    return ESP_OK;
}

esp_err_t httpd_get_client_list(httpd_handle_t handle, size_t *fds, int *client_fds)
{
    if (!hd_ok(handle)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = 0;
    for (int i = 0; i < FAKE_SOCKS; i++) {
        if (fake_socks[i].open) {
            if (n == *fds) {
                return ESP_ERR_INVALID_ARG;
            }
            client_fds[n++] = FAKE_FD_BASE + i;
        }
    }
    *fds = n;
    return ESP_OK;
}

httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t hd, int fd)
{
    if (!hd_ok(hd)) {
        return HTTPD_WS_CLIENT_INVALID;
    }
    fake_sock_t *s = fake_sock(fd);
    if (!s || !s->open) {
        return HTTPD_WS_CLIENT_INVALID;
    }
    return s->ws ? HTTPD_WS_CLIENT_WEBSOCKET : HTTPD_WS_CLIENT_HTTP;
}

void *httpd_sess_get_ctx(httpd_handle_t handle, int sockfd)
{
    if (!hd_ok(handle)) {
        return NULL;
    }
    fake_sock_t *s = fake_sock(sockfd);
    return (s && s->open) ? s->ctx : NULL;
}

void httpd_sess_set_ctx(httpd_handle_t handle, int sockfd, void *ctx, httpd_free_ctx_fn_t free_fn)
{
    if (!hd_ok(handle) || !in_httpd_task("httpd_sess_set_ctx outside the web server task")) {
        return;
    }
    fake_sock_t *s = fake_sock(sockfd);
    if (!s || !s->open) {
        misuse("httpd_sess_set_ctx on a closed socket");
        return;
    }
    s->ctx = ctx;
    s->free_fn = free_fn;
}

esp_err_t httpd_queue_work(httpd_handle_t handle, httpd_work_fn_t work, void *arg)
{
    /* the one call allowed from the module's task */
    check_not_critical("httpd_queue_work inside a critical section");
    if (handle != fake_server) {
        misuse("unknown server handle");
        return ESP_ERR_INVALID_ARG;
    }
    fake_queue_work_calls++;
    TaskHandle_t was = s_current;
    s_current = TASK_HTTPD;
    work(arg);
    s_current = was;
    return ESP_OK;
}

static bool frame_utf8_ok(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint32_t c = s[i];
        size_t len;
        uint32_t min;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2, min = 0x80, c &= 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3, min = 0x800, c &= 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4, min = 0x10000, c &= 0x07;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
            c = (c << 6) | (s[i + k] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame)
{
    if (!hd_ok(hd) || !in_httpd_task("send outside the web server task")) {
        return ESP_ERR_INVALID_ARG;
    }
    fake_sock_t *s = fake_sock(fd);
    if (fake_log_on_send) {
        fake_log_as_httpd("W (%u) httpd_ws: sending to %d\n", (unsigned)fake_now_ms, fd);
    }
    if (!s || !s->open || !s->ws) {
        return ESP_FAIL;
    }
    if (s->fail_send) {
        fake_log_as_httpd("E (%u) httpd_ws: send failed on %d\n", (unsigned)fake_now_ms, fd);
        return ESP_FAIL;
    }
    if (frame->type != HTTPD_WS_TYPE_TEXT || frame->len > 4096 || frame->len == 0 ||
        !frame_utf8_ok(frame->payload, frame->len) || (frame->fragmented && !frame->final)) {
        s->frames_ok = false;
    }
    add(&s->rx, &s->rx_len, &s->rx_cap, frame->payload, frame->len);
    s->frames++;
    if (s->first_ms < 0) {
        s->first_ms = fake_now_ms;
    }
    return ESP_OK;
}

esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd)
{
    if (!hd_ok(handle) || !in_httpd_task("close outside the web server task")) {
        return ESP_ERR_INVALID_ARG;
    }
    fake_trigger_close_calls++;
    fake_close(sockfd);
    return ESP_OK;
}

int httpd_req_to_sockfd(httpd_req_t *r)
{
    return ((fake_aux_t *)r->aux)->fd;
}

esp_err_t httpd_ws_recv_frame(httpd_req_t *req, httpd_ws_frame_t *pkt, size_t max_len)
{
    fake_aux_t *aux = req->aux;
    if (max_len == 0) {
        pkt->len = aux->len;
        pkt->type = HTTPD_WS_TYPE_TEXT;
        return ESP_OK;
    }
    if (pkt->payload == NULL || max_len < aux->len) {
        misuse("bad receive buffer");
        return ESP_ERR_INVALID_ARG;
    }
    memset(pkt->payload, 'q', aux->len);
    pkt->len = aux->len;
    return ESP_OK;
}

esp_err_t fake_browser_sends(int fd, size_t len)
{
    fake_sock_t *s = fake_sock(fd);
    if (!s || !s->open || !s_handler) {
        return ESP_FAIL;
    }
    fake_aux_t aux = { fd, len };
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.handle = fake_server;
    req.method = 0; /* a frame, not the handshake */
    req.aux = &aux;
    req.sess_ctx = s->ctx;
    TaskHandle_t was = s_current;
    s_current = TASK_HTTPD;
    esp_err_t err = s_handler(&req);
    s_current = was;
    if (req.sess_ctx != s->ctx) {
        misuse("handler changed the session context");
    }
    if (err != ESP_OK) {
        fake_close(fd);
    }
    return err;
}

/* ---- driving ----------------------------------------------------------- */

void fake_step(void)
{
    s_blocked = false;
    TaskHandle_t was = s_current;
    s_current = TASK_MODULE;
    log_stream_host_step();
    s_current = was;
}

void fake_run_ms(uint32_t ms)
{
    uint32_t end = fake_now_ms + ms;
    while (fake_now_ms < end) {
        if (s_blocked) {
            /* nothing runs until a notification: let time pass to the next
             * event, or to the end */
            int n = s_n_events;
            uint32_t next = end;
            for (int i = 0; i < n; i++) {
                if (s_events[i].at < next) {
                    next = s_events[i].at;
                }
            }
            advance(next > fake_now_ms ? next - fake_now_ms : 0);
            if (s_notify_bits == 0) {
                continue;
            }
        }
        uint32_t before = fake_now_ms;
        fake_step();
        if (fake_now_ms == before) {
            fake_now_ms++; /* a step that took no time */
        }
    }
}

void fake_reset(void)
{
    for (int i = 0; i < FAKE_SOCKS; i++) {
        free(fake_socks[i].rx);
    }
    memset(fake_socks, 0, sizeof(fake_socks));
    free(fake_uart);
    fake_uart = NULL;
    fake_uart_len = s_uart_cap = 0;
    fake_now_ms = 1000;
    fake_queue_work_calls = 0;
    fake_trigger_close_calls = 0;
    fake_register_calls = 0;
    fake_register_err = ESP_OK;
    fake_task_create_fail = false;
    fake_task_deleted = 0;
    fake_alloc_fail_at = 0;
    fake_internal_bytes = 0;
    fake_stack_in_psram = false;
    fake_log_on_send = false;
    fake_ctx_not_kept = false;
    fake_timed_waits = 0;
    fake_errors = 0;
    s_post_handshake = NULL;
    s_blocked = false;
    s_current = TASK_APP;
    s_notify_bits = 0;
    s_crit_depth = 0;
    s_hook = uart_vprintf;
    s_n_events = 0;
    s_handler = NULL;
}
