#include "spk_update.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_log.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sodium.h"

static const char *TAG = "spk_update";

#define SPK_UPDATE_STACK      6144U
#define SPK_UPDATE_PRIORITY   3
#define SPK_UPDATE_BUF        4096U
#define SPK_UPDATE_TIMEOUT_S  10

typedef struct {
  uint32_t gateway_ip;
  surround_fw_update_t m;
} update_job_t;

static volatile bool s_running;

/* GET the image from the Gateway (gw_fw_serve(): a plain response with
 * Content-Length). Returns the socket with the headers read; the body bytes
 * that came with them are at *body, *body_len. -1 on failure. */
static int http_get(const update_job_t *job, uint8_t *buf, uint8_t **body,
                    size_t *body_len) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  const struct timeval tv = {.tv_sec = SPK_UPDATE_TIMEOUT_S};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_port = htons(job->m.http_port),
                          .sin_addr.s_addr = job->gateway_ip};
  char ip[16];
  inet_ntoa_r(a.sin_addr, ip, sizeof(ip));
  char req[128];
  const int rl = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                          SURROUND_FW_PATH, ip);
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      send(fd, req, (size_t)rl, 0) != rl) {
    ESP_LOGE(TAG, "cannot reach the Gateway at %s:%u", ip, (unsigned)job->m.http_port);
    close(fd);
    return -1;
  }
  size_t have = 0;
  char *end = NULL;
  while (!end) {
    const int n = recv(fd, buf + have, SPK_UPDATE_BUF - 1U - have, 0);
    if (n <= 0) break;
    have += (size_t)n;
    buf[have] = '\0';
    end = strstr((char *)buf, "\r\n\r\n");
    if (!end && have >= SPK_UPDATE_BUF - 1U) break;
  }
  if (!end || strncmp((char *)buf + 8, " 200", 4) != 0) {
    char *eol = strstr((char *)buf, "\r\n");
    if (eol) *eol = '\0';
    ESP_LOGE(TAG, "Gateway answered \"%s\"", have ? (char *)buf : "nothing");
    close(fd);
    return -1;
  }
  *body = (uint8_t *)end + 4;
  *body_len = have - (size_t)(*body - buf);
  return fd;
}

static esp_err_t install(const update_job_t *job) {
  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  if (!part || job->m.size > part->size) return ESP_ERR_INVALID_SIZE;
  uint8_t *buf = heap_caps_malloc(SPK_UPDATE_BUF, MALLOC_CAP_8BIT);
  if (!buf) return ESP_ERR_NO_MEM;
  uint8_t *data = NULL;
  size_t n = 0;
  const int fd = http_get(job, buf, &data, &n);
  if (fd < 0) {
    heap_caps_free(buf);
    return ESP_ERR_NOT_FOUND;
  }
  esp_ota_handle_t ota = 0;
  esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
  const bool ota_open = err == ESP_OK;
  crypto_hash_sha256_state sha;
  crypto_hash_sha256_init(&sha);
  uint32_t got = 0, next_log = 0;
  while (err == ESP_OK) {
    if (got + n > job->m.size) {
      err = ESP_ERR_INVALID_SIZE;
      break;
    }
    if (n > 0) {
      crypto_hash_sha256_update(&sha, data, (unsigned long long)n);
      err = esp_ota_write(ota, data, n);
      got += (uint32_t)n;
    }
    if (got >= next_log) {
      AP_LOGI(TAG, "firmware %u%%", (unsigned)(100ULL * got / job->m.size));
      next_log += job->m.size / 4U;
    }
    if (err != ESP_OK || got == job->m.size) break;
    const int r = recv(fd, buf, SPK_UPDATE_BUF, 0);
    if (r <= 0) {
      ESP_LOGE(TAG, "download stopped at %u of %u bytes", (unsigned)got,
               (unsigned)job->m.size);
      err = ESP_FAIL;
      break;
    }
    data = buf;
    n = (size_t)r;
  }
  close(fd);
  heap_caps_free(buf);
  uint8_t digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&sha, digest);
  if (err == ESP_OK && memcmp(digest, job->m.sha256, sizeof(digest)) != 0) {
    ESP_LOGE(TAG, "SHA-256 of the download differs from the Gateway's");
    err = ESP_ERR_INVALID_CRC;
  }
  if (err == ESP_OK) {
    err = esp_ota_end(ota); /* also checks the image itself */
  } else if (ota_open) {
    esp_ota_abort(ota);
  }
  if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
  return err;
}

static void update_task(void *arg) {
  update_job_t *job = arg;
  AP_LOGI(TAG, "installing firmware from the Gateway (%u bytes)",
          (unsigned)job->m.size);
  /* spk_link stopped any stream before and ignores stream commands until
   * this ends: the update never waits for the playback. */
  const esp_err_t err = install(job);
  free(job);
  if (err == ESP_OK) {
    AP_LOGI(TAG, "firmware installed, restarting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
  }
  ESP_LOGE(TAG, "firmware update failed (%s): this firmware keeps running "
                "and plays again from the next stream",
           esp_err_to_name(err));
  s_running = false;
  vTaskDelete(NULL);
}

void spk_update_start(uint32_t gateway_ip, const surround_fw_update_t *m) {
  if (s_running) {
    AP_LOGI(TAG, "firmware update already running");
    return;
  }
  update_job_t *job = malloc(sizeof(*job));
  if (!job) return;
  job->gateway_ip = gateway_ip;
  job->m = *m;
  s_running = true;
  if (xTaskCreatePinnedToCore(update_task, "spk_update", SPK_UPDATE_STACK, job,
                              SPK_UPDATE_PRIORITY, NULL, 0) != pdPASS) {
    free(job);
    s_running = false;
    ESP_LOGE(TAG, "cannot start the firmware update task");
  }
}

bool spk_update_running(void) { return s_running; }

void spk_update_confirm(void) {
  static bool s_confirmed;
  if (s_confirmed) return;
  s_confirmed = true;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    AP_LOGI(TAG, "new firmware reached the Gateway: kept (%s)", esp_err_to_name(err));
  }
}
