#include "gw_cover_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gw_cover_thumbnail.h"
#include "gw_usb.h"
#include "sodium.h"

#define CACHE_DIR GW_USB_MOUNT "/.airplay-covers-v1"
#define CACHE_TEMP CACHE_DIR "/pending.tmp"
#define CACHE_SLOTS 128U
#define THUMB_MAX (256U * 1024U)
/* Change this when the generated thumbnail geometry or encoding changes. */
#define THUMB_REVISION 3
#define SOURCE_MIN (32U * 1024U)
#define SOURCE_MAX (2U * 1024U * 1024U)
#define IO_CHUNK (32U * 1024U)
#define DMA_ALIGN 128U

static const char *TAG = "gw_cover_cache";

static void slot_path(const gw_cover_thumb_t *thumb, char buf[sizeof(CACHE_DIR) + 9]) {
  snprintf(buf, sizeof(CACHE_DIR) + 9, "%s/%02x.bin", CACHE_DIR,
           (unsigned)(thumb->key[0] % CACHE_SLOTS));
}

static void hash_source(const char *path, int64_t offset, uint32_t length,
                        const struct stat *st, gw_cover_thumb_t *thumb) {
  crypto_generichash_state h;
  const int64_t fields[] = {offset, length, st->st_size, st->st_mtime, THUMB_REVISION};
  crypto_generichash_init(&h, NULL, 0, sizeof(thumb->key));
  crypto_generichash_update(&h, (const uint8_t *)path, strlen(path));
  crypto_generichash_update(&h, (const uint8_t *)fields, sizeof(fields));
  crypto_generichash_final(&h, thumb->key, sizeof(thumb->key));
}

/* Unlike a plain slot name, the header also verifies the full cache key. */
int gw_cover_thumb_open(const gw_cover_thumb_t *thumb) {
  char slot[sizeof(CACHE_DIR) + 9];
  slot_path(thumb, slot);
  const int fd = open(slot, O_RDONLY);
  if (fd < 0) return -1;
  gw_cover_thumb_t entry;
  uint8_t soi[2];
  struct stat st;
  if (read(fd, &entry, sizeof(entry)) == sizeof(entry) &&
      memcmp(entry.key, thumb->key, sizeof(entry.key)) == 0 &&
      entry.length >= 4 && entry.length <= THUMB_MAX &&
      (!thumb->length || entry.length == thumb->length) &&
      fstat(fd, &st) == 0 && st.st_size == (off_t)(sizeof(entry) + entry.length) &&
      read(fd, soi, sizeof(soi)) == sizeof(soi) && soi[0] == 0xff && soi[1] == 0xd8 &&
      lseek(fd, sizeof(entry), SEEK_SET) == sizeof(entry)) return fd;
  close(fd);
  return -1;
}

static bool write_all(int fd, const void *buffer, size_t length) {
  const uint8_t *bytes = buffer;
  for (size_t at = 0; at < length;) {
    const ssize_t n = write(fd, bytes + at, length - at);
    if (n <= 0) return false;
    at += (size_t)n;
  }
  return true;
}

static bool cache_save(const gw_cover_thumb_t *thumb, const uint8_t *jpeg,
                      bool (*cancelled)(void *), void *arg) {
  const int fd = open(CACHE_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;
  bool ok = write_all(fd, thumb, sizeof(*thumb)) && write_all(fd, jpeg, thumb->length) &&
            fsync(fd) == 0;
  if (close(fd) != 0) ok = false;

  if (ok && !cancelled(arg)) {
    char slot[sizeof(CACHE_DIR) + 9];
    slot_path(thumb, slot);
    /* FatFs does not overwrite the destination on rename(). Both names are
     * private cache files and are protected by the USB media lock. */
    if (unlink(slot) == 0 || errno == ENOENT) {
      if (rename(CACHE_TEMP, slot) == 0) return true;
    }
  }
  unlink(CACHE_TEMP);
  return false;
}

static bool read_source(const char *path, int64_t offset, uint32_t length,
                        uint8_t *jpeg, bool (*cancelled)(void *), void *arg) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  uint32_t at = 0;
  if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset) {
    while (at < length && !cancelled(arg)) {
      const uint32_t left = length - at;
      const ssize_t n = read(fd, jpeg + at, left < IO_CHUNK ? left : IO_CHUNK);
      if (n <= 0) break;
      at += (uint32_t)n;
    }
  }
  close(fd);
  return at == length;
}

void gw_cover_thumb_prepare(const char *path, int64_t offset, uint32_t length,
                            bool (*cancelled)(void *), void *arg,
                            gw_cover_thumb_t *thumb) {
  thumb->length = 0;
  if (length < SOURCE_MIN || length > SOURCE_MAX || offset < 0 || cancelled(arg)) return;
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      offset > st.st_size || length > st.st_size - offset) return;
  hash_source(path, offset, length, &st, thumb);

  const int64_t start = esp_timer_get_time();
  const int cached = gw_cover_thumb_open(thumb);
  if (cached >= 0) {
    if (fstat(cached, &st) == 0) thumb->length = (uint32_t)(st.st_size - sizeof(*thumb));
    close(cached);
    if (thumb->length)
      ESP_LOGI(TAG, "USB cover cache hit: %u B in %lld ms", (unsigned)thumb->length,
               (long long)((esp_timer_get_time() - start) / 1000));
    return;
  }
  if (mkdir(CACHE_DIR, 0700) != 0 && errno != EEXIST) return;

  const size_t aligned = (length + DMA_ALIGN - 1U) & ~(DMA_ALIGN - 1U);
  uint8_t *image = heap_caps_aligned_calloc(DMA_ALIGN, 1, aligned, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!image) return;
  uint8_t *small = NULL;
  uint32_t small_size = 0;
  if (read_source(path, offset, length, image, cancelled, arg) && image[0] == 0xff &&
      image[1] == 0xd8 && !cancelled(arg))
    small = gw_cover_thumbnail(image, length, THUMB_MAX, &small_size);
  free(image);
  if (small && small_size >= 4 && small_size < length && !cancelled(arg)) {
    thumb->length = small_size;
    if (cache_save(thumb, small, cancelled, arg))
      ESP_LOGI(TAG, "Saved USB cover thumbnail: %u -> %u B in %lld ms",
               (unsigned)length, (unsigned)small_size, (long long)((esp_timer_get_time() - start) / 1000));
    else
      thumb->length = 0;
  }
  free(small);
}
