#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct ap2_buffered_fifo ap2_buffered_fifo_t;

/* Transport token for one complete frame at the sole consumer's cursor.
 * header contains the first 12 body bytes verbatim; media interpretation stays
 * in audio_receiver. wire_len includes the two length bytes. */
typedef struct {
  uint16_t wire_len;
  uint8_t header[12];
  uint32_t stream_epoch;
  size_t read_pos;
  uint64_t read_serial;
} ap2_buffered_fifo_head_t;

typedef struct {
  size_t buffer_bytes;
  int task_core;
  int task_priority;
  uint32_t task_stack;
} ap2_buffered_fifo_config_t;

typedef struct {
  size_t capacity_bytes;
  size_t used_bytes;
  uint64_t bytes_received;
} ap2_buffered_fifo_usage_t;

esp_err_t ap2_buffered_fifo_create_with_storage(
    ap2_buffered_fifo_t **out, const ap2_buffered_fifo_config_t *cfg,
    void *storage, size_t storage_bytes);
esp_err_t ap2_buffered_fifo_destroy(ap2_buffered_fifo_t *fifo);

esp_err_t ap2_buffered_fifo_start(ap2_buffered_fifo_t *fifo,
                                  uint16_t requested_port,
                                  uint16_t *bound_port);
void ap2_buffered_fifo_stop(ap2_buffered_fifo_t *fifo);
bool ap2_buffered_fifo_is_idle(ap2_buffered_fifo_t *fifo);

/* Hard session-boundary reset. Call only while the buffered transport is
 * stopped/idle; live FLUSHBUFFERED never rewinds or purges this byte FIFO. */
void ap2_buffered_fifo_clear(ap2_buffered_fifo_t *fifo);

size_t ap2_buffered_fifo_capacity(const ap2_buffered_fifo_t *fifo);
/* Revalidate an already-consumed packet before publishing decoded PCM. */
bool ap2_buffered_fifo_epoch_is_current(const ap2_buffered_fifo_t *fifo,
                                        uint32_t stream_epoch);
void ap2_buffered_fifo_get_usage(ap2_buffered_fifo_t *fifo,
                                 ap2_buffered_fifo_usage_t *out);

/* Wake the single packet consumer after a control-plane change. */
void ap2_buffered_fifo_notify(ap2_buffered_fifo_t *fifo);
void ap2_buffered_fifo_wait(ap2_buffered_fifo_t *fifo, uint32_t timeout_ms);

/* Nonblocking inspect. ESP_ERR_TIMEOUT means an incomplete frame: no bytes
 * have been consumed. Invalid framing aborts the current client. */
esp_err_t ap2_buffered_fifo_peek_head(ap2_buffered_fifo_t *fifo,
                                      size_t block_capacity,
                                      ap2_buffered_fifo_head_t *head);

/* Validate a peek token and run a short metadata transaction while the token
 * remains current. The callback must not wait, log, decode or call FIFO APIs.
 * It leaves the cursor unchanged; media policy belongs to the caller. */
typedef void (*ap2_buffered_fifo_head_visit_t)(void *context);
esp_err_t ap2_buffered_fifo_visit_head(ap2_buffered_fifo_t *fifo,
                                       const ap2_buffered_fifo_head_t *head,
                                       ap2_buffered_fifo_head_visit_t visit,
                                       void *context);

/* Commit exactly the inspected head. A reconnect, reset or cursor advance
 * invalidates the token (ESP_ERR_INVALID_STATE). No partial frame commits.
 * Discard only advances metadata; read performs one contiguous body copy
 * (two memcpy spans when the circular storage wraps). */
esp_err_t ap2_buffered_fifo_discard_head(ap2_buffered_fifo_t *fifo,
                                         const ap2_buffered_fifo_head_t *head);
esp_err_t ap2_buffered_fifo_read_head(ap2_buffered_fifo_t *fifo,
                                      const ap2_buffered_fifo_head_t *head,
                                      uint8_t *block_storage,
                                      size_t block_capacity);
esp_err_t ap2_buffered_fifo_copy_head(ap2_buffered_fifo_t *fifo,
                                      const ap2_buffered_fifo_head_t *head,
                                      uint8_t *block_storage,
                                      size_t block_capacity);
