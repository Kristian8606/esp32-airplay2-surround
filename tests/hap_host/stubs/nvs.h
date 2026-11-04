#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);
