/* SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of src/ota.c (ota_start_from_http) against the stand-ins in
 * fake_idf/: the examples of README.md, broken connections, timeouts and
 * failing flash/OTA calls. */
#include "fake_idf/fake_idf.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "ota.h"
#include "ota_image.h"
#include "test_util.h"

int g_failures;
int g_checks;

static const char GATEWAY[] = "airplay-gateway_v4.5.49";
static const char SPEAKER[] = "airplay-speaker_v4.5.49";
#define GATEWAY_LEN 1328736u
#define SPEAKER_LEN 1796816u

static uint8_t gateway_head[EXAMPLE_LEN];
static uint8_t speaker_head[EXAMPLE_LEN];
static uint8_t not_firmware[EXAMPLE_LEN];
static int g_boot_before; /* partition set to boot next when the call starts */

/* A body of len bytes that starts with head (or as much of it as fits). */
static uint8_t *make_body(const uint8_t *head, size_t len)
{
    uint8_t *b = malloc(len ? len : 1);
    for (size_t i = 0; i < len; i++) {
        b[i] = (uint8_t)(i * 31u + 7u);
    }
    memcpy(b, head, len < EXAMPLE_LEN ? len : EXAMPLE_LEN);
    return b;
}

static esp_err_t upload(uint8_t *body, size_t body_len, size_t content_len)
{
    httpd_req_t req = { .content_len = content_len };
    fake_set_body(body, body_len);
    g_boot_before = fake_boot_index();
    return ota_start_from_http(&req);
}

static int count_lines(char level, const char *needle)
{
    int n = 0;
    for (int i = 0; i < g_fake.nlines; i++) {
        if (g_fake.lines[i][0] == level && (needle == NULL || strstr(g_fake.lines[i], needle))) {
            n++;
        }
    }
    return n;
}

/* What every call must respect, whatever the outcome. */
static void check_common(esp_err_t err)
{
    CHECK_EQ(g_fake.frees, g_fake.allocs);        /* nothing kept */
    CHECK(!g_fake.open);                          /* OTA handle released */
    CHECK_EQ(g_fake.misuse, 0);
    CHECK_EQ(g_fake.log_other_tag, 0);
    CHECK(g_fake.max_recv_ask <= 4096);           /* pieces of at most 4 KiB */
    CHECK(g_fake.max_write <= 4096);
    if (g_fake.allocs) {
        CHECK(g_fake.alloc_size <= 4096);
        CHECK(g_fake.alloc_caps & MALLOC_CAP_SPIRAM);
    }
    /* At most: start, 10 % to 90 %, end. */
    CHECK(g_fake.nlines <= 11);
    CHECK(count_lines('I', "%") <= 9);
    CHECK_EQ(count_lines('I', "100 %"), 0);
    CHECK_EQ(g_fake.log_e, err == ESP_OK ? 0 : 1); /* one line for the reason */
    /* Never written while the board points elsewhere than the running firmware. */
    CHECK_EQ(g_fake.write_while_boot_elsewhere, 0);
    if (err != ESP_OK) {
        CHECK_EQ(count_lines('E', esp_err_to_name(err)), 1);
        CHECK_EQ(g_fake.boot_set_to_update, 0);
        if (g_fake.begin_calls > 0) {
            CHECK_EQ(fake_boot_index(), g_fake.running); /* boots the running firmware */
        } else {
            CHECK_EQ(fake_boot_index(), g_boot_before);  /* boot setting untouched */
        }
    }
}

/* Refused from the start of the image: flash never touched. */
static void check_untouched(void)
{
    CHECK_EQ(g_fake.begin_calls, 0);
    CHECK_EQ(g_fake.write_calls, 0);
    CHECK_EQ(g_fake.set_boot_calls, 0);
    CHECK(fake_flash_untouched());
}

static void check_accepted(const uint8_t *body, size_t len, int target)
{
    CHECK_EQ(fake_boot_index(), target);
    CHECK_EQ(g_fake.begin_calls, 1);
    CHECK_EQ(g_fake.begin_size, OTA_WITH_SEQUENTIAL_WRITES);
    CHECK_EQ(g_fake.end_calls, 1);
    CHECK_EQ(g_fake.abort_calls, 0);
    CHECK_EQ(g_fake.set_boot_calls, 1);
    CHECK_EQ(g_fake.written, len);
    CHECK(memcmp(g_fake.flash[target], body, len) == 0);
    /* The running partition is never written. */
    size_t touched = 0;
    for (size_t i = 0; i < g_fake.flash_size; i++) {
        touched += g_fake.flash[1 - target][i] != 0xFF;
    }
    CHECK_EQ(touched, 0);
}

