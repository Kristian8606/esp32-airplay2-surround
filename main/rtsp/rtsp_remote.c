#include "rtsp_remote.h"
#include "rtsp_handlers.h"
#include "rtsp_crypto.h"
#include "rtsp_events.h"
#include "ap2_compat.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sodium.h"
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include "airplay_log.h"

#define EVENT_STACK_SIZE 3072
#define EVENT_TIMEOUT_US INT64_C(3000000)
#define EVENT_HEADER_MAX 1024
#define EVENT_BODY_MAX (64 * 1024)
static const char *TAG = "airplay_remote";

/* Task-owned storage in PSRAM, with separate event keys/counters. The RTSP
 * client joins this task before freeing volume_owner (see server cleanup).
 * The borrowed pointer is used only through synchronized volume APIs. */
typedef struct {
  int listener;
  rtsp_conn_t *volume_owner;
  float pending_volume_base, pending_volume_target;
  bool pending_volume;
  uint32_t peer_ip;
  bool encrypted;
  uint8_t tx_key[32], rx_key[32];
  uint64_t tx_nonce, rx_nonce;
  uint8_t wire[RTSP_ENCRYPTED_BLOCK_MAX + 18];
  uint8_t plain[RTSP_ENCRYPTED_BLOCK_MAX];
  uint8_t tx[512];
  char header[EVENT_HEADER_MAX + 1];
  size_t wire_used, header_used, body_left;
  bool reading_body;
  int response_code, response_cseq, request_cseq;
  int request_status;
  uint32_t pending_id;
  int64_t pending_deadline, read_deadline;
} event_context_t;

static int event_client_socket = -1;
static int event_listen_socket = -1;
static _Atomic bool event_task_live;
static _Atomic bool event_task_should_stop;
static _Atomic(SemaphoreHandle_t) event_socket_mutex;
static rtsp_remote_status_t remote_status;
static uint32_t next_id;

static const char *remote_command_name(uint8_t command) {
  static const char *const names[] = {
    "play", "pause", "toggle", "stop", "next", "previous", "volume_down", "volume_up"
  };
  return command < sizeof(names) / sizeof(names[0]) ? names[command] : "unknown";
}

static bool remote_busy(void) {
  return remote_status.result == RTSP_REMOTE_QUEUED ||
         remote_status.result == RTSP_REMOTE_SENT;
}

const char *rtsp_remote_result_name(rtsp_remote_result_t r) {
  static const char *const names[] = {
    "idle", "queued", "sent", "acknowledged", "rejected",
    "timeout", "failed", "disconnected"
  };
  return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "failed";
}

void rtsp_remote_get_status(rtsp_remote_status_t *status) {
  if (!status) return;
  memset(status, 0, sizeof(*status));
  if (!event_socket_mutex) return;
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  *status = remote_status;
  xSemaphoreGive(event_socket_mutex);
}

esp_err_t rtsp_remote_enqueue(uint8_t command, uint32_t *id) {
  if (command > RTSP_REMOTE_VOLUME_UP || !id) return ESP_ERR_INVALID_ARG;
  if (!event_socket_mutex) return ESP_ERR_NOT_FOUND;
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  esp_err_t err = ESP_OK;
  if (!remote_status.connected || event_task_should_stop)
    err = ESP_ERR_NOT_FOUND;
  else if (remote_busy())
    err = ESP_ERR_INVALID_STATE;
  else {
    if (++next_id > INT_MAX) next_id = 1;
    remote_status.id = next_id;
    remote_status.command = command;
    remote_status.result = RTSP_REMOTE_QUEUED;
    remote_status.response_code = 0;
    *id = next_id;
  }
  xSemaphoreGive(event_socket_mutex);
  return err;
}

static void remote_finish(uint32_t id, rtsp_remote_result_t result, int code) {
  bool updated = false;
  uint8_t command = 0;
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  if (remote_status.id == id && remote_busy()) {
    remote_status.result = result;
    remote_status.response_code = code;
    command = remote_status.command;
    updated = true;
  }
  xSemaphoreGive(event_socket_mutex);
  if (updated)
    AP_LOGI(TAG, "Command id=%" PRIu32 " cmd=%s %s RTSP=%d", id,
             remote_command_name(command), rtsp_remote_result_name(result), code);
}

