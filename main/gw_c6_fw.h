#pragma once
/* The C6's firmware (P4 Gateway; phase 5 of ROADMAP_GATEWAY_P4).
 *
 * The P4 build carries the C6 firmware (coprocessor/c6/prebuilt/, embedded by
 * main/CMakeLists.txt). At start the Gateway compares it with what the C6
 * runs: the C6's HELLO (gw_c6_time) gives its project name and version; the
 * factory firmware sends none. Another name, another version or no HELLO
 * within GW_C6_FW_HELLO_WAIT_MS: the Gateway installs its image over ESP-Hosted
 * (coprocessor OTA). The C6 then restarts into it, the Gateway restarts too
 * and checks again. At most GW_C6_FW_MAX_TRIES automatic tries per image
 * (counted in NVS), so a C6 that cannot take the image does not restart the
 * Gateway for ever; a power-on (or the reset pin) starts the count again, so
 * the Gateway never stops trying for good. */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define GW_C6_FW_HELLO_WAIT_MS 20000
#define GW_C6_FW_MAX_TRIES 3

typedef enum {
  GW_C6_FW_CHECKING,   /* waiting for the C6's HELLO */
  GW_C6_FW_CURRENT,    /* the C6 runs the Gateway's image */
  GW_C6_FW_UPDATING,   /* sending the image; the Gateway restarts after it */
  GW_C6_FW_GAVE_UP,    /* GW_C6_FW_MAX_TRIES failed: again at the next power-on */
  GW_C6_FW_FAILED,     /* the last update did not complete */
} gw_c6_fw_state_t;

typedef struct {
  gw_c6_fw_state_t state;
  char image_version[32]; /* the image the Gateway carries */
  char c6_version[32];    /* what the C6 says it runs, "" without HELLO */
  uint32_t percent;       /* while updating */
  char message[96];
} gw_c6_fw_status_t;

/* After gw_c6_time_start() and NVS. Starts the check (a task). */
esp_err_t gw_c6_fw_start(void);

/* Install the image now (the web page's button), whatever the C6 runs. The
 * Gateway restarts after it. ESP_ERR_INVALID_STATE while one runs. */
esp_err_t gw_c6_fw_update(void);

void gw_c6_fw_get_status(gw_c6_fw_status_t *out);