/* Example 1 */
static void test_ex1_gateway_accepted(void)
{
    fake_reset(GATEWAY);
    g_fake.chunk = 1436; /* what one TCP segment brings */
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    check_accepted(body, GATEWAY_LEN, 1);
    /* Start line: size, partition, project and version. */
    CHECK_EQ(count_lines('I', "1328736"), 1);
    CHECK(strstr(g_fake.lines[0], "1328736") && strstr(g_fake.lines[0], "ota_1") &&
          strstr(g_fake.lines[0], GATEWAY) && strstr(g_fake.lines[0], "v4.5.49"));
    CHECK_EQ(count_lines('I', "%"), 9);
    CHECK(strstr(g_fake.lines[g_fake.nlines - 1], "ota_1") != NULL);
    CHECK(strstr(g_fake.lines[g_fake.nlines - 1], "done") != NULL);
    CHECK_EQ(g_fake.nlines, 11);
    CHECK(strstr(g_fake.lines[0], "boot set back") == NULL);
    free(body);
}

/* The speaker on a speaker, a body of exactly the partition size, running
 * from ota_1, and the head arriving one byte at a time. */
static void test_accepted_variants(void)
{
    fake_reset(SPEAKER);
    uint8_t *body = make_body(speaker_head, SPEAKER_LEN);
    esp_err_t err = upload(body, SPEAKER_LEN, SPEAKER_LEN);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    check_accepted(body, SPEAKER_LEN, 1);
    free(body);

    fake_reset(GATEWAY);
    body = make_body(gateway_head, 0x300000);
    err = upload(body, 0x300000, 0x300000);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    check_accepted(body, 0x300000, 1);
    free(body);

    fake_reset(GATEWAY);
    g_fake.running = 1;
    g_fake.boot = &g_fake.parts[1];
    body = make_body(gateway_head, GATEWAY_LEN);
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_OK);
    CHECK_EQ(fake_boot_index(), 0);
    CHECK(memcmp(g_fake.flash[0], body, GATEWAY_LEN) == 0);
    CHECK(strstr(g_fake.lines[0], "ota_0") != NULL);
    free(body);

    fake_reset(GATEWAY);
    g_fake.chunk = 1;
    body = make_body(gateway_head, 20000);
    err = upload(body, 20000, 20000);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    check_accepted(body, 20000, 1);
    free(body);
}

/* Examples 2 to 5: refused before flash */
static void refused(const char *running, const uint8_t *head, size_t len, esp_err_t want, const char *in_log)
{
    fake_reset(running);
    uint8_t *body = make_body(head, len);
    esp_err_t err = upload(body, len, len);
    CHECK_EQ(err, want);
    check_common(err);
    check_untouched();
    CHECK(g_fake.recv_calls >= 1);
    if (in_log) {
        CHECK_EQ(count_lines('E', in_log), 1);
    }
    free(body);
}

static void test_ex2_speaker_on_gateway(void)
{
    refused(GATEWAY, speaker_head, SPEAKER_LEN, ESP_ERR_NOT_SUPPORTED, SPEAKER);
}

static void test_ex3_gateway_on_speaker(void)
{
    refused(SPEAKER, gateway_head, GATEWAY_LEN, ESP_ERR_NOT_SUPPORTED, GATEWAY);
}

static void test_ex4_not_firmware(void)
{
    refused(GATEWAY, not_firmware, GATEWAY_LEN, ESP_ERR_IMAGE_INVALID, NULL);
}

static void test_ex5_chip_esp32(void)
{
    uint8_t h[EXAMPLE_LEN];
    memcpy(h, gateway_head, sizeof(h));
    h[12] = 0;
    h[13] = 0;
    refused(GATEWAY, h, GATEWAY_LEN, ESP_ERR_IMAGE_INVALID, NULL);
    memcpy(h, gateway_head, sizeof(h));
    h[33] ^= 0x01; /* description magic */
    refused(GATEWAY, h, GATEWAY_LEN, ESP_ERR_IMAGE_INVALID, NULL);
}

