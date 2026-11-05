/* SPDX-License-Identifier: Apache-2.0 */
#include "fake_idf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

fake_t g_fake;

#define FAKE_HANDLE 7u

void fake_reset(const char *running_project)
{
    fake_free();
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.close_at = FAKE_NEVER;
    g_fake.fail_at = FAKE_NEVER;
    g_fake.stall_at = FAKE_NEVER;
    g_fake.write_fail_at = FAKE_NEVER;
    g_fake.hook_at = FAKE_NEVER;
    g_fake.recv_wait_us = 5 * 1000 * 1000; /* HTTPD_DEFAULT_CONFIG recv_wait_timeout = 5 s */
    g_fake.now_us = 1000000;
    g_fake.flash_size = 0x300000;
    g_fake.parts[0] = (esp_partition_t){ .address = 0x20000, .size = 0x300000, .label = "ota_0" };
    g_fake.parts[1] = (esp_partition_t){ .address = 0x320000, .size = 0x300000, .label = "ota_1" };
    g_fake.running = 0;
    g_fake.boot = &g_fake.parts[0];
    for (int i = 0; i < 2; i++) {
        g_fake.flash[i] = malloc(g_fake.flash_size);
        memset(g_fake.flash[i], 0xFF, g_fake.flash_size);
    }
    g_fake.running_desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    strncpy(g_fake.running_desc.project_name, running_project, sizeof(g_fake.running_desc.project_name));
}

void fake_free(void)
{
    for (int i = 0; i < 2; i++) {
        free(g_fake.flash[i]);
        g_fake.flash[i] = NULL;
    }
}

void fake_set_body(const uint8_t *body, size_t len)
{
    g_fake.body = body;
    g_fake.body_len = len;
    g_fake.pos = 0;
}

int fake_boot_index(void)
{
    return g_fake.boot == &g_fake.parts[1] ? 1 : 0;
}

bool fake_flash_untouched(void)
{
    for (int i = 0; i < 2; i++) {
        for (size_t j = 0; j < g_fake.flash_size; j++) {
            if (g_fake.flash[i][j] != 0xFF) {
                return false;
            }
        }
    }
    return true;
}

/* esp_err.h */
const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_OTA_PARTITION_CONFLICT: return "ESP_ERR_OTA_PARTITION_CONFLICT";
    case ESP_ERR_OTA_VALIDATE_FAILED: return "ESP_ERR_OTA_VALIDATE_FAILED";
    case ESP_ERR_IMAGE_INVALID: return "ESP_ERR_IMAGE_INVALID";
    case ESP_ERR_FLASH_OP_FAIL: return "ESP_ERR_FLASH_OP_FAIL";
    }
    return "UNKNOWN ERROR";
}

/* esp_http_server.h */
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    g_fake.recv_calls++;
    if (g_fake.recv_error_seen) {
        g_fake.recv_calls_after_error++;
        if (g_fake.recv_calls_after_error > 1000) {
            printf("  FAIL: httpd_req_recv called again and again after the connection ended\n");
            abort();
        }
    }
    if (buf_len > g_fake.max_recv_ask) {
        g_fake.max_recv_ask = buf_len;
    }
    if (buf == NULL) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    if (buf_len == 0) {
        return 0;
    }
    if (g_fake.hook != NULL && g_fake.pos >= g_fake.hook_at) {
        void (*hook)(void) = g_fake.hook;
        g_fake.hook = NULL;
        hook();
    }
    if (g_fake.pos >= g_fake.fail_at) {
        g_fake.recv_error_seen = true;
        return HTTPD_SOCK_ERR_FAIL;
    }
    if (g_fake.pos >= g_fake.stall_at && g_fake.stall_timeouts != 0) {
        if (g_fake.stall_timeouts > 0) {
            g_fake.stall_timeouts--;
        }
        g_fake.now_us += g_fake.recv_wait_us;
        g_fake.timeouts++;
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    if (g_fake.pos >= g_fake.close_at || g_fake.pos >= g_fake.body_len) {
        g_fake.recv_error_seen = true;
        return 0;
    }
    size_t n = g_fake.body_len - g_fake.pos;
    if (n > buf_len) n = buf_len;
    if (g_fake.chunk && n > g_fake.chunk) n = g_fake.chunk;
    /* Stop at the scripted events so that they happen at their offset. */
    size_t stops[] = { g_fake.close_at, g_fake.fail_at, g_fake.stall_at,
                       g_fake.hook ? g_fake.hook_at : FAKE_NEVER };
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        if (stops[i] > g_fake.pos && g_fake.pos + n > stops[i]) {
            n = stops[i] - g_fake.pos;
        }
    }
    memcpy(buf, g_fake.body + g_fake.pos, n);
    g_fake.pos += n;
    g_fake.now_us += 1000;
    return (int)n;
}

