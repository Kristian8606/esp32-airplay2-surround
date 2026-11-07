/*
 * SPDX-FileCopyrightText: 2026 Kristian8606
 * SPDX-License-Identifier: Apache-2.0
 */

/* Captive DNS task: receives on UDP 53 and answers with dns_message_reply().
 * The task never closes the socket nor deletes itself; dns_server_stop()
 * does both once the task has said it is done. Start and stop are called
 * only from the default event loop task, so they take no lock. */

#include "dns_server.h"

#include <errno.h>
#include <stdatomic.h>

#include "dns_message.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "socket_utils.h"

#define DNS_PORT 53
#define DNS_TASK_STACK 4096
#define DNS_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define DNS_RECV_TIMEOUT_MS 200
#define DNS_STOP_WAIT_MS 2000
#define DNS_ERROR_BACKOFF_MS 20

static const char *TAG = "dns_server";

static TaskHandle_t s_task;
static int s_sock = -1;
static uint32_t s_redirect_ip;
static atomic_bool s_stop;
static StaticSemaphore_t s_done_buf;
static SemaphoreHandle_t s_done;

static void dns_task(void *arg) {
  (void)arg;
  /* One byte more than the largest query: a datagram over 512 bytes reads
   * as 513 bytes and dns_message_reply() drops it. */
  uint8_t query[DNS_MESSAGE_MAX + 1];
  uint8_t reply[DNS_MESSAGE_MAX];

  while (!atomic_load(&s_stop)) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(s_sock, query, sizeof(query), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        vTaskDelay(pdMS_TO_TICKS(DNS_ERROR_BACKOFF_MS));
      }
      continue;
    }

    size_t len = dns_message_reply(query, (size_t)n, s_redirect_ip, reply,
                                   sizeof(reply));
    if (len > 0) {
      sendto(s_sock, reply, len, 0, (struct sockaddr *)&from, from_len);
    }
  }

  xSemaphoreGive(s_done);
  /* Wait here to be deleted by dns_server_stop(). */
  for (;;) {
    vTaskSuspend(NULL);
  }
}

esp_err_t dns_server_start(uint32_t redirect_ip) {
  if (s_task != NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  int sock = socket_utils_bind_udp(DNS_PORT, 0, 0, NULL);
  if (sock < 0) {
    return ESP_FAIL;
  }
  struct timeval tv = {.tv_sec = 0, .tv_usec = DNS_RECV_TIMEOUT_MS * 1000};
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
    close(sock);
    return ESP_FAIL;
  }

  if (s_done == NULL) {
    s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
  }

  s_sock = sock;
  s_redirect_ip = redirect_ip;
  atomic_store(&s_stop, false);

  TaskHandle_t task = NULL;
  if (xTaskCreateWithCaps(dns_task, "dns_server", DNS_TASK_STACK, NULL,
                          DNS_TASK_PRIORITY, &task,
                          MALLOC_CAP_SPIRAM) != pdPASS) {
    s_sock = -1;
    close(sock);
    return ESP_ERR_NO_MEM;
  }
  s_task = task;

  const uint8_t *ip = (const uint8_t *)&redirect_ip;
  ESP_LOGI(TAG, "captive DNS on UDP %d, answers %u.%u.%u.%u", DNS_PORT, ip[0],
           ip[1], ip[2], ip[3]);
  return ESP_OK;
}

void dns_server_stop(void) {
  if (s_task == NULL) {
    return;
  }

  atomic_store(&s_stop, true);
  /* The task sees the flag within one receive timeout (200 ms). If it has
   * not ended in 2 s it may be inside a socket call, so it is not deleted:
   * the server stays running and a later stop tries again. */
  if (xSemaphoreTake(s_done, pdMS_TO_TICKS(DNS_STOP_WAIT_MS)) != pdTRUE) {
    ESP_LOGE(TAG, "captive DNS task did not end within %d ms",
             DNS_STOP_WAIT_MS);
    return;
  }
  vTaskDeleteWithCaps(s_task);
  s_task = NULL;

  close(s_sock);
  s_sock = -1;

  ESP_LOGI(TAG, "captive DNS stopped");
}
