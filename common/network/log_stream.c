/**
 * WebSocket-based log streaming over HTTP.
 *
 * Intercepts ESP-IDF log output via esp_log_set_vprintf(), stores lines
 * in a ring buffer, and broadcasts them to any connected WebSocket
 * client on /ws/logs.  UART output is preserved.
 */

#include "log_stream.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "airplay_log.h"
#include "sdkconfig.h"

/* Multichannel experiment: FDK AAC keeps Core0 busy at a higher priority;
 * the log broadcaster runs on Core1 (next to WiFi/LwIP) instead. */
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#define LOG_WS_CORE 1
#else
#define LOG_WS_CORE 0
#endif

/* Ring buffer size — must be power of two for masking. */
#define LOG_RING_SIZE 8192
#define LOG_RING_MASK (LOG_RING_SIZE - 1)

#define BROADCAST_TASK_STACK          4096
#define BROADCAST_TASK_PRIORITY       2
#define BROADCAST_ACTIVE_INTERVAL_MS  100
#define BROADCAST_IDLE_INTERVAL_MS    1000
#define MAX_SEND_CHUNK                1024
#define WS_RX_MAX_PAYLOAD             128

static char *s_ring;
static size_t s_head; /* next write position  */
static size_t s_tail; /* next read position   */
static SemaphoreHandle_t s_mutex;

/* Serialize scheduling and detach; HTTPD work never takes this mutex. */
static SemaphoreHandle_t s_server_mutex;
static httpd_handle_t s_server;
static TaskHandle_t s_broadcast_task;
static bool s_initialized;
static bool s_initializing;
static bool s_work_pending;
static bool s_has_viewers;
static bool s_detaching;

typedef struct {
  httpd_handle_t server;
  char payload[MAX_SEND_CHUNK + 3];
} log_broadcast_work_t;

/* Only HTTPD work touches these bytes; detach resets them after draining. */
static uint8_t s_utf8_pending[3];
static size_t s_utf8_pending_len;

/* Emit complete canonical UTF-8 only. Replace lost/malformed bytes in place
 * without expansion and retain a valid incomplete suffix for the next chunk. */
static size_t sanitize_utf8(char *buf, size_t len) {
  size_t read = 0, written = 0;
  s_utf8_pending_len = 0;
  while (read < len) {
    uint8_t lead = (uint8_t)buf[read];
    size_t width = lead <= 0x7f ? 1 :
        (lead >= 0xc2 && lead <= 0xdf ? 2 :
         (lead >= 0xe0 && lead <= 0xef ? 3 :
          (lead >= 0xf0 && lead <= 0xf4 ? 4 : 0)));
    bool valid = width != 0;
    size_t available = len - read;
    size_t check = available < width ? available : width;
    for (size_t i = 1; valid && i < check; ++i) {
      uint8_t byte = (uint8_t)buf[read + i];
      valid = byte >= 0x80 && byte <= 0xbf;
      if (i == 1) {
        if (lead == 0xe0 && byte < 0xa0) valid = false; /* overlong */
        if (lead == 0xed && byte > 0x9f) valid = false; /* surrogate */
        if (lead == 0xf0 && byte < 0x90) valid = false; /* overlong */
        if (lead == 0xf4 && byte > 0x8f) valid = false; /* > U+10FFFF */
      }
    }
    if (!valid) {
      buf[written++] = '?';
      ++read;
    } else if (available < width) {
      memcpy(s_utf8_pending, buf + read, available);
      s_utf8_pending_len = available; /* A valid incomplete suffix is <= 3. */
      break;
    } else {
      memmove(buf + written, buf + read, width);
      written += width;
      read += width;
    }
  }
  return written;
}

static vprintf_like_t s_orig_vprintf;

/* ------------------------------------------------------------------ */
/*  Ring buffer helpers (protected by s_mutex)                         */
/* ------------------------------------------------------------------ */

static inline size_t ring_used(void) {
  return (s_head - s_tail) & LOG_RING_MASK;
}

static void ring_write(const char *data, size_t len) {
  const size_t capacity = LOG_RING_SIZE - 1;
  if (len > capacity) {
    /* Retain exactly the suffix that the bytewise overwrite kept. */
    const size_t skipped = len - capacity;
    data += skipped;
    len = capacity;
    s_head = (s_head + skipped) & LOG_RING_MASK;
    s_tail = s_head;
  }
  const size_t free_bytes = capacity - ring_used();
  if (len > free_bytes)
    s_tail = (s_tail + len - free_bytes) & LOG_RING_MASK;
  size_t first = LOG_RING_SIZE - s_head;
  if (first > len) first = len;
  memcpy(s_ring + s_head, data, first);
  memcpy(s_ring, data + first, len - first);
  s_head = (s_head + len) & LOG_RING_MASK;
}