/* Example 10 */
static void test_ex10_no_hash(void)
{
    uint8_t h[EXAMPLE_LEN];
    memcpy(h, gateway_head, sizeof(h));
    h[23] = 0;
    refused(GATEWAY, h, GATEWAY_LEN, ESP_ERR_IMAGE_INVALID, NULL);
    h[23] = 2;
    refused(GATEWAY, h, GATEWAY_LEN, ESP_ERR_IMAGE_INVALID, NULL);
}

/* Example 11 */
static void test_ex11_running_without_kind(void)
{
    refused("airplay-gateway", gateway_head, GATEWAY_LEN, ESP_ERR_NOT_SUPPORTED, GATEWAY);
}

/* Examples 6 and 7: refused before reading the body */
static void size_refused(size_t content_len)
{
    fake_reset(GATEWAY);
    uint8_t *body = make_body(gateway_head, content_len);
    esp_err_t err = upload(body, content_len, content_len);
    CHECK_EQ(err, ESP_ERR_INVALID_SIZE);
    check_common(err);
    check_untouched();
    CHECK_EQ(g_fake.recv_calls, 0);
    CHECK_EQ(g_fake.allocs, 0);
    free(body);
}

static void test_ex6_too_long(void)
{
    size_refused(0x300001);
    /* The real partition size counts, not a constant. */
    fake_reset(GATEWAY);
    g_fake.parts[1].size = 0x100000;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_INVALID_SIZE);
    CHECK_EQ(g_fake.recv_calls, 0);
    free(body);
}

static void test_ex7_too_short(void)
{
    size_refused(100);
    size_refused(287);
    size_refused(1);
}

/* Example 8 */
static void test_ex8_other_version(void)
{
    uint8_t h[EXAMPLE_LEN];
    memcpy(h, gateway_head, sizeof(h));
    memset(h + 80, 0, 32);
    memcpy(h + 80, "airplay-gateway_v4.5.10", 23);
    memset(h + 48, 0, 32);
    memcpy(h + 48, "v4.5.10", 7);
    fake_reset(GATEWAY);
    uint8_t *body = make_body(h, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    check_accepted(body, GATEWAY_LEN, 1);
    CHECK(strstr(g_fake.lines[0], "v4.5.10") != NULL);
    free(body);
}

/* Example 9, and the connection closing at other points */
static void closed_at(size_t at, bool started)
{
    fake_reset(GATEWAY);
    g_fake.close_at = at;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_FAIL);
    check_common(err);
    CHECK_EQ(g_fake.set_boot_calls, 0);
    CHECK_EQ(g_fake.end_calls, 0);
    CHECK_EQ(g_fake.recv_calls_after_error, 0); /* fails at once */
    if (started) {
        CHECK_EQ(g_fake.abort_calls, 1);
    } else {
        check_untouched();
    }
    free(body);
}

static void test_ex9_closed_half_way(void)
{
    closed_at(GATEWAY_LEN / 2, true);
    closed_at(GATEWAY_LEN - 1, true);
    closed_at(0, false);
    closed_at(100, false);
    closed_at(287, false);
    /* Body shorter than Content-Length. */
    fake_reset(GATEWAY);
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, 600000, GATEWAY_LEN);
    CHECK_EQ(err, ESP_FAIL);
    check_common(err);
    CHECK_EQ(g_fake.abort_calls, 1);
    free(body);
}

static void test_receive_error(void)
{
    fake_reset(GATEWAY);
    g_fake.fail_at = 700000;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_FAIL);
    check_common(err);
    CHECK_EQ(g_fake.abort_calls, 1);
    CHECK_EQ(g_fake.recv_calls_after_error, 0);
    free(body);
}

static void test_bad_args(void)
{
    fake_reset(GATEWAY);
    esp_err_t err = ota_start_from_http(NULL);
    CHECK_EQ(err, ESP_ERR_INVALID_ARG);
    check_common(err);
    check_untouched();

    fake_reset(GATEWAY);
    err = upload(NULL, 0, 0);
    CHECK_EQ(err, ESP_ERR_INVALID_ARG);
    check_common(err);
    check_untouched();
    CHECK_EQ(g_fake.recv_calls, 0);
}