/* Only this task closes transferred descriptors. Stop holds the same mutex
 * through shutdown; detach-before-close prevents descriptor reuse races. */
static void event_close_client(void) {
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  int client = event_client_socket;
  event_client_socket = -1;
  remote_status.connected = false;
  bool interrupted = remote_busy();
  uint32_t id = remote_status.id;
  uint8_t command = remote_status.command;
  if (interrupted) remote_status.result = RTSP_REMOTE_DISCONNECTED;
  xSemaphoreGive(event_socket_mutex);
  if (interrupted)
    AP_LOGW(TAG, "Command id=%" PRIu32 " cmd=%s disconnected before confirmation", id, remote_command_name(command));
  if (client >= 0) {
    close(client);
    AP_LOGI(TAG, "Event connection closed");
  }
}

static int event_send(event_context_t *ctx, int fd, const uint8_t *data, size_t n) {
  if (n > sizeof(ctx->tx) - 18) return -1;
  if (!ctx->encrypted)
    return rtsp_socket_send_all(fd, data, n, esp_timer_get_time() + EVENT_TIMEOUT_US);
  ctx->tx[0] = (uint8_t)n;
  ctx->tx[1] = (uint8_t)(n >> 8);
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &ctx->tx_nonce, 8);
  unsigned long long written = 0;
  if (crypto_aead_chacha20poly1305_ietf_encrypt(
        ctx->tx + 2, &written, data, n, ctx->tx, 2, NULL, nonce, ctx->tx_key) != 0)
    return -1;
  ctx->tx_nonce++;
  return rtsp_socket_send_all(fd, ctx->tx, (size_t)written + 2,
                             esp_timer_get_time() + EVENT_TIMEOUT_US);
}

/* Parse only complete, NUL-terminated headers. Missing CSeq is allowed for
 * Apple's event replies, with exactly one command outstanding. */
static int event_cseq(const char *header) {
  const char *line = strstr(header, "\r\n");
  int cseq = 0;
  while (line && line[2]) {
    line += 2;
    const char *end = strstr(line, "\r\n");
    if (!end) return -1;
    if (strncasecmp(line, "CSeq:", 5) == 0) {
      if (cseq) return -1;
      errno = 0;
      char *tail;
      long value = strtol(line + 5, &tail, 10);
      if (tail == line + 5 || errno || value <= 0 || value > INT_MAX || tail > end)
        return -1;
      while (tail < end && (*tail == ' ' || *tail == '\t')) tail++;
      if (tail != end) return -1;
      cseq = (int)value;
    }
    line = end;
  }
  return cseq;
}

static int event_message_done(event_context_t *ctx, int fd) {
  if (ctx->response_code) {
    if (ctx->pending_id && (!ctx->response_cseq ||
        (uint32_t)ctx->response_cseq == ctx->pending_id)) {
      if (ctx->response_code >= 200) {
        if (ctx->response_code < 300 && ctx->pending_volume && !event_task_should_stop) {
          /* A sender volume update received meanwhile remains authoritative. */
          bool applied = rtsp_conn_set_volume_if_unchanged(
              ctx->volume_owner, ctx->pending_volume_base, ctx->pending_volume_target);
          AP_LOGI(TAG, "Volume target %.3f dB: %s", ctx->pending_volume_target,
                   applied ? "applied after acknowledgment" : "newer sender volume preserved");
        }
        ctx->pending_volume = false;
        remote_finish(ctx->pending_id, ctx->response_code < 300 ?
                      RTSP_REMOTE_ACKNOWLEDGED : RTSP_REMOTE_REJECTED,
                      ctx->response_code);
        ctx->pending_id = 0;
      }
    } else {
      AP_LOGW(TAG, "Unmatched event reply RTSP=%d CSeq=%d",
               ctx->response_code, ctx->response_cseq);
    }
  } else {
    char response[128];
    int n = snprintf(response, sizeof(response),
        "RTSP/1.0 %d %s\r\nCSeq: %d\r\nContent-Length: 0\r\n\r\n",
        ctx->request_status, ctx->request_status == 200 ? "OK" : "Not Implemented",
        ctx->request_cseq);
    if (n <= 0 || (size_t)n >= sizeof(response) ||
        event_send(ctx, fd, (const uint8_t *)response, (size_t)n) != 0) return -1;
  }
  ctx->header_used = 0;
  ctx->reading_body = false;
  ctx->read_deadline = 0;
  return 0;
}

