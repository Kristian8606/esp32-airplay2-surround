#include "gw_fw.h"

#include <stdio.h>
#include <string.h>

#include "airplay_log.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sodium.h"

static const char *TAG = "gw_fw";

#define GW_FW_PARTITION   "storage"
#define GW_FW_NVS_NS      "gw_fw"
#define GW_FW_NVS_KEY     "image"
#define GW_FW_NVS_HALT    "halt" /* name of the speaker that stopped it */
#define GW_FW_NVS_MAGIC   0x47574657U /* "GWFW" */
#define GW_FW_BUF         4096U
#define GW_FW_ERASE_BLOCK (64U * 1024U)
#define GW_FW_IDLE_US     (15LL * 1000000LL)
#define GW_FW_SEND_STALLS 50 /* send timeouts in a row before giving up */
/* Only a speaker image is stored: its project name starts with this. */
#define GW_FW_PROJECT     "airplay-speaker"

typedef struct {
  uint32_t magic;
  uint32_t size;
  uint8_t sha256[32];
  char id[24];
} gw_fw_record_t;

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static gw_fw_image_t s_img;

static void save_record(const gw_fw_image_t *img) {
  nvs_handle_t h;
  if (nvs_open(GW_FW_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  if (img) {
    gw_fw_record_t r = {.magic = GW_FW_NVS_MAGIC, .size = img->size};
    memcpy(r.sha256, img->sha256, sizeof(r.sha256));
    memcpy(r.id, img->id, sizeof(r.id));
    (void)nvs_set_blob(h, GW_FW_NVS_KEY, &r, sizeof(r));
  } else {
    (void)nvs_erase_key(h, GW_FW_NVS_KEY);
  }
  (void)nvs_commit(h);
  nvs_close(h);
}

static void save_halt(const char *name) {
  nvs_handle_t h;
  if (nvs_open(GW_FW_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  if (name) (void)nvs_set_str(h, GW_FW_NVS_HALT, name);
  else (void)nvs_erase_key(h, GW_FW_NVS_HALT);
  (void)nvs_commit(h);
  nvs_close(h);
}

void gw_fw_halt(const char *speaker_name) {
  if (!s_lock) return;
  char name[sizeof(s_img.halted_by)];
  strlcpy(name, speaker_name && speaker_name[0] ? speaker_name : "?", sizeof(name));
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_img.halted = true;
  memcpy(s_img.halted_by, name, sizeof(name));
  xSemaphoreGive(s_lock);
  save_halt(name);
  ESP_LOGE(TAG, "speaker firmware %s held back: %s did not come back with it; "
                "upload the firmware again to continue",
           s_img.id, name);
}

esp_err_t gw_fw_init(void) {
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, GW_FW_PARTITION);
  if (!s_part) {
    ESP_LOGW(TAG, "no \"%s\" partition: speakers cannot update from the Gateway",
             GW_FW_PARTITION);
    return ESP_ERR_NOT_FOUND;
  }
  nvs_handle_t h;
  gw_fw_record_t r = {0};
  size_t len = sizeof(r);
  if (nvs_open(GW_FW_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_blob(h, GW_FW_NVS_KEY, &r, &len) == ESP_OK && len == sizeof(r) &&
        r.magic == GW_FW_NVS_MAGIC && r.size > 0 && r.size <= s_part->size) {
      s_img.valid = true;
      s_img.size = r.size;
      memcpy(s_img.sha256, r.sha256, sizeof(s_img.sha256));
      memcpy(s_img.id, r.id, sizeof(s_img.id));
      s_img.id[sizeof(s_img.id) - 1] = '\0';
      s_img.generation = 1;
    }
    size_t nlen = sizeof(s_img.halted_by);
    if (nvs_get_str(h, GW_FW_NVS_HALT, s_img.halted_by, &nlen) == ESP_OK) {
      s_img.halted = true;
    }
    nvs_close(h);
  }
  if (s_img.valid && s_img.halted) {
    AP_LOGW(TAG, "speaker firmware %s held back since %s did not come back with it",
            s_img.id, s_img.halted_by);
  } else if (s_img.valid) {
    AP_LOGI(TAG, "speaker firmware %s (%u bytes) ready for the speakers", s_img.id,
            (unsigned)s_img.size);
  } else {
    AP_LOGI(TAG, "no speaker firmware stored (%u KiB available)",
            (unsigned)(s_part->size / 1024U));
  }
  return ESP_OK;
}

void gw_fw_get(gw_fw_image_t *out) {
  memset(out, 0, sizeof(*out));
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_img;
  xSemaphoreGive(s_lock);
}

size_t gw_fw_capacity(void) { return s_part ? s_part->size : 0U; }

/* Receive up to `want` bytes; false on an error or a stalled sender. */
static bool recv_some(httpd_req_t *req, uint8_t *buf, size_t want, size_t *got) {
  const int64_t deadline = esp_timer_get_time() + GW_FW_IDLE_US;
  for (;;) {
    const int n = httpd_req_recv(req, (char *)buf, want);
    if (n > 0) {
      *got = (size_t)n;
      return true;
    }
    if (n != HTTPD_SOCK_ERR_TIMEOUT || esp_timer_get_time() >= deadline) return false;
  }
}

/* The build id the speaker will report: version and the first 4 bytes of the
 * ELF SHA-256 (firmware_id.c). Checks that it is a speaker image. */
static bool image_id(const uint8_t *head, size_t len, char *id, size_t id_len,
                     const char **why) {
  const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  const esp_image_header_t *ih = (const esp_image_header_t *)head;
  if (len < desc_off + sizeof(esp_app_desc_t) || ih->magic != ESP_IMAGE_HEADER_MAGIC) {
    *why = "Not an ESP32 firmware image";
    return false;
  }
  esp_app_desc_t d;
  memcpy(&d, head + desc_off, sizeof(d));
  if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) {
    *why = "Not an ESP32 firmware image";
    return false;
  }
  d.project_name[sizeof(d.project_name) - 1] = '\0';
  d.version[sizeof(d.version) - 1] = '\0';
  if (strncmp(d.project_name, GW_FW_PROJECT, strlen(GW_FW_PROJECT)) != 0) {
    *why = strncmp(d.project_name, "airplay-gateway", 15) == 0
               ? "This is the Gateway firmware: update the Gateway on its main page"
               : "Not a speaker firmware (airplay-speaker_*.bin)";
    return false;
  }
  /* Cut at id_len like the speaker's own HELLO, so the two compare equal. */
  char full[sizeof(d.version) + 16];
  snprintf(full, sizeof(full), "%s %02x%02x%02x%02x", d.version, d.app_elf_sha256[0],
           d.app_elf_sha256[1], d.app_elf_sha256[2], d.app_elf_sha256[3]);
  strlcpy(id, full, id_len);
  return true;
}

esp_err_t gw_fw_receive(httpd_req_t *req, const char **why) {
  *why = "Upload failed";
  if (!s_part) {
    *why = "This Gateway has no storage partition";
    return ESP_ERR_NOT_FOUND;
  }
  const size_t total = req->content_len;
  if (total == 0 || total > s_part->size) {
    *why = total ? "Firmware larger than the Gateway's storage partition"
                 : "No firmware uploaded";
    return ESP_ERR_INVALID_SIZE;
  }
  uint8_t *buf = heap_caps_malloc(GW_FW_BUF, MALLOC_CAP_8BIT);
  if (!buf) return ESP_ERR_NO_MEM;

  /* The first block names the image before anything is erased. */
  size_t have = 0;
  while (have < GW_FW_BUF && have < total) {
    size_t got = 0;
    if (!recv_some(req, buf + have, (GW_FW_BUF < total ? GW_FW_BUF : total) - have,
                   &got)) {
      heap_caps_free(buf);
      return ESP_FAIL;
    }
    have += got;
  }
  char id[24];
  if (!image_id(buf, have, id, sizeof(id), why)) {
    heap_caps_free(buf);
    return ESP_ERR_INVALID_STATE;
  }

  /* From here the stored image is gone until this one is complete. */
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_img.valid = false;
  xSemaphoreGive(s_lock);
  save_record(NULL);
  AP_LOGI(TAG, "receiving speaker firmware %s (%u bytes)", id, (unsigned)total);

  crypto_hash_sha256_state sha;
  crypto_hash_sha256_init(&sha);
  size_t written = 0, erased = 0;
  esp_err_t err = ESP_OK;
  for (;;) {
    while (erased < written + have) {
      size_t n = GW_FW_ERASE_BLOCK;
      if (erased + n > s_part->size) n = s_part->size - erased;
      err = esp_partition_erase_range(s_part, erased, n);
      if (err != ESP_OK) break;
      erased += n;
    }
    if (err == ESP_OK) err = esp_partition_write(s_part, written, buf, have);
    if (err != ESP_OK) {
      *why = "Writing the Gateway's flash failed";
      break;
    }
    crypto_hash_sha256_update(&sha, buf, have);
    written += have;
    if (written >= total) break;
    const size_t want = total - written < GW_FW_BUF ? total - written : GW_FW_BUF;
    if (!recv_some(req, buf, want, &have)) {
      err = ESP_FAIL;
      *why = "Upload interrupted";
      break;
    }
  }
  uint8_t digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256_final(&sha, digest);
  heap_caps_free(buf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "speaker firmware upload failed: %s", *why);
    return err;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_img.valid = true;
  s_img.size = (uint32_t)total;
  memcpy(s_img.sha256, digest, sizeof(digest));
  memcpy(s_img.id, id, sizeof(s_img.id));
  s_img.generation++;
  s_img.halted = false; /* a new upload: the rollout starts over */
  s_img.halted_by[0] = '\0';
  const gw_fw_image_t img = s_img;
  xSemaphoreGive(s_lock);
  save_record(&img);
  save_halt(NULL);
  AP_LOGI(TAG, "speaker firmware %s stored: speakers with another build update "
               "one at a time, now",
          img.id);
  return ESP_OK;
}

/* httpd_send() may take part of a buffer. */
static esp_err_t send_raw(httpd_req_t *req, const void *data, size_t len) {
  const char *p = data;
  int stalls = 0;
  while (len > 0) {
    const int n = httpd_send(req, p, len);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < GW_FW_SEND_STALLS) continue;
    if (n <= 0) return ESP_FAIL;
    stalls = 0;
    p += n;
    len -= (size_t)n;
  }
  return ESP_OK;
}

/* A plain response with Content-Length, not chunked: the speaker reads it
 * with a few lines of socket code instead of a full HTTP client (about 100
 * KiB of firmware, which the storage partition cannot spare). */
esp_err_t gw_fw_serve(httpd_req_t *req) {
  gw_fw_image_t img;
  gw_fw_get(&img);
  if (!img.valid || !s_part) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No speaker firmware stored");
    return ESP_FAIL;
  }
  uint8_t *buf = heap_caps_malloc(GW_FW_BUF, MALLOC_CAP_8BIT);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_ERR_NO_MEM;
  }
  AP_LOGI(TAG, "sending speaker firmware %s", img.id);
  char hdr[160];
  const int hl = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                          "Content-Length: %u\r\nConnection: close\r\n\r\n",
                          (unsigned)img.size);
  esp_err_t err = send_raw(req, hdr, (size_t)hl);
  for (size_t off = 0; err == ESP_OK && off < img.size; off += GW_FW_BUF) {
    const size_t n = img.size - off < GW_FW_BUF ? img.size - off : GW_FW_BUF;
    err = esp_partition_read(s_part, off, buf, n);
    if (err == ESP_OK) err = send_raw(req, buf, n);
  }
  heap_caps_free(buf);
  if (err != ESP_OK) ESP_LOGW(TAG, "sending speaker firmware stopped: %s", esp_err_to_name(err));
  /* Connection: close, as announced. */
  (void)httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
  return err;
}