static void test_no_partition(void)
{
    fake_reset(GATEWAY);
    g_fake.no_next = true;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_NOT_FOUND);
    check_common(err);
    check_untouched();
    CHECK_EQ(g_fake.recv_calls, 0);
    free(body);
}

static void test_no_memory(void)
{
    fake_reset(GATEWAY);
    g_fake.malloc_fail = true;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_NO_MEM);
    check_common(err);
    check_untouched();
    CHECK_EQ(g_fake.recv_calls, 0);
    free(body);
}

/* A read that times out is tried again ... */
static void test_timeout_retried(void)
{
    const int64_t waits[] = { 5000000, 9000000, 1000000 };
    const int counts[] = { 1, 1, 9 };
    for (size_t i = 0; i < 3; i++) {
        fake_reset(GATEWAY);
        g_fake.recv_wait_us = waits[i];
        g_fake.stall_at = 500000;
        g_fake.stall_timeouts = counts[i];
        uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
        esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
        CHECK_EQ(err, ESP_OK);
        check_common(err);
        check_accepted(body, GATEWAY_LEN, 1);
        CHECK_EQ(g_fake.timeouts, counts[i]);
        free(body);
    }
    /* A 9 s pause while the head is still being read. */
    fake_reset(GATEWAY);
    g_fake.stall_at = 100;
    g_fake.stall_timeouts = 1;
    g_fake.recv_wait_us = 9000000;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    free(body);
}

/* ... and fails only after 10 s in a row without a byte. */
static void stalled(size_t at, int64_t wait_us, int want_timeouts, bool started)
{
    fake_reset(GATEWAY);
    g_fake.stall_at = at;
    g_fake.stall_timeouts = -1;
    g_fake.recv_wait_us = wait_us;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_TIMEOUT);
    check_common(err);
    CHECK_EQ(g_fake.timeouts, want_timeouts);
    CHECK_EQ(g_fake.abort_calls, started ? 1 : 0);
    CHECK_EQ(g_fake.set_boot_calls, 0);
    if (!started) {
        check_untouched();
    }
    free(body);
}

static void test_timeout_fails(void)
{
    stalled(800000, 5000000, 2, true);   /* default recv_wait_timeout 5 s */
    stalled(800000, 1000000, 10, true);
    stalled(800000, 3000000, 4, true);   /* 12 s: first check at or past 10 s */
    stalled(800000, 20000000, 1, true);  /* a longer socket timeout: the first timeout past 10 s */
    stalled(0, 5000000, 2, false);
    stalled(200, 5000000, 2, false);
}

static void test_flash_errors(void)
{
    /* esp_ota_begin fails: its code, nothing written. */
    fake_reset(GATEWAY);
    g_fake.begin_err = ESP_ERR_FLASH_OP_FAIL;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_FLASH_OP_FAIL);
    check_common(err);
    CHECK_EQ(g_fake.abort_calls, 0);
    CHECK(fake_flash_untouched());

    /* esp_ota_write fails half way: its code, handle aborted. */
    fake_reset(GATEWAY);
    g_fake.write_fail_at = 600000;
    g_fake.write_err = ESP_ERR_FLASH_OP_FAIL;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_FLASH_OP_FAIL);
    check_common(err);
    CHECK_EQ(g_fake.abort_calls, 1);
    CHECK_EQ(g_fake.end_calls, 0);

    /* The complete image fails validation: no abort after end. */
    fake_reset(GATEWAY);
    g_fake.end_err = ESP_ERR_OTA_VALIDATE_FAILED;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_OTA_VALIDATE_FAILED);
    check_common(err);
    CHECK_EQ(g_fake.end_calls, 1);
    CHECK_EQ(g_fake.abort_calls, 0);
    CHECK_EQ(g_fake.set_boot_calls, 0);
    CHECK_EQ(g_fake.recv_calls_after_error, 0);

    /* Setting the boot partition fails: its code. */
    fake_reset(GATEWAY);
    g_fake.set_boot_err = ESP_ERR_FLASH_OP_FAIL;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_FLASH_OP_FAIL);
    check_common(err);
    CHECK_EQ(g_fake.set_boot_calls, 1);
    free(body);

    /* Shortest upload that passes the checks: 288 bytes, then validation. */
    fake_reset(GATEWAY);
    g_fake.end_err = ESP_ERR_OTA_VALIDATE_FAILED;
    body = make_body(gateway_head, OTA_IMAGE_HEAD_LEN);
    err = upload(body, OTA_IMAGE_HEAD_LEN, OTA_IMAGE_HEAD_LEN);
    CHECK_EQ(err, ESP_ERR_OTA_VALIDATE_FAILED);
    check_common(err);
    CHECK_EQ(g_fake.written, OTA_IMAGE_HEAD_LEN);
    free(body);
}

