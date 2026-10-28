#include "ap2_buffered_fifo.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#if defined(ESP_PLATFORM)
#include "lwip/sockets.h"
#endif
#include "sdkconfig.h"
#if defined(ESP_PLATFORM) && defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif
#include <unistd.h>

#include "audio_diag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network/socket_utils.h"
#include "airplay_log.h"

/* Match Shairport Sync 5.x buffered_read.c: recv at most 4096 bytes, and once
 * more than 16 KiB is queued, sleep 10 ms after each recv. This intentionally
 * lets AutoMix bursts accumulate in the large compressed FIFO without making
 * the TCP reader monopolise the CPU. */
#define FIFO_RECV_CHUNK       4096U
#define FIFO_PACE_THRESHOLD  16384U
#define FIFO_PACE_SLEEP_MS      10U
#define FIFO_MIN_WIRE_LEN       14U
/* Diagnostics: the sender paces buffered audio in bursts several seconds
 * apart. A silence much longer than that, with room in the FIFO, means the
 * sender stopped feeding this connection. */
#define FIFO_STALL_LOG_MS    20000U

static const char *TAG = "aac_fifo";

struct ap2_buffered_fifo {
  uint8_t *buffer;
  size_t capacity;
  size_t read_pos;
  size_t write_pos;
  size_t occupancy;
  uint64_t total_written;
  uint64_t total_read;


  SemaphoreHandle_t fifo_mutex;
  SemaphoreHandle_t not_full;
  SemaphoreHandle_t control_wake;

  bool connected;
  int listen_sock;
  int client_sock;
  uint16_t port;
  volatile bool running;
  TaskHandle_t reader_task;
  volatile uint32_t stream_epoch;

  int task_core;
  int task_priority;
  uint32_t task_stack;
};

static uint32_t next_epoch(ap2_buffered_fifo_t *fifo) {
  uint32_t v = __atomic_add_fetch(&fifo->stream_epoch, 1U, __ATOMIC_ACQ_REL);
  if (v == 0U) {
    __atomic_store_n(&fifo->stream_epoch, 1U, __ATOMIC_RELEASE);
    v = 1U;
  }
  return v;
}

static void signal_all(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  if (fifo->not_full) xSemaphoreGive(fifo->not_full);
  if (fifo->control_wake) xSemaphoreGive(fifo->control_wake);
}

void ap2_buffered_fifo_notify(ap2_buffered_fifo_t *fifo) { signal_all(fifo); }

void ap2_buffered_fifo_wait(ap2_buffered_fifo_t *fifo, uint32_t timeout_ms) {
  if (!fifo || !fifo->control_wake) return;
  TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
  if (ticks == 0) ticks = 1;
  (void)xSemaphoreTake(fifo->control_wake, ticks);
}