/* Bodies are bounded and drained without allocating metadata storage. A
 * reply is complete only after its body; coalesced messages remain framed. */
static int event_consume(event_context_t *ctx, int fd, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!ctx->read_deadline) ctx->read_deadline = esp_timer_get_time() + EVENT_TIMEOUT_US;
    if (ctx->reading_body) {
      if (--ctx->body_left == 0 && event_message_done(ctx, fd) != 0) return -1;
      continue;
    }
    if (ctx->header_used == EVENT_HEADER_MAX || p[i] == 0) return -1;
    ctx->header[ctx->header_used++] = (char)p[i];
    if (ctx->header_used < 4 || memcmp(ctx->header + ctx->header_used - 4,
                                     "\r\n\r\n", 4) != 0) continue;
    ctx->header[ctx->header_used] = 0;
    int length = rtsp_parse_content_length(ctx->header);
    int cseq = event_cseq(ctx->header);
    if (length < 0 || length > EVENT_BODY_MAX || cseq < 0) return -1;
    ctx->response_code = 0;
    if (strncmp(ctx->header, "RTSP/1.0 ", 9) == 0 ||
        strncmp(ctx->header, "RTSP/1.1 ", 9) == 0) {
      if (ctx->header_used < 13 || ctx->header[9] < '1' || ctx->header[9] > '5' ||
          ctx->header[10] < '0' || ctx->header[10] > '9' ||
          ctx->header[11] < '0' || ctx->header[11] > '9' ||
          (ctx->header[12] != ' ' && ctx->header[12] != '\r')) return -1;
      ctx->response_code = (ctx->header[9] - '0') * 100 +
                          (ctx->header[10] - '0') * 10 + ctx->header[11] - '0';
      ctx->response_cseq = cseq;
    } else {
      if (!cseq) return -1;
      ctx->request_cseq = cseq;
      ctx->request_status = strncmp(ctx->header, "OPTIONS ", 8) == 0 ? 200 : 501;
      AP_LOGW(TAG, "Inbound event request: %.80s", ctx->header);
    }
    ctx->body_left = (size_t)length;
    ctx->reading_body = true;
    if (!length && event_message_done(ctx, fd) != 0) return -1;
  }
  return 0;
}

static int event_read(event_context_t *ctx, int fd) {
  size_t target = 2;
  if (ctx->wire_used >= 2) {
    size_t length = (size_t)ctx->wire[0] | ((size_t)ctx->wire[1] << 8);
    if (!length || length > RTSP_ENCRYPTED_BLOCK_MAX) return -1;
    target = length + 18;
  }
  ssize_t n = recv(fd, ctx->wire + ctx->wire_used, target - ctx->wire_used, MSG_DONTWAIT);
  if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  if (n <= 0) return -1;
  if (!ctx->read_deadline) ctx->read_deadline = esp_timer_get_time() + EVENT_TIMEOUT_US;
  ctx->wire_used += (size_t)n;
  if (target == 2 || ctx->wire_used < target) return 0;
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, &ctx->rx_nonce, 8);
  unsigned long long length = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
        ctx->plain, &length, NULL, ctx->wire + 2, target - 2,
        ctx->wire, 2, nonce, ctx->rx_key) != 0) {
    ESP_LOGE(TAG, "Event authentication failed");
    return -1;
  }
  ctx->rx_nonce++;
  ctx->wire_used = 0;
  return event_consume(ctx, fd, ctx->plain, (size_t)length);
}