/* Every byte of the first 288 changed, through the whole call: a refused
 * image never reaches flash, an accepted one is written. */
static void test_damaged_head_end_to_end(void)
{
    const size_t len = 8192;
    int bad = 0;
    for (size_t n = 0; n < 2 * OTA_IMAGE_HEAD_LEN; n++) {
        size_t off = n % OTA_IMAGE_HEAD_LEN;
        uint8_t v = n < OTA_IMAGE_HEAD_LEN ? (uint8_t)(gateway_head[off] ^ 0xFF) : 0;
        if (v == gateway_head[off]) {
            continue;
        }
        uint8_t h[EXAMPLE_LEN];
        memcpy(h, gateway_head, sizeof(h));
        h[off] = v;
        fake_reset(GATEWAY);
        uint8_t *body = make_body(h, len);
        esp_err_t err = upload(body, len, len);
        bool kind = off >= 80 && off <= 95;
        bool image = off == 0 || off == 12 || off == 13 || off == 23 || (off >= 32 && off < 36);
        esp_err_t want = image ? ESP_ERR_IMAGE_INVALID : kind ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
        if (err != want) {
            bad++;
            printf("  offset %zu: got 0x%x want 0x%x\n", off, err, want);
        }
        if (want != ESP_OK && (g_fake.begin_calls != 0 || !fake_flash_untouched())) {
            bad++;
        }
        if (g_fake.frees != g_fake.allocs || g_fake.open || g_fake.log_e != (want != ESP_OK)) {
            bad++;
        }
        free(body);
    }
    CHECK_EQ(bad, 0);
}

/* Example 12: a second call while the first is writing. */
static esp_err_t g_nested_err[2];
static int g_nested_side_effects;

static void nested_upload(void)
{
    int before = g_fake.begin_calls + g_fake.write_calls + g_fake.set_boot_calls + g_fake.end_calls +
                 g_fake.abort_calls + g_fake.allocs + g_fake.recv_calls;
    httpd_req_t req2 = { .content_len = GATEWAY_LEN };
    g_nested_err[0] = ota_start_from_http(&req2);
    g_nested_err[1] = ota_start_from_http(NULL);
    int after = g_fake.begin_calls + g_fake.write_calls + g_fake.set_boot_calls + g_fake.end_calls +
                g_fake.abort_calls + g_fake.allocs + g_fake.recv_calls;
    g_nested_side_effects = after - before;
}

static void test_ex12_second_call(void)
{
    fake_reset(GATEWAY);
    g_fake.hook = nested_upload;
    g_fake.hook_at = 600000;
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(g_fake.hook == NULL, 1);           /* the hook ran */
    CHECK_EQ(g_nested_err[0], ESP_ERR_INVALID_STATE);
    CHECK_EQ(g_nested_err[1], ESP_ERR_INVALID_STATE);
    CHECK_EQ(g_nested_side_effects, 0);         /* touched nothing */
    CHECK_EQ(err, ESP_OK);                      /* the first goes on */
    check_accepted(body, GATEWAY_LEN, 1);
    CHECK_EQ(g_fake.log_e, 2);
    CHECK_EQ(count_lines('E', "ESP_ERR_INVALID_STATE"), 2);

    /* Once it has returned, success or failure, the next call runs. */
    fake_reset(GATEWAY);
    g_fake.close_at = 1000;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_FAIL);
    fake_reset(GATEWAY);
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    free(body);
}