static void fifo_discard_all(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  /* Any in-flight read belongs to the previous byte stream. */
  (void)next_epoch(fifo);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

#if defined(ESP_PLATFORM) && defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
/* True when another TCP connection is waiting on the listener (the sender
 * opened a new data connection while this one is still attached). */
static bool listener_has_pending(int listen_sock) {
  if (listen_sock < 0) return false;
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(listen_sock, &rfds);
  struct timeval tv = {0, 0};
  return select(listen_sock + 1, &rfds, NULL, NULL, &tv) > 0 &&
         FD_ISSET(listen_sock, &rfds);
}

static void log_rx_stall(ap2_buffered_fifo_t *fifo, int64_t silent_ms) {
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  const size_t used = fifo->occupancy;
  const uint64_t total = fifo->total_written;
  xSemaphoreGive(fifo->fifo_mutex);
  AP_LOGW(TAG,
           "RX STALL: no data from the sender for %lld ms with %u/%u KiB used "
           "(total rx %llu KiB); new connection pending=%d; internal heap "
           "free=%u min=%u largest=%u",
           (long long)silent_ms, (unsigned)(used / 1024U),
           (unsigned)(fifo->capacity / 1024U), (unsigned long long)(total / 1024U),
           listener_has_pending(fifo->listen_sock) ? 1 : 0,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
#endif

static void tcp_reader_task(void *arg) {
  ap2_buffered_fifo_t *fifo = (ap2_buffered_fifo_t *)arg;
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_TCP_READER,
                                    xPortGetCoreID(), fifo->task_priority, 0U);

  while (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    int c = accept(fifo->listen_sock, (struct sockaddr *)&addr, &alen);
    if (c < 0) {
      if (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) && errno != EAGAIN && errno != EWOULDBLOCK &&
          errno != EINTR)
        AP_LOGW(TAG, "accept errno=%d", errno);
      if (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    /* A newly accepted connection is a fresh sequential byte stream. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    fifo->read_pos = 0;
    fifo->write_pos = 0;
    fifo->occupancy = 0;
    fifo->total_written = 0;
    fifo->total_read = 0;
    fifo->connected = true;
    fifo->client_sock = c;
    (void)next_epoch(fifo);
    xSemaphoreGive(fifo->fifo_mutex);

    AP_LOGI(TAG, "buffered TCP connected fifo=%uKiB",
             (unsigned)(fifo->capacity / 1024U));
    signal_all(fifo);
#if defined(ESP_PLATFORM) && defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
    /* recv wakes at least once a second so a stalled sender is noticed. */
    const struct timeval rx_tv = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rx_tv, sizeof(rx_tv));
    int64_t last_rx_us = esp_timer_get_time();
    bool stall_logged = false;
#endif

    while (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
      size_t write_pos = 0;
      size_t request = 0;
      uint32_t write_epoch = 0;

      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
      if (fifo->occupancy == fifo->capacity) {
        xSemaphoreGive(fifo->fifo_mutex);
        (void)xSemaphoreTake(fifo->not_full, pdMS_TO_TICKS(100));
        continue;
      }

      const size_t free_bytes = fifo->capacity - fifo->occupancy;
      request = free_bytes < FIFO_RECV_CHUNK ? free_bytes : FIFO_RECV_CHUNK;
      const size_t to_end = fifo->capacity - fifo->write_pos;
      if (request > to_end) request = to_end;
      write_pos = fifo->write_pos;
      write_epoch = __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
      xSemaphoreGive(fifo->fifo_mutex);

      const ssize_t n = recv(c, fifo->buffer + write_pos, request, 0);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
#if defined(ESP_PLATFORM) && defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
          const int64_t silent_ms = (esp_timer_get_time() - last_rx_us) / 1000LL;
          if (!stall_logged && silent_ms >= (int64_t)FIFO_STALL_LOG_MS) {
            stall_logged = true;
            log_rx_stall(fifo, silent_ms);
          }
#endif
          continue;
        }
        /* Our own stop/abort shuts the socket down first: log only a close
         * that came from the sender or the network. */
        if (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) &&
            fifo->client_sock == c)
          AP_LOGI(TAG, "buffered TCP: %s from the sender (errno %d)",
                   n == 0 ? "orderly close" : "receive error", n == 0 ? 0 : errno);
        break;
      }
#if defined(ESP_PLATFORM) && defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
      {
        const int64_t now_us = esp_timer_get_time();
        if (stall_logged) {
          AP_LOGI(TAG, "RX resumed after %lld ms without data",
                   (long long)((now_us - last_rx_us) / 1000LL));
          stall_logged = false;
        }
        last_rx_us = now_us;
      }
#endif

      xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
      if (__atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) == write_epoch &&
          fifo->connected) {
        fifo->write_pos += (size_t)n;
        if (fifo->write_pos == fifo->capacity) fifo->write_pos = 0;
        fifo->occupancy += (size_t)n;
        fifo->total_written += (uint64_t)n;
      }
      const bool have_time_to_sleep = fifo->occupancy > FIFO_PACE_THRESHOLD;
      xSemaphoreGive(fifo->fifo_mutex);
      xSemaphoreGive(fifo->control_wake);

      if (have_time_to_sleep) {
        TickType_t ticks = pdMS_TO_TICKS(FIFO_PACE_SLEEP_MS);
        if (ticks == 0) ticks = 1;
        vTaskDelay(ticks);
      }
    }

    /* The reader owns close(); detach the shared descriptor before closing it
     * so stop/abort can never shutdown a recycled lwIP descriptor. */
    xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
    if (fifo->client_sock == c) fifo->client_sock = -1;
    fifo->connected = false;
    fifo->read_pos = fifo->write_pos;
    fifo->occupancy = 0;
    fifo->total_read = fifo->total_written;
    (void)next_epoch(fifo);
    xSemaphoreGive(fifo->fifo_mutex);

    (void)shutdown(c, SHUT_RDWR);
    close(c);
    signal_all(fifo);
    AP_LOGI(TAG, "buffered TCP disconnected");
  }

  __atomic_store_n(&fifo->reader_task, NULL, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

esp_err_t ap2_buffered_fifo_create_with_storage(
    ap2_buffered_fifo_t **out, const ap2_buffered_fifo_config_t *cfg,
    void *storage, size_t storage_bytes) {
  if (!out || !cfg || !storage || cfg->buffer_bytes < 16384U ||
      storage_bytes < cfg->buffer_bytes)
    return ESP_ERR_INVALID_ARG;

  ap2_buffered_fifo_t *fifo = calloc(1, sizeof(*fifo));
  if (!fifo) return ESP_ERR_NO_MEM;
  fifo->buffer = (uint8_t *)storage;
  fifo->capacity = cfg->buffer_bytes;
  fifo->task_core = cfg->task_core;
  fifo->task_priority = cfg->task_priority;
  fifo->task_stack = cfg->task_stack;
  fifo->listen_sock = -1;
  fifo->client_sock = -1;
  fifo->stream_epoch = 1U;
  fifo->fifo_mutex = xSemaphoreCreateMutex();
  fifo->not_full = xSemaphoreCreateBinary();
  fifo->control_wake = xSemaphoreCreateBinary();
  if (!fifo->fifo_mutex || !fifo->not_full ||
      !fifo->control_wake) {
    (void)ap2_buffered_fifo_destroy(fifo);
    return ESP_ERR_NO_MEM;
  }
  *out = fifo;
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_destroy(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return ESP_OK;
  if (fifo->fifo_mutex) {
    ap2_buffered_fifo_stop(fifo);
  } else {
    __atomic_store_n(&fifo->running, false, __ATOMIC_RELEASE);
  }
  if (__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE)) {
    ESP_LOGE(TAG, "destroy refused: TCP reader still active");
    return ESP_ERR_TIMEOUT;
  }
  if (fifo->fifo_mutex) vSemaphoreDelete(fifo->fifo_mutex);
  if (fifo->not_full) vSemaphoreDelete(fifo->not_full);
  if (fifo->control_wake) vSemaphoreDelete(fifo->control_wake);
  free(fifo);
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_start(ap2_buffered_fifo_t *fifo,
                                  uint16_t requested_port,
                                  uint16_t *bound_port) {
  if (!fifo) return ESP_ERR_INVALID_ARG;
  if (__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    if (bound_port) *bound_port = fifo->port;
    return ESP_OK;
  }
  if (__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE))
    return ESP_ERR_INVALID_STATE;

  uint16_t bound = requested_port;
  fifo->listen_sock =
      socket_utils_bind_tcp_listener(requested_port, 1, true, &bound);
  if (fifo->listen_sock < 0) return ESP_FAIL;
  fifo->port = bound;
  __atomic_store_n(&fifo->running, true, __ATOMIC_RELEASE);
  if (xTaskCreatePinnedToCore(tcp_reader_task, "aac_fifo_rx", fifo->task_stack,
                              fifo, fifo->task_priority, &fifo->reader_task,
                              fifo->task_core) != pdPASS) {
    __atomic_store_n(&fifo->running, false, __ATOMIC_RELEASE);
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
    return ESP_FAIL;
  }
  if (bound_port) *bound_port = bound;
  return ESP_OK;
}

void ap2_buffered_fifo_stop(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  __atomic_store_n(&fifo->running, false, __ATOMIC_RELEASE);

  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  (void)next_epoch(fifo);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);

  if (fifo->listen_sock >= 0) shutdown(fifo->listen_sock, SHUT_RDWR);
  for (int i = 0;
       __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) && i < 100; ++i)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (!__atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) &&
      fifo->listen_sock >= 0) {
    close(fifo->listen_sock);
    fifo->listen_sock = -1;
  }
  fifo->port = 0;
}