static size_t ring_read(char *buf, size_t max) {
  size_t avail = ring_used();
  if (avail > max) avail = max;
  size_t first = LOG_RING_SIZE - s_tail;
  if (first > avail) first = avail;
  memcpy(buf, s_ring + s_tail, first);
  memcpy(buf + first, s_ring, avail - first);
  s_tail = (s_tail + avail) & LOG_RING_MASK;
  return avail;
}

/* ------------------------------------------------------------------ */
/*  Log hook — called from task context by esp_log       */
/* ------------------------------------------------------------------ */

static int log_vprintf_hook(const char *fmt, va_list args) {
  /* A va_list may be consumed by vprintf. Give UART its own copy so the
   * original list remains valid for the WebSocket copy below. */
  va_list uart_args;
  va_copy(uart_args, args);
  int ret = s_orig_vprintf(fmt, uart_args);
  va_end(uart_args);

  /* Format into a stack buffer and push to ring. */
  char buf[256];
  va_list copy;
  va_copy(copy, args);
  int len = vsnprintf(buf, sizeof(buf), fmt, copy);
  va_end(copy);

  if (len > 0) {
    if ((size_t)len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
      ring_write(buf, (size_t)len);
      xSemaphoreGive(s_mutex);
    }
    /* If the mutex is held we silently drop — better than blocking a log call.
     */
  }
  return ret;
}

/* ------------------------------------------------------------------ */
/*  WebSocket handler                                                  */
/* ------------------------------------------------------------------ */

static void close_ws_session(httpd_handle_t server, int fd, const char *reason) {
  if (!server || fd < 0) {
    return;
  }

  esp_err_t err = httpd_sess_trigger_close(server, fd);
  if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
    AP_LOGD("log_stream", "WS close fd=%d (%s) failed: %s", fd,
             reason ? reason : "unknown", esp_err_to_name(err));
  }
}

static esp_err_t ws_log_handler(httpd_req_t *req) {
  /* The server completes the WebSocket handshake internally; depending on the
   * IDF build the handshake GET may or may not reach here. Return OK for it. */
  if (req->method == HTTP_GET) {
    return ESP_OK;
  }

  const int fd = httpd_req_to_sockfd(req);
  httpd_ws_frame_t frame = {0};

  /* First call only parses the WS header and reports the payload length. */
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    /* A dead/stale browser socket must not remain in the HTTPD session list;
     * otherwise the broadcast task keeps sending to it and HTTPD repeatedly
     * attempts to parse a stream which is no longer a valid WebSocket. */
    close_ws_session(req->handle, fd, "recv-header");
    return ESP_OK;
  }

  /* /ws/logs is receive-only from the browser's point of view. Normal browser
   * control frames are tiny. If a client sends a larger payload, close the
   * session rather than leaving an unconsumed frame in the socket. */
  if (frame.len > WS_RX_MAX_PAYLOAD) {
    AP_LOGD("log_stream", "Closing WS fd=%d: unexpected payload %u bytes",
             fd, (unsigned)frame.len);
    close_ws_session(req->handle, fd, "oversize-frame");
    return ESP_OK;
  }

  if (frame.len > 0) {
    uint8_t buf[WS_RX_MAX_PAYLOAD];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
      close_ws_session(req->handle, fd, "recv-payload");
      return ESP_OK;
    }
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    close_ws_session(req->handle, fd, "peer-close");
  }
  return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Broadcast task                                                     */
/* ------------------------------------------------------------------ */

/* Run session lookup and every frame write on HTTPD's own task. The work
 * owns its payload until synchronous sends finish; only one work item exists. */
static void broadcast_work(void *arg) {
  log_broadcast_work_t *work = arg;
  int fds[CONFIG_LWIP_MAX_SOCKETS];
  size_t fd_count = CONFIG_LWIP_MAX_SOCKETS;
  size_t ws_count = 0;
  if (httpd_get_client_list(work->server, &fd_count, fds) == ESP_OK) {
    for (size_t i = 0; i < fd_count; ++i) {
      if (httpd_ws_get_fd_info(work->server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
        fds[ws_count++] = fds[i];
    }
  }
  __atomic_store_n(&s_has_viewers, ws_count != 0, __ATOMIC_RELEASE);
  size_t len = 0;
  if (ws_count && xSemaphoreTake(s_mutex, 0) == pdTRUE) {
    memcpy(work->payload, s_utf8_pending, s_utf8_pending_len);
    len = s_utf8_pending_len + ring_read(
        work->payload + s_utf8_pending_len, MAX_SEND_CHUNK);
    xSemaphoreGive(s_mutex);
  }
  if (len) len = sanitize_utf8(work->payload, len);
  if (len) {
    /* TEXT remains compatible with UI assets retained by firmware-only OTA. */
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)work->payload,
        .len = len,
    };
    for (size_t i = 0; i < ws_count; ++i) {
      if (httpd_ws_send_frame_async(work->server, fds[i], &frame) != ESP_OK)
        close_ws_session(work->server, fds[i], "send-failed");
    }
  }
  free(work);
  /* Release only after the last use of the server, including close requests. */
  __atomic_store_n(&s_work_pending, false, __ATOMIC_RELEASE);
}

