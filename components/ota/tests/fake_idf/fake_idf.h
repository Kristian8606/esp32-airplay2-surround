/* SPDX-License-Identifier: Apache-2.0
 *
 * Control and record of the host stand-ins for ESP-IDF used by the tests of
 * src/ota.c. They model the documented behaviour of the ESP-IDF calls, not
 * the real code: see README.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_partition.h"

#define FAKE_NEVER ((size_t)-1)
#define FAKE_MAX_LINES 64

typedef struct {
    /* Request body. */
    const uint8_t *body;
    size_t body_len;
    size_t pos;
    size_t chunk;          /* max bytes one recv returns, 0 = as asked */
    size_t close_at;       /* recv returns 0 from this offset */
    size_t fail_at;        /* recv returns HTTPD_SOCK_ERR_FAIL from this offset */
    size_t stall_at;       /* recv returns HTTPD_SOCK_ERR_TIMEOUT from this offset ... */
    int stall_timeouts;    /* ... this many times (-1: forever) */
    int64_t recv_wait_us;  /* time one timed-out recv takes */
    int recv_calls;
    int timeouts;               /* HTTPD_SOCK_ERR_TIMEOUT returned */
    int recv_calls_after_error; /* recv calls after a 0 or FAIL was returned */
    bool recv_error_seen;
    size_t max_recv_ask;
    /* Called once from httpd_req_recv when pos reaches hook_at. */
    void (*hook)(void);
    size_t hook_at;

    /* Partitions: [0] ota_0, [1] ota_1. */
    esp_partition_t parts[2];
    int running;
    bool no_next;
    bool no_running;
    bool no_boot;               /* esp_ota_get_boot_partition returns NULL */
    const esp_partition_t *boot;
    int boot_set_to_update;     /* successful sets to the partition being written */
    int boot_set_to_running;
    int write_while_boot_elsewhere; /* begin/write while boot is not the running partition */
    uint8_t *flash[2];
    size_t flash_size;

    /* OTA calls. */
    int begin_calls, write_calls, end_calls, abort_calls, set_boot_calls;
    int misuse;                 /* calls on a handle that is not open */
    size_t begin_size;
    esp_err_t begin_err, end_err, set_boot_err, write_err;
    size_t write_fail_at;       /* esp_ota_write fails once this many bytes are written */
    bool open;
    size_t written;
    size_t max_write;

    /* Heap. */
    bool malloc_fail;
    int allocs, frees;
    size_t alloc_size;
    uint32_t alloc_caps;

    /* Time. */
    int64_t now_us;

    /* Log lines with their level letter in front. */
    char lines[FAKE_MAX_LINES][300];
    int nlines;
    int log_e, log_other_tag;

    esp_app_desc_t running_desc;
} fake_t;

extern fake_t g_fake;

void fake_reset(const char *running_project);
void fake_free(void);
void fake_set_body(const uint8_t *body, size_t len);
/* Partition that boots next, 0 or 1. */
int fake_boot_index(void);
/* True when no byte of either partition differs from the erased state. */
bool fake_flash_untouched(void);
