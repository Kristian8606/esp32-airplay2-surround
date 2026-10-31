#pragma once

/* Gateway: the speaker firmware it hands out.
 *
 * The speakers page uploads a speaker .bin once; the Gateway keeps it in the
 * "storage" partition (otherwise unused) with its size, SHA-256 and build id
 * in NVS. gw_speakers.c then sends FW_UPDATE to every speaker whose HELLO
 * shows another build, one at a time and at once (a speaker in a stream
 * stops playing for it), and the speaker fetches the image from
 * gw_fw_serve(). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
  bool valid;
  char id[24];          /* "v4.5.9 1a2b3c4d", as firmware_id() on a speaker */
  uint32_t size;
  uint8_t sha256[32];
  uint32_t generation;  /* changes with every upload */
  /* The rollout stopped because this speaker did not come back with the
   * image (gw_fw_halt); the next upload clears it. Kept across restarts. */
  bool halted;
  char halted_by[24];
} gw_fw_image_t;

esp_err_t gw_fw_init(void);
void gw_fw_get(gw_fw_image_t *out);
/* The partition's size: the largest image that fits. */
size_t gw_fw_capacity(void);

/* Stop handing out the stored image: a speaker that installed it did not
 * come back with it, so it may not start. Until the next upload. */
void gw_fw_halt(const char *speaker_name);

/* POST body: a speaker .bin. On failure *why says what was wrong. */
esp_err_t gw_fw_receive(httpd_req_t *req, const char **why);
/* GET: the stored image (404 when there is none). */
esp_err_t gw_fw_serve(httpd_req_t *req);