static void broadcast_task(void *arg) {
  (void)arg;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(__atomic_load_n(&s_has_viewers, __ATOMIC_ACQUIRE)
        ? BROADCAST_ACTIVE_INTERVAL_MS : BROADCAST_IDLE_INTERVAL_MS));
    xSemaphoreTake(s_server_mutex, portMAX_DELAY);
    if (s_server && !__atomic_load_n(&s_work_pending, __ATOMIC_ACQUIRE)) {
      log_broadcast_work_t *work = heap_caps_malloc(sizeof(*work),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!work) work = malloc(sizeof(*work));
      if (work) {
        work->server = s_server;
        __atomic_store_n(&s_work_pending, true, __ATOMIC_RELEASE);
        /* This project's nonblocking HTTPD queue fails fast when full. */
        if (httpd_queue_work(s_server, broadcast_work, work) != ESP_OK) {
          free(work);
          __atomic_store_n(&s_work_pending, false, __ATOMIC_RELEASE);
        }
      }
    }
    xSemaphoreGive(s_server_mutex);
  }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t log_stream_init(void) {
  if (__atomic_load_n(&s_initialized, __ATOMIC_ACQUIRE)) return ESP_OK;
  bool expected = false;
  if (!__atomic_compare_exchange_n(&s_initializing, &expected, true, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return ESP_ERR_INVALID_STATE;

  s_mutex = xSemaphoreCreateMutex();
  s_server_mutex = xSemaphoreCreateMutex();
  if (!s_mutex || !s_server_mutex) goto no_memory;
#ifdef CONFIG_SPIRAM
  s_ring = heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!s_ring) s_ring = malloc(LOG_RING_SIZE);
  if (!s_ring) goto no_memory;
  s_head = s_tail = 0;
  s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
  __atomic_store_n(&s_initialized, true, __ATOMIC_RELEASE);
  __atomic_store_n(&s_initializing, false, __ATOMIC_RELEASE);
  return ESP_OK;

no_memory:
  if (s_mutex) vSemaphoreDelete(s_mutex);
  if (s_server_mutex) vSemaphoreDelete(s_server_mutex);
  s_mutex = s_server_mutex = NULL;
  __atomic_store_n(&s_initializing, false, __ATOMIC_RELEASE);
  return ESP_ERR_NO_MEM;
}

esp_err_t log_stream_register(httpd_handle_t server) {
  if (!server) return ESP_ERR_INVALID_ARG;
  if (!__atomic_load_n(&s_initialized, __ATOMIC_ACQUIRE))
    return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(s_server_mutex, portMAX_DELAY);
  if (s_detaching) {
    xSemaphoreGive(s_server_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (s_server) {
    esp_err_t result = s_server == server ? ESP_OK : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_server_mutex);
    return result;
  }
  httpd_uri_t ws_uri = {
      .uri = "/ws/logs",
      .method = HTTP_GET,
      .handler = ws_log_handler,
      .is_websocket = true,
  };
  esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
  if (err == ESP_OK && !s_broadcast_task) {
    /* Only the stack is external; the WithCaps API keeps the TCB internal. */
    if (xTaskCreatePinnedToCoreWithCaps(
            broadcast_task, "log_ws", BROADCAST_TASK_STACK, NULL,
            BROADCAST_TASK_PRIORITY, &s_broadcast_task, LOG_WS_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      httpd_unregister_uri_handler(server, "/ws/logs", HTTP_GET);
      err = ESP_ERR_NO_MEM;
    }
  }
  if (err == ESP_OK) s_server = server;
  xSemaphoreGive(s_server_mutex);
  if (err != ESP_OK)
    ESP_LOGE("log_stream", "Register /ws/logs failed: %s", esp_err_to_name(err));
  else
    ESP_LOGI("log_stream", "Log streaming on /ws/logs");
  return err;
}