bool ap2_buffered_fifo_is_idle(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return true;
  return !__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&fifo->reader_task, __ATOMIC_ACQUIRE) == NULL;
}

void ap2_buffered_fifo_clear(ap2_buffered_fifo_t *fifo) {
  if (!fifo) return;
  /* FLUSHBUFFERED never calls this. It is reserved for stopped session/codec
   * boundaries, so a hard byte reset cannot split a live framed packet. */
  fifo_discard_all(fifo);
}

/* Abort only the current buffered TCP client after fatal framing corruption.
 * The listening socket remains available for the RTSP session. */
static void ap2_buffered_fifo_abort_client(ap2_buffered_fifo_t *fifo) {
  if (!fifo || !fifo->fifo_mutex) return;

  int client = -1;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  client = fifo->client_sock;
  fifo->client_sock = -1;
  fifo->connected = false;
  fifo->read_pos = fifo->write_pos;
  fifo->occupancy = 0;
  fifo->total_read = fifo->total_written;
  (void)next_epoch(fifo);
  if (client >= 0) (void)shutdown(client, SHUT_RDWR);
  xSemaphoreGive(fifo->fifo_mutex);
  signal_all(fifo);
}

size_t ap2_buffered_fifo_capacity(const ap2_buffered_fifo_t *fifo) {
  return fifo ? fifo->capacity : 0U;
}