static int event_send_pending(event_context_t *ctx, int fd) {
  rtsp_remote_status_t status;
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  status = remote_status;
  if (status.result == RTSP_REMOTE_QUEUED)
    remote_status.result = RTSP_REMOTE_SENT;
  xSemaphoreGive(event_socket_mutex);
  if (status.result != RTSP_REMOTE_QUEUED) return 0;
  /* No receive/parser call is active here. Reuse the PSRAM plaintext
   * buffer for this small request, keeping crypto off large stack arrays. */
  uint8_t *message = ctx->plain;
  const size_t header_capacity = 160;
  size_t len;
  ctx->pending_volume = status.command == RTSP_REMOTE_VOLUME_DOWN ||
                        status.command == RTSP_REMOTE_VOLUME_UP;
  if (ctx->pending_volume) {
    ctx->pending_volume_base = rtsp_conn_get_volume_db(ctx->volume_owner);
    float target = ctx->pending_volume_base;
    if (target < -30.0f) target = -30.0f;
    target += status.command == RTSP_REMOTE_VOLUME_UP ? 1.875f : -1.875f;
    if (target > 0.0f) target = 0.0f;
    if (target < -30.0f) target = -144.0f;
    ctx->pending_volume_target = target;
    double unit_volume = target <= -30.0f ? 0.0 : (double)target / 30.0 + 1.0;
    len = bplist_build_media_remote_volume(message + header_capacity,
        sizeof(ctx->plain) - header_capacity, unit_volume);
  } else {
    len = bplist_build_media_remote_command(message + header_capacity,
        sizeof(ctx->plain) - header_capacity, status.command);
  }
  int n = snprintf((char *)message, header_capacity,
      "POST /command RTSP/1.0\r\nCSeq: %" PRIu32 "\r\n"
      "Content-Type: application/x-apple-binary-plist\r\nContent-Length: %u\r\n\r\n",
      status.id, (unsigned)len);
  if (!len || n <= 0 || (size_t)n >= header_capacity ||
      (size_t)n + len > sizeof(ctx->tx) - 18) goto failed;
  memmove(message + n, message + header_capacity, len);
  if (event_send(ctx, fd, message, (size_t)n + len) != 0) goto failed;
  ctx->pending_id = status.id;
  ctx->pending_deadline = esp_timer_get_time() + EVENT_TIMEOUT_US;
  AP_LOGI(TAG, "Sent id=%" PRIu32 " cmd=%s on encrypted audio event channel",
           status.id, remote_command_name(status.command));
  return 0;
failed:
  remote_finish(status.id, RTSP_REMOTE_FAILED, 0);
  return -1;
}

static void event_port_task(void *arg) {
  event_context_t *ctx = arg;
  while (!event_task_should_stop) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ctx->listener, &fds);
    struct timeval tv = {.tv_sec = 1};
    int ready = select(ctx->listener + 1, &fds, NULL, NULL, &tv);
    if (event_task_should_stop) break;
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) break;
    if (!ready) continue;
    struct sockaddr_in peer;
    socklen_t size = sizeof(peer);
    int client = accept(ctx->listener, (struct sockaddr *)&peer, &size);
    if (client < 0) { if (errno == EINTR) continue; break; }
    if (peer.sin_addr.s_addr != ctx->peer_ip) { close(client); continue; }
    ctx->wire_used = ctx->header_used = ctx->body_left = 0;
    ctx->reading_body = false;
    ctx->pending_id = 0;
    ctx->pending_volume = false;
    ctx->read_deadline = 0;
    /* Counters belong to the session and are not reset on TCP reconnect. */
    xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
    event_client_socket = client;
    remote_status.connected = ctx->encrypted && !event_task_should_stop;
    xSemaphoreGive(event_socket_mutex);
    AP_LOGI(TAG, "Audio event connected, encrypted=%d", ctx->encrypted);
    rtsp_events_emit(RTSP_EVENT_CLIENT_CONNECTED);
    while (!event_task_should_stop) {
      if (event_send_pending(ctx, client) != 0) break;
      int64_t now = esp_timer_get_time();
      if (ctx->pending_id && now >= ctx->pending_deadline) {
        remote_finish(ctx->pending_id, RTSP_REMOTE_TIMEOUT, 0);
        break; /* No retry: a late response must not acknowledge the next command. */
      }
      if (ctx->read_deadline && now >= ctx->read_deadline) {
        AP_LOGW(TAG, "Incomplete event message timeout");
        break;
      }
      FD_ZERO(&fds);
      FD_SET(client, &fds);
      tv = (struct timeval){.tv_usec = 100000};
      ready = select(client + 1, &fds, NULL, NULL, &tv);
      if (event_task_should_stop) break;
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) break;
      if (!ready) continue;
      if (!ctx->encrypted) {
        /* Keep legacy unencrypted events isolated from remote commands. */
        uint8_t byte;
        if (recv(client, &byte, 1, MSG_DONTWAIT) <= 0) break;
        AP_LOGW(TAG, "Unencrypted inbound event unsupported");
        break;
      }
      if (event_read(ctx, client) != 0) break;
    }
    event_close_client();
  }
  event_close_client();
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  event_listen_socket = -1;
  xSemaphoreGive(event_socket_mutex);
  close(ctx->listener);
  sodium_memzero(ctx, sizeof(*ctx));
  free(ctx);
  event_task_live = false;
  vTaskDelete(NULL);
}

