#include "rtsp_server.h"

#include <errno.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "audio_diag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "rtsp_conn.h"
#include "rtsp_crypto.h"
#include "rtsp_handlers.h"
#include "rtsp_message.h"

#include "rtsp_events.h"
#include "airplay_log.h"

static const char *TAG = "rtsp_server";

#define RTSP_PORT           7000
#define RTSP_BUFFER_INITIAL 4096
#define RTSP_BUFFER_LARGE   ((size_t)256 * 1024)

#define CLIENT_STACK_SIZE 10240
#define SERVER_STACK_SIZE 4096
#define RTSP_STOP_SEND_TIMEOUT_MS 200

/* RTSP control must run immediately after lwIP has delivered socket data.
 * ESP-IDF keeps the TCP/IP task above application code (prio 18 on ESP32-S3),
 * so place the active RTSP client directly below it.  The accept/listen task is
 * not latency-critical and stays at the normal application priority. */
#define RTSP_CLIENT_TASK_PRIORITY 17
#define RTSP_SERVER_TASK_PRIORITY 5

static int server_socket = -1;
static _Atomic bool server_task_live;
static _Atomic bool server_running;
static _Atomic(SemaphoreHandle_t) lifecycle_mutex;

// RTSP tasks are restartable. Use dynamic TCB allocation so
// reconnect/start-stop paths cannot reuse static task memory before FreeRTOS
// idle finishes deletion.

// Client slot for tracking connections
typedef struct {
  rtsp_conn_t *conn;
  _Atomic bool live;
  int socket;
  _Atomic bool should_stop;
  _Atomic bool is_old; // Marked as old client being killed
} client_slot_t;

static client_slot_t clients[2] = {{.socket = -1}, {.socket = -1}}; // Current and old
static int current_slot = 0;

/* Shairport principal_conn: the one connection allowed to drive the global
 * audio / PTP / event-port state. */
static rtsp_conn_t *s_owner;
static portMUX_TYPE s_owner_lock = portMUX_INITIALIZER_UNLOCKED;

static void signal_client_stop(int slot_idx);
static bool wait_client_stopped(int slot_idx, TickType_t timeout_ticks);

/* Only stopping sockets get this timeout. Live RTSP response sends retain
 * their existing behaviour; lwIP FIN-allocation retries must not hold the
 * lifecycle mutex for its default 20 seconds. Retry polls can overshoot. */