bool ap2_buffered_fifo_epoch_is_current(const ap2_buffered_fifo_t *fifo,
                                        uint32_t stream_epoch) {
  return fifo && __atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) && stream_epoch ==
      __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
}

void ap2_buffered_fifo_get_usage(ap2_buffered_fifo_t *fifo,
                                 ap2_buffered_fifo_usage_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!fifo) return;

  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  out->capacity_bytes = fifo->capacity;
  out->used_bytes = fifo->occupancy;
  out->bytes_received = fifo->total_written;
  xSemaphoreGive(fifo->fifo_mutex);

}

/* Called with fifo_mutex held. No cursor or media-policy change. */
static void fifo_copy_at(const ap2_buffered_fifo_t *fifo, size_t pos,
                         uint8_t *dst, size_t len) {
  size_t first = fifo->capacity - pos;
  if (first > len) first = len;
  memcpy(dst, fifo->buffer + pos, first);
  if (first < len) memcpy(dst + first, fifo->buffer, len - first);
}

esp_err_t ap2_buffered_fifo_peek_head(ap2_buffered_fifo_t *fifo,
                                      size_t block_capacity,
                                      ap2_buffered_fifo_head_t *head) {
  if (!fifo || !head || block_capacity < sizeof(head->header))
    return ESP_ERR_INVALID_ARG;

  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  if (!__atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE)) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (fifo->occupancy < 2U) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_TIMEOUT;
  }
  uint8_t length_bytes[2];
  fifo_copy_at(fifo, fifo->read_pos, length_bytes, sizeof(length_bytes));
  const uint16_t wire_len = ((uint16_t)length_bytes[0] << 8) | length_bytes[1];
  if (wire_len < FIFO_MIN_WIRE_LEN || wire_len > fifo->capacity ||
      (size_t)wire_len - 2U > block_capacity) {
    xSemaphoreGive(fifo->fifo_mutex);
    AP_LOGW(TAG, "invalid buffered block length=%u; aborting client to restore framing",
             (unsigned)wire_len);
    ap2_buffered_fifo_abort_client(fifo);
    return ESP_ERR_INVALID_SIZE;
  }
  if (fifo->occupancy < wire_len) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_TIMEOUT;
  }

  head->wire_len = wire_len;
  head->stream_epoch = __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE);
  head->read_pos = fifo->read_pos;
  head->read_serial = fifo->total_read;
  fifo_copy_at(fifo, (fifo->read_pos + 2U) % fifo->capacity,
               head->header, sizeof(head->header));
  xSemaphoreGive(fifo->fifo_mutex);
  return ESP_OK;
}