bool rtsp_event_port_is_idle(void) { return !event_task_live; }

esp_err_t rtsp_start_event_port_task(int listener, rtsp_conn_t *conn) {
  if (listener < 0 || !conn || event_task_live) return ESP_ERR_INVALID_STATE;
  if (!event_socket_mutex) {
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created) return ESP_ERR_NO_MEM;
    SemaphoreHandle_t expected = NULL;
    if (!atomic_compare_exchange_strong(&event_socket_mutex, &expected, created))
      vSemaphoreDelete(created);
  }
  event_context_t *ctx = heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ctx) {
    ESP_LOGE(TAG, "Event context allocation failed: %u bytes PSRAM", (unsigned)sizeof(*ctx));
    return ESP_ERR_NO_MEM;
  }
  ctx->listener = listener;
  ctx->volume_owner = conn;
  ctx->peer_ip = conn->client_ip;
  const hap_session_t *hap = conn->hap_session;
  ctx->encrypted = conn->encrypted_mode && hap && ap2_hap_established(hap);
  if (ctx->encrypted) {
    size_t len = 0;
    const uint8_t *key = ap2_hap_pairing_secret(hap, &len);
    if (!key || !len) { free(ctx); return ESP_ERR_INVALID_STATE; }
    const char *salt = "Events-Salt";
    const char *tx_info = "Events-Write-Encryption-Key";
    const char *rx_info = "Events-Read-Encryption-Key";
    hap_hkdf_sha512((const uint8_t *)salt, strlen(salt), key, len,
                    (const uint8_t *)tx_info, strlen(tx_info), ctx->tx_key, 32);
    hap_hkdf_sha512((const uint8_t *)salt, strlen(salt), key, len,
                    (const uint8_t *)rx_info, strlen(rx_info), ctx->rx_key, 32);
  }
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  if (event_task_live) {
    xSemaphoreGive(event_socket_mutex);
    sodium_memzero(ctx, sizeof(*ctx)); free(ctx);
    return ESP_ERR_INVALID_STATE;
  }
  remote_status.connected = false;
  event_task_should_stop = false;
  event_listen_socket = listener;
  event_task_live = true;
  BaseType_t ret = xTaskCreatePinnedToCore(event_port_task, "event_port", EVENT_STACK_SIZE,
                                          ctx, 5, NULL, 0);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Event task start failed: stack=%u internalFree=%u internalLargest=%u",
             EVENT_STACK_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    event_listen_socket = -1;
    event_task_live = false;
  }
  xSemaphoreGive(event_socket_mutex);
  if (ret != pdPASS) { sodium_memzero(ctx, sizeof(*ctx)); free(ctx); }
  return ret == pdPASS ? ESP_OK : ESP_FAIL;
}

void rtsp_stop_event_port_task(void) {
  if (!event_socket_mutex) return;
  xSemaphoreTake(event_socket_mutex, portMAX_DELAY);
  event_task_should_stop = true;
  remote_status.connected = false;
  if (event_client_socket >= 0) shutdown(event_client_socket, SHUT_RDWR);
  if (event_listen_socket >= 0) shutdown(event_listen_socket, SHUT_RDWR);
  xSemaphoreGive(event_socket_mutex);
  TickType_t start = xTaskGetTickCount();
  while (event_task_live && (TickType_t)(xTaskGetTickCount() - start) < pdMS_TO_TICKS(1000))
    vTaskDelay(1);
  if (event_task_live) AP_LOGW(TAG, "Event task still stopping");
}