/* esp_ota_ops.h */
static int part_index(const esp_partition_t *p)
{
    if (p == &g_fake.parts[0]) return 0;
    if (p == &g_fake.parts[1]) return 1;
    return -1;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return g_fake.no_running ? NULL : &g_fake.parts[g_fake.running];
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    if (g_fake.no_boot) {
        return NULL;
    }
    /* A copy at another address, as ESP-IDF does not promise the same pointer. */
    static esp_partition_t copy;
    copy = *g_fake.boot;
    return &copy;
}

static void check_boot_at_running(void)
{
    if (g_fake.boot != &g_fake.parts[g_fake.running]) {
        g_fake.write_while_boot_elsewhere++;
    }
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    if (g_fake.no_next) {
        return NULL;
    }
    int from = start_from ? part_index(start_from) : g_fake.running;
    return &g_fake.parts[from == 0 ? 1 : 0];
}

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle)
{
    g_fake.begin_calls++;
    g_fake.begin_size = image_size;
    if (partition == NULL || out_handle == NULL || part_index(partition) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (part_index(partition) == g_fake.running) {
        return ESP_ERR_OTA_PARTITION_CONFLICT;
    }
    check_boot_at_running();
    if (g_fake.begin_err != ESP_OK) {
        return g_fake.begin_err;
    }
    if (image_size != OTA_WITH_SEQUENTIAL_WRITES) {
        /* Begin with a known size or OTA_SIZE_UNKNOWN erases up front. */
        memset(g_fake.flash[part_index(partition)], 0xFF, g_fake.flash_size);
    }
    g_fake.open = true;
    g_fake.written = 0;
    *out_handle = FAKE_HANDLE;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    g_fake.write_calls++;
    if (!g_fake.open || handle != FAKE_HANDLE) {
        g_fake.misuse++;
        return ESP_ERR_INVALID_ARG;
    }
    if (size == 0) {
        return ESP_OK;
    }
    check_boot_at_running();
    if (size > g_fake.max_write) {
        g_fake.max_write = size;
    }
    if (g_fake.written == 0 && ((const uint8_t *)data)[0] != ESP_IMAGE_HEADER_MAGIC) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (g_fake.written + size > g_fake.write_fail_at) {
        return g_fake.write_err;
    }
    if (g_fake.written + size > g_fake.flash_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    int target = g_fake.running == 0 ? 1 : 0;
    memcpy(g_fake.flash[target] + g_fake.written, data, size);
    g_fake.written += size;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    g_fake.end_calls++;
    if (!g_fake.open || handle != FAKE_HANDLE) {
        g_fake.misuse++;
        return ESP_ERR_NOT_FOUND;
    }
    g_fake.open = false; /* freed whatever the result */
    if (g_fake.written == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return g_fake.end_err;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    g_fake.abort_calls++;
    if (!g_fake.open || handle != FAKE_HANDLE) {
        g_fake.misuse++;
        return ESP_ERR_NOT_FOUND;
    }
    g_fake.open = false;
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    g_fake.set_boot_calls++;
    int i = part_index(partition);
    if (i < 0) {
        /* ESP-IDF finds the partition by its address, not its pointer. */
        for (int j = 0; j < 2; j++) {
            if (partition != NULL && partition->address == g_fake.parts[j].address) {
                i = j;
            }
        }
    }
    if (i < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_fake.set_boot_err != ESP_OK) {
        return g_fake.set_boot_err;
    }
    if (i == g_fake.running) {
        g_fake.boot_set_to_running++;
    } else {
        g_fake.boot_set_to_update++;
    }
    g_fake.boot = &g_fake.parts[i];
    return ESP_OK;
}

/* esp_app_desc.h */
const esp_app_desc_t *esp_app_get_description(void)
{
    return &g_fake.running_desc;
}

/* esp_heap_caps.h */
void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if (g_fake.malloc_fail) {
        return NULL;
    }
    g_fake.allocs++;
    g_fake.alloc_size = size;
    g_fake.alloc_caps = caps;
    return malloc(size);
}

void heap_caps_free(void *ptr)
{
    if (ptr != NULL) {
        g_fake.frees++;
    }
    free(ptr);
}

/* esp_timer.h */
int64_t esp_timer_get_time(void)
{
    return g_fake.now_us;
}

/* esp_log.h */
void fake_log(char level, const char *tag, const char *fmt, ...)
{
    if (strcmp(tag, "ota") != 0) {
        g_fake.log_other_tag++;
    }
    if (level == 'E') {
        g_fake.log_e++;
    }
    if (g_fake.nlines < FAKE_MAX_LINES) {
        char *line = g_fake.lines[g_fake.nlines++];
        line[0] = level;
        line[1] = ' ';
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + 2, sizeof(g_fake.lines[0]) - 2, fmt, ap);
        va_end(ap);
    }
    if (getenv("OTA_TEST_VERBOSE") && g_fake.nlines < FAKE_MAX_LINES) {
        printf("    log: %s\n", g_fake.lines[g_fake.nlines - 1]);
    }
}