/* Caller holds fifo_mutex. */
static bool fifo_head_is_current_locked(const ap2_buffered_fifo_t *fifo,
                                         const ap2_buffered_fifo_head_t *head) {
  return __atomic_load_n(&fifo->running, __ATOMIC_ACQUIRE) && head->stream_epoch ==
      __atomic_load_n(&fifo->stream_epoch, __ATOMIC_ACQUIRE) &&
      head->read_pos == fifo->read_pos && head->read_serial == fifo->total_read &&
      head->wire_len >= FIFO_MIN_WIRE_LEN && head->wire_len <= fifo->occupancy;
}

esp_err_t ap2_buffered_fifo_visit_head(ap2_buffered_fifo_t *fifo,
                                       const ap2_buffered_fifo_head_t *head,
                                       ap2_buffered_fifo_head_visit_t visit,
                                       void *context) {
  if (!fifo || !head || !visit) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  const bool valid = fifo_head_is_current_locked(fifo, head);
  if (valid) visit(context);
  xSemaphoreGive(fifo->fifo_mutex);
  return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t fifo_consume_head(ap2_buffered_fifo_t *fifo,
                                    const ap2_buffered_fifo_head_t *head,
                                    uint8_t *dst, size_t capacity) {
  if (!fifo || !head || head->wire_len < FIFO_MIN_WIRE_LEN)
    return ESP_ERR_INVALID_ARG;
  if (dst && (size_t)head->wire_len - 2U > capacity)
    return ESP_ERR_INVALID_SIZE;

  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  if (!fifo_head_is_current_locked(fifo, head)) {
    xSemaphoreGive(fifo->fifo_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (dst) {
    fifo_copy_at(fifo, (fifo->read_pos + 2U) % fifo->capacity,
                 dst, (size_t)head->wire_len - 2U);
  }
  fifo->read_pos = (fifo->read_pos + head->wire_len) % fifo->capacity;
  fifo->occupancy -= head->wire_len;
  fifo->total_read += head->wire_len;
  xSemaphoreGive(fifo->fifo_mutex);
  xSemaphoreGive(fifo->not_full);
  return ESP_OK;
}

esp_err_t ap2_buffered_fifo_discard_head(ap2_buffered_fifo_t *fifo,
                                         const ap2_buffered_fifo_head_t *head) {
  return fifo_consume_head(fifo, head, NULL, 0U);
}

esp_err_t ap2_buffered_fifo_read_head(ap2_buffered_fifo_t *fifo,
                                      const ap2_buffered_fifo_head_t *head,
                                      uint8_t *block_storage,
                                      size_t block_capacity) {
  if (!block_storage) return ESP_ERR_INVALID_ARG;
  return fifo_consume_head(fifo, head, block_storage, block_capacity);
}

esp_err_t ap2_buffered_fifo_copy_head(ap2_buffered_fifo_t *fifo,
                                      const ap2_buffered_fifo_head_t *head,
                                      uint8_t *block_storage,
                                      size_t block_capacity) {
  if (!fifo || !head || !block_storage || head->wire_len < FIFO_MIN_WIRE_LEN)
    return ESP_ERR_INVALID_ARG;
  if ((size_t)head->wire_len - 2U > block_capacity)
    return ESP_ERR_INVALID_SIZE;
  xSemaphoreTake(fifo->fifo_mutex, portMAX_DELAY);
  const bool valid = fifo_head_is_current_locked(fifo, head);
  if (valid)
    fifo_copy_at(fifo, (fifo->read_pos + 2U) % fifo->capacity,
                 block_storage, (size_t)head->wire_len - 2U);
  xSemaphoreGive(fifo->fifo_mutex);
  return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
