/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for src/log_stream.c against the fake ESP-IDF in esp_fake.c:
 * the API, browsers that never send anything (found by the post-handshake
 * callback), sending, closing, refusing an 8th browser, sleeping without
 * polling, and what is not captured. The fake also flags any web server
 * call from the module's own task. Build and run:
 * make -C tests
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_fake.h"
#include "log_stream.h"

void log_stream_host_reset(void);

static int s_fail;
static int s_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        s_checks++;                                                          \
        if (!(cond)) {                                                       \
            s_fail++;                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define READY_LINE "Log streaming on /ws/logs"

static void fresh(void)
{
    log_stream_host_reset();
    fake_reset();
}

static void start(void)
{
    fresh();
    CHECK(log_stream_init() == ESP_OK);
    CHECK(log_stream_register(fake_server) == ESP_OK);
}

static bool rx_is(int fd, const char *s)
{
    fake_sock_t *k = fake_sock(fd);
    size_t n = strlen(s);
    return k->rx_len == n && memcmp(k->rx, s, n) == 0;
}

static bool rx_has(int fd, const char *s)
{
    fake_sock_t *k = fake_sock(fd);
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= k->rx_len; i++) {
        if (memcmp(k->rx + i, s, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool uart_has(const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= fake_uart_len; i++) {
        if (memcmp(fake_uart + i, s, n) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- API --------------------------------------------------------------- */

static void test_register_errors(void)
{
    fresh();
    CHECK(log_stream_register(fake_server) == ESP_ERR_INVALID_STATE);
    CHECK(log_stream_init() == ESP_OK);
    CHECK(log_stream_register(NULL) == ESP_ERR_INVALID_ARG);

    fake_task_create_fail = true;
    CHECK(log_stream_register(fake_server) == ESP_ERR_NO_MEM);
    CHECK(fake_register_calls == 0);
    fake_task_create_fail = false;

    fake_register_err = 0x8001; /* e.g. handlers full */
    CHECK(log_stream_register(fake_server) == 0x8001);
    CHECK(fake_task_deleted == 1);
    fake_register_err = ESP_OK;

    fake_alloc_fail_at = 1;
    CHECK(log_stream_register(fake_server) == ESP_ERR_NO_MEM);
    CHECK(fake_register_calls == 0);

    CHECK(log_stream_register(fake_server) == ESP_OK);
    CHECK(fake_register_calls == 1);
    CHECK(log_stream_register(fake_server) == ESP_OK);
    CHECK(fake_register_calls == 1);
    static int other;
    CHECK(log_stream_register(&other) == ESP_ERR_INVALID_STATE);
    CHECK(fake_register_calls == 1);
    CHECK(fake_errors == 0);
}

static void test_init(void)
{
    /* no memory: logging works as before, nothing captured */
    fresh();
    fake_alloc_fail_at = 1;
    CHECK(log_stream_init() == ESP_ERR_NO_MEM);
    fake_log("I (1) a: plain\n");
    CHECK(uart_has("I (1) a: plain\n"));
    CHECK(log_stream_register(fake_server) == ESP_ERR_INVALID_STATE);

    /* second call does nothing: the UART gets each line once */
    fresh();
    CHECK(log_stream_init() == ESP_OK);
    CHECK(log_stream_init() == ESP_OK);
    fake_log("I (2) a: once\n");
    CHECK(fake_uart_len == strlen("I (2) a: once\n"));
    CHECK(fake_internal_bytes == 0);
}

static void test_uart_unchanged(void)
{
    start();
    static char big[3001];
    memset(big, 'x', 3000);
    big[3000] = 0;
    size_t before = fake_uart_len;
    fake_log("%s", big);
    CHECK(fake_uart_len - before == 3000); /* the UART gets all of it */
    int fd = fake_ws_open();
    fake_run_ms(100);
    fake_sock_t *k = fake_sock(fd);
    /* ready line + 1024 x + '?' */
    CHECK(k->rx_len == strlen(READY_LINE) + 1025);
    CHECK(k->rx_len >= 2 && k->rx[k->rx_len - 1] == '?' && k->rx[k->rx_len - 2] == 'x');
}

/* ---- finding browsers -------------------------------------------------- */

static void test_silent_browser_gets_history(void)
{
    start();
    fake_log("I (5) a: one\n");
    fake_log("I (6) a: two\n");
    fake_run_ms(500);
    CHECK(fake_queue_work_calls == 0); /* no browser: no work */
    CHECK(fake_timed_waits == 0);      /* and no polling */

    int fd = fake_ws_open(); /* upgrade done; the handler is not called */
    fake_run_ms(200);
    fake_sock_t *k = fake_sock(fd);
    CHECK(rx_is(fd, READY_LINE "I (5) a: one\nI (6) a: two\n"));
    CHECK(k->first_ms >= 0 && k->first_ms - k->opened_ms <= 200);
    CHECK(k->frames_ok);
    CHECK(fake_errors == 0);
}

/* The browser upgrades, or a line is logged, just after the module's task
 * went to sleep: the worst case for the 200 ms budget. */
static int s_late_fd = -1;

static void open_late(void)
{
    s_late_fd = fake_ws_open();
}

static void log_late(void)
{
    fake_log("\x1b[0;32mI (100) x: hello\x1b[0m\n");
}

static void test_latency_worst_case(void)
{
    start();
    fake_run_ms(300);
    fake_at(fake_now_ms + 1, open_late);
    fake_run_ms(400);
    fake_sock_t *k = fake_sock(s_late_fd);
    CHECK(k && k->first_ms >= 0 && k->first_ms - (int64_t)k->opened_ms <= 200);

    size_t before = k->rx_len;
    uint32_t t = fake_now_ms + 1;
    fake_at(t, log_late);
    while (fake_sock(s_late_fd)->rx_len == before && fake_now_ms < t + 1000) {
        fake_step();
    }
    CHECK(fake_now_ms - t <= 200);
    CHECK(rx_has(s_late_fd, "\x1b[0;32mI (100) x: hello\x1b[0m\n"));
}

static void test_http_socket_ignored(void)
{
    start();
    int h = fake_http_open();
    fake_log("I (1) a: x\n");
    fake_run_ms(300);
    CHECK(fake_queue_work_calls == 0);
    CHECK(fake_sock(h)->rx_len == 0);
}

static void test_four_browsers_and_late_one(void)
{
    start();
    int fd[5];
    for (int i = 0; i < 4; i++) {
        fd[i] = fake_ws_open();
    }
    fake_log("I (6) a: two\n");
    fake_run_ms(200);
    fake_log("I (7) a: three\n");
    fake_run_ms(200);
    fd[4] = fake_ws_open();
    fake_run_ms(200);
    fake_log("I (8) a: four\n");
    fake_run_ms(200);
    const char *all = READY_LINE "I (6) a: two\nI (7) a: three\nI (8) a: four\n";
    for (int i = 0; i < 5; i++) {
        CHECK(rx_is(fd[i], all));
        CHECK(fake_sock(fd[i])->frames_ok);
    }
}

static void test_eighth_browser_refused(void)
{
    start();
    int fd[8];
    for (int i = 0; i < 7; i++) {
        fd[i] = fake_ws_open();
    }
    fake_run_ms(200);
    fd[7] = fake_ws_open();
    fake_run_ms(200);
    CHECK(!fake_sock(fd[7])->open); /* the callback failed: closed */
    CHECK(fake_sock(fd[7])->rx_len == 0);
    CHECK(fake_trigger_close_calls == 0);
    fake_log("I (9) a: after\n");
    fake_run_ms(200);
    for (int i = 0; i < 7; i++) {
        CHECK(rx_has(fd[i], "I (9) a: after\n"));
    }
    CHECK(fake_errors == 0);
}

/* ---- gone browsers ----------------------------------------------------- */

static void test_close_and_fd_reuse(void)
{
    start();
    int a = fake_ws_open();
    int b = fake_ws_open();
    fake_log("I (1) a: first\n");
    fake_run_ms(200);
    fake_close(a); /* the browser leaves */
    fake_log("I (2) a: second\n");
    int c = fake_ws_open(); /* gets the same number */
    CHECK(c == a);
    fake_run_ms(200);
    /* the new one gets the whole history, not the rest of the old stream */
    CHECK(rx_is(c, READY_LINE "I (1) a: first\nI (2) a: second\n"));
    CHECK(rx_is(b, READY_LINE "I (1) a: first\nI (2) a: second\n"));
    CHECK(fake_errors == 0);
}

/* Same, if the web server did not keep the context set in the callback:
 * closed browsers are still noticed, and their place is free again. */
static void test_close_without_context(void)
{
    start();
    fake_ctx_not_kept = true;
    int fd[7];
    for (int i = 0; i < 7; i++) {
        fd[i] = fake_ws_open();
    }
    fake_log("I (1) a: first\n");
    fake_run_ms(200);
    fake_close(fd[0]);
    fake_close(fd[1]);
    int x = fake_ws_open(); /* same number as fd[0] */
    int y = fake_ws_open();
    CHECK(x == fd[0] && y == fd[1]);
    CHECK(fake_sock(x)->open && fake_sock(y)->open); /* not refused */
    fake_log("I (2) a: second\n");
    fake_run_ms(200);
    const char *all = READY_LINE "I (1) a: first\nI (2) a: second\n";
    CHECK(rx_is(x, all));
    CHECK(rx_is(y, all));
    for (int i = 2; i < 7; i++) {
        CHECK(rx_is(fd[i], all));
    }
    CHECK(fake_errors == 0);
}

/* Without the context: a closed browser's number taken by a plain HTTP
 * connection. That connection is neither sent to nor closed, and the
 * browser's place is free for a new one. */
static void test_closed_number_reused_by_http(void)
{
    start();
    fake_ctx_not_kept = true;
    int fd[7];
    for (int i = 0; i < 7; i++) {
        fd[i] = fake_ws_open();
    }
    fake_run_ms(200);
    fake_close(fd[0]);
    int h = fake_http_open();
    CHECK(h == fd[0]);
    fake_log("I (2) a: second\n"); /* sent before any new browser comes */
    fake_run_ms(200);
    CHECK(fake_sock(h)->open && fake_sock(h)->rx_len == 0);
    CHECK(fake_trigger_close_calls == 0);
    int z = fake_ws_open(); /* another number */
    CHECK(z != fd[0] && fake_sock(z)->open);
    fake_log("I (3) a: third\n");
    fake_run_ms(200);
    CHECK(fake_sock(h)->open && fake_sock(h)->rx_len == 0);
    CHECK(fake_trigger_close_calls == 0);
    CHECK(rx_has(z, "I (3) a: third\n"));

    /* again, with the new browser right after: nothing sent in between */
    fake_close(fd[1]);
    int h2 = fake_http_open();
    CHECK(h2 == fd[1]);
    int z2 = fake_ws_open();
    CHECK(z2 != fd[1] && fake_sock(z2)->open); /* not refused: 7 again */
    fake_log("I (4) a: fourth\n");
    fake_run_ms(200);
    CHECK(rx_has(z2, "I (4) a: fourth\n"));
    CHECK(fake_sock(h2)->open && fake_sock(h2)->rx_len == 0);
    CHECK(fake_errors == 0);
}

static void test_sleeps_again_when_last_leaves(void)
{
    start();
    int fd = fake_ws_open();
    fake_run_ms(200);
    fake_close(fd);
    fake_run_ms(100);
    int waits = fake_timed_waits;
    int works = fake_queue_work_calls;
    fake_log("I (1) a: nobody\n");
    fake_run_ms(2000);
    CHECK(fake_timed_waits == waits);
    CHECK(fake_queue_work_calls == works);
}

static void test_send_failure(void)
{
    start();
    int a = fake_ws_open();
    int b = fake_ws_open();
    fake_run_ms(200);
    fake_sock(a)->fail_send = true;
    fake_log("I (1) a: x\n");
    fake_run_ms(200);
    CHECK(!fake_sock(a)->open); /* forgotten and closed */
    CHECK(fake_trigger_close_calls == 1);
    CHECK(rx_has(b, "I (1) a: x\n"));
    /* its failure message went to the UART only */
    CHECK(uart_has("send failed"));
    CHECK(!rx_has(b, "send failed"));
    fake_log("I (2) a: y\n");
    fake_run_ms(200);
    CHECK(rx_has(b, "I (2) a: y\n"));
    CHECK(fake_trigger_close_calls == 1);
}

static void test_browser_frames(void)
{
    start();
    int fd = fake_ws_open();
    fake_run_ms(200);
    CHECK(fake_browser_sends(fd, 10) == ESP_OK);
    CHECK(fake_browser_sends(fd, 512) == ESP_OK);
    CHECK(fake_sock(fd)->open);
    CHECK(fake_browser_sends(fd, 513) != ESP_OK);
    CHECK(!fake_sock(fd)->open);
    fake_log("I (1) a: x\n");
    fake_run_ms(200);
    CHECK(fake_errors == 0);
}

/* ---- what is captured -------------------------------------------------- */

static void test_logs_during_send_not_captured(void)
{
    start();
    int fd = fake_ws_open();
    fake_log_on_send = true;
    fake_log("I (1) a: x\n");
    fake_run_ms(400);
    CHECK(uart_has("httpd_ws: sending"));
    CHECK(!rx_has(fd, "httpd_ws"));
    size_t frames = fake_sock(fd)->frames;
    fake_run_ms(1000);
    CHECK(fake_sock(fd)->frames == frames); /* no growth from its own sends */
    /* the web server's messages at other times are captured */
    fake_log_on_send = false;
    fake_log_as_httpd("I (2) httpd: other\n");
    fake_run_ms(200);
    CHECK(rx_has(fd, "I (2) httpd: other\n"));
}

static void test_boot_log(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    static char file[65536];
    size_t n = fread(file, 1, sizeof(file) - 1, f);
    fclose(f);
    file[n] = 0;

    start();
    int early = fake_ws_open();
    fake_run_ms(100);
    char *line = file;
    while (*line) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        char tmp[2048];
        memcpy(tmp, line, len);
        tmp[len] = 0;
        fake_log("%s", tmp);
        line += len;
        fake_step();
    }
    fake_run_ms(300);
    int late = fake_ws_open();
    fake_run_ms(300);
    size_t r = strlen(READY_LINE);
    fake_sock_t *e = fake_sock(early), *l = fake_sock(late);
    CHECK(e->rx_len == r + n && memcmp(e->rx + r, file, n) == 0);
    CHECK(l->rx_len == r + n && memcmp(l->rx + r, file, n) == 0);
    CHECK(e->frames_ok && l->frames_ok);
}

static void test_memory_placement(void)
{
    fresh();
    CHECK(log_stream_init() == ESP_OK);
    CHECK(log_stream_register(fake_server) == ESP_OK);
    int fd = fake_ws_open();
    fake_run_ms(200);
    CHECK(fake_browser_sends(fd, 100) == ESP_OK);
    CHECK(fake_internal_bytes == 0); /* all buffers asked from PSRAM */
    CHECK(fake_stack_in_psram);
}

int main(int argc, char **argv)
{
    const char *boot = argc > 1 ? argv[1] : "examples/boot.log";
    test_register_errors();
    test_init();
    test_uart_unchanged();
    test_silent_browser_gets_history();
    test_latency_worst_case();
    test_http_socket_ignored();
    test_four_browsers_and_late_one();
    test_eighth_browser_refused();
    test_close_and_fd_reuse();
    test_close_without_context();
    test_closed_number_reused_by_http();
    test_sleeps_again_when_last_leaves();
    test_send_failure();
    test_browser_frames();
    test_logs_during_send_not_captured();
    test_boot_log(boot);
    test_memory_placement();
    log_stream_host_reset();
    fake_reset();
    printf("%d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
