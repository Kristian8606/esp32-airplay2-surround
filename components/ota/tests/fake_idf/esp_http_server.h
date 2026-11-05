/* SPDX-License-Identifier: Apache-2.0
 * Host stand-in for ESP-IDF's esp_http_server.h. */
#pragma once
#include <stddef.h>
#include "esp_err.h"

#define HTTPD_SOCK_ERR_FAIL     -1
#define HTTPD_SOCK_ERR_INVALID  -2
#define HTTPD_SOCK_ERR_TIMEOUT  -3

typedef struct httpd_req {
    size_t content_len;
} httpd_req_t;

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