static void bound_socket_stop(int fd) {
  const struct timeval tv = {
      .tv_sec = 0, .tv_usec = RTSP_STOP_SEND_TIMEOUT_MS * 1000};
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void detach_client_socket(client_slot_t *slot) {
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  int socket = slot->socket;
  slot->socket = -1;
  xSemaphoreGive(lifecycle_mutex);
  if (socket >= 0) {
    bound_socket_stop(socket);
    close(socket);
  }
}

static void detach_client_conn(client_slot_t *slot) {
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  slot->conn = NULL;
  xSemaphoreGive(lifecycle_mutex);
}

// Helper to grow buffer
static uint8_t *grow_buffer(uint8_t *old_buf, size_t old_size, size_t new_size,
                            size_t data_len) {
  (void)old_size;
  /* Keep one guard byte beyond the logical receive capacity.
   * process_rtsp_buffer() temporarily writes buffer[total_len] = '\0'; when
   * a request exactly fills the logical buffer this sentinel must still be
   * inside the allocation. */
  uint8_t *new_buf =
      heap_caps_malloc(new_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!new_buf) {
    new_buf = malloc(new_size + 1U);
  }
  if (!new_buf) {
    return NULL;
  }
  if (old_buf && data_len > 0) {
    memcpy(new_buf, old_buf, data_len);
  }
  free(old_buf);
  return new_buf;
}

// Process buffered RTSP requests
static void process_rtsp_buffer(client_slot_t *slot, uint8_t *buffer,
                                size_t *buf_len) {
  while (*buf_len > 0 && !slot->should_stop) {
    const uint8_t *header_end = rtsp_find_header_end(buffer, *buf_len);
    if (!header_end) {
      break;
    }

    size_t header_len = (size_t)(header_end - buffer) + 4;
    /* One guard byte already exists; inspect only the header in place. */
    uint8_t header_saved = buffer[header_len];
    buffer[header_len] = '\0';
    int content_len = rtsp_parse_content_length((const char *)buffer);
    buffer[header_len] = header_saved;
    if (content_len < 0 || header_len > RTSP_BUFFER_LARGE ||
        (size_t)content_len > RTSP_BUFFER_LARGE - header_len) {
      slot->conn->close_after_response = true;
      return; /* Never reinterpret an invalid body as a new request. */
    }
    size_t total_len = header_len + (size_t)content_len;
    if (*buf_len < total_len) break;

    // Null-terminate so strcasestr in parse_raw_header won't read past
    // the message boundary. The allocation always has one guard byte beyond
    // the logical receive capacity, including the exact-capacity case.
    uint8_t saved = buffer[total_len];
    buffer[total_len] = '\0';
    rtsp_dispatch(slot->socket, slot->conn, buffer, total_len);
    buffer[total_len] = saved;

    if (slot->conn->close_after_response) {
      // Shairport handle_teardown_2() sets conn->stop after returning the
      // TEARDOWN response. Do not process any pipelined request belonging to
      // the connection that has just been torn down.
      *buf_len = 0;
      return;
    }

    if (*buf_len > total_len) {
      memmove(buffer, buffer + total_len, *buf_len - total_len);
    }
    *buf_len -= total_len;
  }
}

// Client task
static void client_task(void *pvParameters) {
  int slot_idx = (int)(intptr_t)pvParameters;
  client_slot_t *slot = &clients[slot_idx];

  // Create connection state
  rtsp_conn_t *conn = rtsp_conn_create();
  if (!conn) {
    ESP_LOGE(TAG, "Failed to create connection state");
    detach_client_socket(slot);
    slot->live = false;
    vTaskDelete(NULL);
    return;
  }
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  slot->conn = conn;
  xSemaphoreGive(lifecycle_mutex);

  // Sender IP: PTP timing peer, event-port filter, realtime retransmits
  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  if (getpeername(slot->socket, (struct sockaddr *)&peer_addr, &peer_len) ==
      0) {
    conn->client_ip = peer_addr.sin_addr.s_addr;
    AP_LOGI(TAG, "Client IP: %u.%u.%u.%u",
             (unsigned int)(conn->client_ip & 0xFF),
             (unsigned int)((conn->client_ip >> 8) & 0xFF),
             (unsigned int)((conn->client_ip >> 16) & 0xFF),
             (unsigned int)((conn->client_ip >> 24) & 0xFF));
  }

  // Allocate buffer
  size_t buf_capacity = RTSP_BUFFER_INITIAL;
  /* +1 guard byte for the temporary RTSP message terminator. */
  uint8_t *buffer = malloc(buf_capacity + 1U);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate buffer");
    detach_client_conn(slot);
    rtsp_conn_free(conn);
    detach_client_socket(slot);
    slot->live = false;
    vTaskDelete(NULL);
    return;
  }

  size_t buf_len = 0;
  uint8_t *decrypted = NULL; /* Lazy bounded encrypted-frame staging. */

  // Socket timeout for stop signal responsiveness
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(slot->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Disable Nagle: RTSP control commands (volume, pause) are tiny and must
  // not wait for coalescing/delayed-ACK, which adds tens to hundreds of ms of
  // latency to every command on this connection.
  int nodelay = 1;
  setsockopt(slot->socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  /* Shairport rtsp.c: TCP keepalive on every RTSP connection (timeout 120 s:
   * idle 95 s, then 5 probes 5 s apart). A sender that vanishes without
   * closing (out of Wi-Fi, battery) is detected and its session released:
   * recv() fails and the normal cleanup below runs. */
  int keepalive = 1;
  setsockopt(slot->socket, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
  int keep_idle = 95, keep_intvl = 5, keep_cnt = 5;
  setsockopt(slot->socket, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
  setsockopt(slot->socket, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
  setsockopt(slot->socket, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));
#endif

  while (server_running && !slot->should_stop) {
    if (conn->encrypted_mode) {
      // Encrypted mode
      while (server_running && conn->encrypted_mode && !slot->should_stop) {
        /* Read one bounded frame separately so a frame that also contains
         * the next request can straddle the receive-capacity boundary. */
        if (!decrypted) {
          decrypted = heap_caps_malloc(RTSP_ENCRYPTED_BLOCK_MAX,
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (!decrypted) decrypted = malloc(RTSP_ENCRYPTED_BLOCK_MAX);
          if (!decrypted) goto cleanup;
        }
        int block_len = rtsp_crypto_read_block(
            slot->socket, conn, decrypted, RTSP_ENCRYPTED_BLOCK_MAX);
        if (block_len <= 0) goto cleanup;
        size_t offset = 0;
        while (offset < (size_t)block_len) {
          if (buf_len == buf_capacity) {
            if (buf_capacity == RTSP_BUFFER_LARGE) goto cleanup;
            size_t new_cap = buf_capacity * 2;
            if (new_cap > RTSP_BUFFER_LARGE) new_cap = RTSP_BUFFER_LARGE;
            uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
            if (!new_buf) goto cleanup;
            buffer = new_buf;
            buf_capacity = new_cap;
          }
          size_t chunk = (size_t)block_len - offset;
          if (chunk > buf_capacity - buf_len) chunk = buf_capacity - buf_len;
          memcpy(buffer + buf_len, decrypted + offset, chunk);
          buf_len += chunk;
          offset += chunk;
          process_rtsp_buffer(slot, buffer, &buf_len);
          if (conn->close_after_response) goto cleanup;
        }
      }
      goto cleanup;
    }

    // Plain-text mode
    if (buf_len == buf_capacity) {
      if (buf_capacity == RTSP_BUFFER_LARGE) break;
      size_t new_cap = buf_capacity * 2;
      if (new_cap > RTSP_BUFFER_LARGE) new_cap = RTSP_BUFFER_LARGE;
      uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
      if (!new_buf) {
        break;
      }
      buffer = new_buf;
      buf_capacity = new_cap;
    }

    ssize_t recv_len =
        recv(slot->socket, buffer + buf_len, buf_capacity - buf_len, 0);
    if (recv_len <= 0) {
      if (recv_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        continue;
      }
      break;
    }
    buf_len += (size_t)recv_len;
    process_rtsp_buffer(slot, buffer, &buf_len);
    if (conn->close_after_response) {
      goto cleanup;
    }
  }

cleanup:
  AP_LOGI(TAG, "Client slot %d disconnected", slot_idx);
  free(decrypted);
  free(buffer);
  detach_client_socket(slot);

  /* Only the owner of the session tears down the global audio / event state.
   * A connection that never took the play lock (a probe) leaves the playing
   * session alone, exactly as Shairport's release_play_lock() does. */
  if (conn->owns_session) {
    // Immediate: stop audio and the event channel
    audio_receiver_stop();
    rtsp_stop_event_port_task();
    /* A timeout requests cancellation, but does not release ownership. Keep
     * this slot live until every producer and event socket actually exits. */
    while (!audio_receiver_is_idle() || !rtsp_event_port_is_idle()) {
      /* A late static DATA-task suspension must be reaped by another stop. */
      if (!audio_receiver_is_idle()) audio_receiver_stop();
      if (!rtsp_event_port_is_idle()) rtsp_stop_event_port_task();
      vTaskDelay(1);
    }
    audio_receiver_set_stream_type(AUDIO_STREAM_NONE);
    audio_receiver_set_encryption(NULL);
  }

  detach_client_conn(slot);
  rtsp_conn_free(conn);

  slot->should_stop = false;
  slot->is_old = false;
  slot->live = false;

  vTaskDelete(NULL);
}

// Signal a client to stop.  The server must wait for the task to finish its
// global audio/PTP cleanup before giving a replacement client stream ownership.
static void signal_client_stop(int slot_idx) {
  client_slot_t *slot = &clients[slot_idx];
  if (!slot->live) {
    return;
  }

  AP_LOGI(TAG, "Signaling client slot %d to stop", slot_idx);
  slot->is_old = true;
  slot->should_stop = true;

  // Serialize shutdown with descriptor detach; only the owner closes.
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  if (slot->socket >= 0) {
    bound_socket_stop(slot->socket);
    shutdown(slot->socket, SHUT_RDWR);
  }
  xSemaphoreGive(lifecycle_mutex);
}

static bool wait_client_stopped(int slot_idx, TickType_t timeout_ticks) {
  client_slot_t *slot = &clients[slot_idx];
  TickType_t start = xTaskGetTickCount();

  while (slot->live) {
    if ((TickType_t)(xTaskGetTickCount() - start) >= timeout_ticks) {
      return false;
    }
    vTaskDelay(1);
  }
  return true;
}


/* Find the client slot that runs `conn` (NULL if none). */
static int slot_of_conn(const rtsp_conn_t *conn) {
  int idx = -1;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  for (int i = 0; i < 2; i++) {
    if (clients[i].live && clients[i].conn == conn) { idx = i; break; }
  }
  xSemaphoreGive(lifecycle_mutex);
  return idx;
}

void rtsp_session_release(rtsp_conn_t *conn) {
  taskENTER_CRITICAL(&s_owner_lock);
  if (s_owner == conn) s_owner = NULL;
  taskEXIT_CRITICAL(&s_owner_lock);
  if (conn) conn->owns_session = false;
}

/* Shairport get_play_lock(). With break_in the previous owner is stopped and
 * its global audio/PTP/event teardown completes before this returns, so the
 * caller starts from a clean state. */
bool rtsp_session_acquire(rtsp_conn_t *conn, bool break_in) {
  if (!conn) return false;
  const TickType_t start = xTaskGetTickCount();
  bool signalled = false;
  for (;;) {
    rtsp_conn_t *old = NULL;
    bool got = false;
    taskENTER_CRITICAL(&s_owner_lock);
    if (s_owner == conn) {
      got = true;
    } else if (s_owner == NULL) {
      s_owner = conn;
      got = true;
    } else {
      old = s_owner;
    }
    taskEXIT_CRITICAL(&s_owner_lock);
    if (got) {
      if (!conn->owns_session) {
        AP_LOGI(TAG, "play lock acquired%s", signalled ? " by breaking in" : "");
        rtsp_conn_take_volume(conn);
      }
      conn->owns_session = true;
      return true;
    }
    if (!break_in) return false;

    const int my_idx = slot_of_conn(conn);
    if (my_idx >= 0 && clients[my_idx].should_stop) return false;
    if ((TickType_t)(xTaskGetTickCount() - start) >= pdMS_TO_TICKS(3000)) {
      ESP_LOGE(TAG, "play lock: the old session did not end in time");
      return false;
    }
    if (!signalled) {
      const int old_idx = slot_of_conn(old);
      if (old_idx >= 0) {
        AP_LOGI(TAG, "play lock: ending the session in slot %d for slot %d",
                 old_idx, my_idx);
        signal_client_stop(old_idx);
        signalled = true;
      }
    }
    vTaskDelay(1);
  }
}

static void server_task(void *pvParameters) {
  (void)pvParameters;

  struct sockaddr_in server_addr, client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  int listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    goto server_exit;
  }
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  server_socket = listen_socket;
  bool stopping = !server_running;
  xSemaphoreGive(lifecycle_mutex);
  if (stopping) goto server_close;

  int opt = 1;
  setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  /* lwIP accept waits on the listener's mailbox. Do not rely on shutdown
   * alone to wake that wait: the owner must observe cancellation even with
   * no incoming clients, then close its own descriptor. */
  const struct timeval accept_timeout = {.tv_sec = 0, .tv_usec = 250000};
  if (setsockopt(listen_socket, SOL_SOCKET, SO_RCVTIMEO,
                 &accept_timeout, sizeof(accept_timeout)) < 0) {
    ESP_LOGE(TAG, "Failed to set accept timeout: %d", errno);
    goto server_close;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(RTSP_PORT);

  if (bind(listen_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind: %d", errno);
    goto server_close;
  }

  if (listen(listen_socket, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen: %d", errno);
    goto server_close;
  }

  ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);
  while (server_running) {
    client_addr_len = sizeof(client_addr);
    int new_socket = accept(listen_socket, (struct sockaddr *)&client_addr,
                            &client_addr_len);
    if (new_socket < 0) {
      const int accept_error = errno;
      if (!server_running) break;
      if (accept_error == EINTR || accept_error == EAGAIN ||
          accept_error == EWOULDBLOCK || accept_error == ETIMEDOUT) continue;
      if (accept_error == ENFILE || accept_error == EMFILE ||
          accept_error == ENOMEM || accept_error == ENOBUFS ||
          accept_error == ECONNABORTED) {
        /* Out of sockets or memory for a moment (e.g. several web UI pages
         * open). Not a reason to stop AirPlay and the music that is
         * playing: wait for a socket to be freed and accept again. */
        static int64_t last_warn_us;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us > 10000000LL) {
          AP_LOGW(TAG, "accept: no socket available (errno %d); retrying",
                   accept_error);
          last_warn_us = now_us;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      ESP_LOGE(TAG, "Failed to accept: %d", accept_error);
      break;
    }

    if (!server_running) { close(new_socket); break; }
    AP_LOGI(TAG, "New client connected");

    /* Shairport 5.6-dev-157: a new connection must not end the playing
     * session. It gets a slot beside the owner; the owner is replaced only
     * when this connection later asks to play (SETUP with streams, see
     * rtsp_session_acquire). Other AirPlay 2 senders open short "is the
     * device still there" connections that never become audio sessions. */
    int new_slot = -1;
    for (int i = 0; i < 2; i++) {
      if (!clients[i].live) { new_slot = i; break; }
    }
    if (new_slot < 0) {
      /* Both slots busy: drop a connection that is not the session owner. */
      rtsp_conn_t *owner;
      taskENTER_CRITICAL(&s_owner_lock);
      owner = s_owner;
      taskEXIT_CRITICAL(&s_owner_lock);
      int victim = -1;
      xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
      for (int i = 0; i < 2; i++) {
        if (!clients[i].conn || clients[i].conn != owner) { victim = i; break; }
      }
      xSemaphoreGive(lifecycle_mutex);
      if (victim < 0) { /* cannot happen: only one owner */
        close(new_socket);
        continue;
      }
      AP_LOGI(TAG, "Both client slots busy: dropping idle connection in slot %d",
               victim);
      signal_client_stop(victim);
      if (!wait_client_stopped(victim, pdMS_TO_TICKS(3000))) {
        ESP_LOGE(TAG, "Slot %d task did not exit in time", victim);
        close(new_socket);
        continue;
      }
      new_slot = victim;
    }

    // Set up the new slot (the playing session, if any, is untouched).
    xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
    if (!server_running) {
      xSemaphoreGive(lifecycle_mutex);
      close(new_socket);
      break;
    }
    clients[new_slot].socket = new_socket;
    clients[new_slot].should_stop = false;
    clients[new_slot].is_old = false;
    clients[new_slot].live = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        client_task, "rtsp_client", CLIENT_STACK_SIZE,
        (void *)(intptr_t)new_slot, RTSP_CLIENT_TASK_PRIORITY, NULL, 0);
    if (task_ret != pdPASS) {
      clients[new_slot].socket = -1;
      clients[new_slot].live = false;
    } else {
      current_slot = new_slot;
    }
    xSemaphoreGive(lifecycle_mutex);
    if (task_ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create client task");
      close(new_socket);
    }
  }

  for (int i = 0; i < 2; i++) signal_client_stop(i);

server_close:
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  server_socket = -1;
  xSemaphoreGive(lifecycle_mutex);
  close(listen_socket);
server_exit:
  server_running = false;
  server_task_live = false;
  vTaskDelete(NULL);
}

bool rtsp_server_is_idle(void) {
  return !server_task_live && !clients[0].live && !clients[1].live &&
         rtsp_event_port_is_idle();
}

esp_err_t rtsp_server_start(void) {
  if (!lifecycle_mutex) {
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created) return ESP_ERR_NO_MEM;
    SemaphoreHandle_t expected = NULL;
    if (!atomic_compare_exchange_strong(&lifecycle_mutex, &expected, created))
      vSemaphoreDelete(created);
  }
  if (!lifecycle_mutex) return ESP_ERR_NO_MEM;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  if (!rtsp_server_is_idle()) {
    xSemaphoreGive(lifecycle_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  server_running = true;
  server_task_live = true;
  BaseType_t ret = xTaskCreatePinnedToCore(
      server_task, "rtsp_server", SERVER_STACK_SIZE, NULL,
      RTSP_SERVER_TASK_PRIORITY, NULL, 0);
  if (ret != pdPASS) {
    server_running = false;
    server_task_live = false;
  }
  xSemaphoreGive(lifecycle_mutex);
  return ret == pdPASS ? ESP_OK : ESP_FAIL;
}

void rtsp_server_stop(void) {
  if (!lifecycle_mutex) return;
  xSemaphoreTake(lifecycle_mutex, portMAX_DELAY);
  server_running = false;
  if (server_socket >= 0) shutdown(server_socket, SHUT_RDWR);
  xSemaphoreGive(lifecycle_mutex);
  for (int i = 0; i < 2; ++i) signal_client_stop(i);
  TickType_t start = xTaskGetTickCount();
  while (server_task_live &&
         (TickType_t)(xTaskGetTickCount() - start) < pdMS_TO_TICKS(2000))
    vTaskDelay(1);
  for (int i = 0; i < 2; ++i) {
    if (!wait_client_stopped(i, pdMS_TO_TICKS(3000)))
      AP_LOGW(TAG, "RTSP client slot %d still stopping", i);
  }
  if (!rtsp_server_is_idle())
    AP_LOGW(TAG,
             "RTSP owners still stopping: listener=%d client0=%d client1=%d event=%d; "
             "restart/resource release forbidden",
             (int)atomic_load(&server_task_live), (int)atomic_load(&clients[0].live),
             (int)atomic_load(&clients[1].live), !rtsp_event_port_is_idle());
}
