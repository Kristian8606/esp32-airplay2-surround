# ota

Firmware update of both firmwares from the web page (`POST /api/ota/update`,
the `.bin` as the raw request body). An ESP-IDF component under the
**Apache License 2.0** ([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)).

- `include/ota.h`: `ota_start_from_http()`, called from the URI handler
- `src/ota_image.c`: checks of the image start and the progress steps,
  without ESP-IDF
- `src/ota.c`: reading the body, writing and validating the image

## The caller

Before: checks the user may update, refuses an empty body, stops the audio.
After `ESP_OK`: answers and restarts the board. After an error: answers
`500` with `esp_err_to_name()` and starts the audio again. The module never
sends a response and never restarts. The server keeps the default
`recv_wait_timeout` (5 s).

## Steps

1. An update by this module already running: `ESP_ERR_INVALID_STATE`, at
   once. The running or the update partition not found: `ESP_ERR_NOT_FOUND`.
2. The body is read in pieces of at most 4 KiB into a PSRAM buffer
   (`ESP_ERR_NO_MEM` without PSRAM room; no buffer in internal RAM).
3. The start of the image is checked before anything goes to flash.
4. If the partition set to boot next is not the running one (an earlier
   update without a restart) or cannot be read, the boot partition is set
   back to the running one, before `esp_ota_begin()`.
5. The image is written to the other partition as it arrives.
6. After the last byte ESP-IDF validates the whole image; only then that
   partition is set to boot next.
7. `ESP_OK`.

On a failure after writing has begun the partial image is abandoned and the
board still boots the running firmware.

## Checks

| Upload | Result |
|---|---|
| longer than the partition (0x300000) or shorter than image header + first segment header + `esp_app_desc_t` (288 bytes) | `ESP_ERR_INVALID_SIZE`, before reading |
| first byte not `0xE9`, chip id not ESP32-S3, `hash_appended` not 1, description magic not `0xABCD5432` | `ESP_ERR_IMAGE_INVALID` |
| another kind than the running firmware (the project name up to the first `_`: `airplay-gateway`, `airplay-speaker`), or no kind | `ESP_ERR_NOT_SUPPORTED` |
| 10 s without a byte | `ESP_ERR_TIMEOUT` |
| connection closed early, other read error | `ESP_FAIL` |
| the complete image fails validation | `ESP_ERR_OTA_VALIDATE_FAILED` |
| flash or OTA errors | the ESP-IDF code |

Any version of the same kind is accepted, also the same or an older one.

Log tag `ota`: one line at the start (size, partition, image name and
version), 10 % to 90 %, one line at the end (the partition that boots next,
or the reason, `ESP_LOGE`). At most 4 KiB internal RAM, no task of its own.

## Examples

With the v4.5.49 images (Gateway, 1328736 bytes; speaker, 1796816 bytes)
and a text file:

1. Gateway, Gateway image: accepted.
2. Gateway, speaker image: `ESP_ERR_NOT_SUPPORTED`, flash untouched.
3. Speaker, Gateway image: `ESP_ERR_NOT_SUPPORTED`.
4. The text file: `ESP_ERR_IMAGE_INVALID`.
5. Chip id changed to 0 (ESP32): `ESP_ERR_IMAGE_INVALID`.
6. Length 0x300001: `ESP_ERR_INVALID_SIZE`, before reading the body.
7. Length 100: `ESP_ERR_INVALID_SIZE`.
8. A Gateway image of another version: accepted.
9. Connection closed half way: `ESP_FAIL`, the boot partition unchanged.
10. `hash_appended` 0 or 2: `ESP_ERR_IMAGE_INVALID`.
11. Running firmware named without `_`: `ESP_ERR_NOT_SUPPORTED`.
12. A second call while the first writes: `ESP_ERR_INVALID_STATE`, the first
    goes on.
13. An earlier update set the other partition to boot next, then a new upload
    breaks off half way: the board boots the running firmware.