/* Example 13: an earlier update already set ota_1 to boot next. */
static void test_ex13_earlier_update_pending(void)
{
    uint8_t *body = make_body(gateway_head, GATEWAY_LEN);

    /* The new upload breaks off half way: the board boots the running firmware. */
    fake_reset(GATEWAY);
    g_fake.boot = &g_fake.parts[1];
    g_fake.close_at = GATEWAY_LEN / 2;
    esp_err_t err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_FAIL);
    check_common(err);
    CHECK_EQ(fake_boot_index(), 0);
    CHECK_EQ(g_fake.boot_set_to_running, 1);
    CHECK_EQ(g_fake.abort_calls, 1);

    /* It completes: ota_1 boots next again, with the new image. */
    fake_reset(GATEWAY);
    g_fake.boot = &g_fake.parts[1];
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_OK);
    check_common(err);
    CHECK_EQ(fake_boot_index(), 1);
    CHECK_EQ(g_fake.boot_set_to_running, 1);
    CHECK_EQ(g_fake.boot_set_to_update, 1);
    CHECK(strstr(g_fake.lines[0], "boot set back to ota_0") != NULL);
    CHECK_EQ(g_fake.nlines, 11);

    /* A refused image changes nothing, not even the pending boot setting. */
    fake_reset(GATEWAY);
    g_fake.boot = &g_fake.parts[1];
    uint8_t *other = make_body(speaker_head, SPEAKER_LEN);
    err = upload(other, SPEAKER_LEN, SPEAKER_LEN);
    CHECK_EQ(err, ESP_ERR_NOT_SUPPORTED);
    check_common(err);
    check_untouched();
    CHECK_EQ(fake_boot_index(), 1);
    free(other);
    fake_reset(GATEWAY);
    g_fake.boot = &g_fake.parts[1];
    err = upload(body, 0x300001, 0x300001);
    CHECK_EQ(err, ESP_ERR_INVALID_SIZE);
    CHECK_EQ(fake_boot_index(), 1);

    /* Setting it back fails: its code, nothing written. */
    fake_reset(GATEWAY);
    g_fake.boot = &g_fake.parts[1];
    g_fake.set_boot_err = ESP_ERR_FLASH_OP_FAIL;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_FLASH_OP_FAIL);
    check_common(err);
    CHECK_EQ(g_fake.set_boot_calls, 1);
    CHECK_EQ(g_fake.begin_calls, 0);
    CHECK_EQ(g_fake.write_calls, 0);
    CHECK(fake_flash_untouched());
    CHECK_EQ(fake_boot_index(), 1);

    /* Running from ota_1, ota_0 pending. */
    fake_reset(GATEWAY);
    g_fake.running = 1;
    g_fake.boot = &g_fake.parts[0];
    g_fake.close_at = GATEWAY_LEN / 2;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_FAIL);
    check_common(err);
    CHECK_EQ(fake_boot_index(), 1);

    /* Boot partition cannot be read: set back to the running one (README, step 4). */
    fake_reset(GATEWAY);
    g_fake.no_boot = true;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_OK);
    CHECK_EQ(g_fake.boot_set_to_running, 1);
    CHECK_EQ(fake_boot_index(), 1);

    /* Running partition not found: refused before reading (README, step 1). */
    fake_reset(GATEWAY);
    g_fake.no_running = true;
    err = upload(body, GATEWAY_LEN, GATEWAY_LEN);
    CHECK_EQ(err, ESP_ERR_NOT_FOUND);
    check_common(err);
    check_untouched();
    CHECK_EQ(g_fake.recv_calls, 0);
    free(body);
}

int main(void)
{
    load_example("gateway_head.bin", gateway_head);
    load_example("speaker_head.bin", speaker_head);
    load_example("not_firmware.bin", not_firmware);

    RUN(test_ex1_gateway_accepted);
    RUN(test_accepted_variants);
    RUN(test_ex2_speaker_on_gateway);
    RUN(test_ex3_gateway_on_speaker);
    RUN(test_ex4_not_firmware);
    RUN(test_ex5_chip_esp32);
    RUN(test_ex10_no_hash);
    RUN(test_ex11_running_without_kind);
    RUN(test_ex6_too_long);
    RUN(test_ex7_too_short);
    RUN(test_ex8_other_version);
    RUN(test_ex9_closed_half_way);
    RUN(test_ex12_second_call);
    RUN(test_ex13_earlier_update_pending);
    RUN(test_receive_error);
    RUN(test_bad_args);
    RUN(test_no_partition);
    RUN(test_no_memory);
    RUN(test_timeout_retried);
    RUN(test_timeout_fails);
    RUN(test_flash_errors);
    RUN(test_damaged_head_end_to_end);
    fake_free();
    return test_summary("test_ota_http");
}
