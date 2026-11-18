#pragma once

/* The Gateway's USB host (ROADMAP_USB_PLAYER.md, phase 4;
 * CONFIG_GATEWAY_USB_PLAYER_USB). One place installs the USB host library
 * and runs its events, so other classes (the UMIK-1 of ROADMAP_ROOM_EQ_MIC.md)
 * can join as clients later; hubs are on. The first client is mass storage:
 * a FAT32 drive is mounted at /usb when it is plugged in and let go when it
 * is pulled out, while the Gateway runs. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define GW_USB_MOUNT "/usb"

typedef struct {
  const char *state;  /* "starting", "no drive", "mounting", "ready", "not FAT32", "error" */
  char message[96];
  uint16_t vid, pid;
  uint64_t size_bytes, free_bytes;
} gw_usb_status_t;

/* Once at boot: the USB host on the native port. */
esp_err_t gw_usb_start(void);

/* A drive is mounted at GW_USB_MOUNT. */
bool gw_usb_ready(void);

void gw_usb_status(gw_usb_status_t *out);
