#pragma once

/* Speaker: install the firmware the Gateway holds (FW_UPDATE). A task of its
 * own fetches the image from the Gateway's web server, checks its size and
 * SHA-256 while writing it to the other OTA slot, and restarts into it at
 * once. A stream that plays is stopped first (spk_link.c) and the link
 * ignores stream commands meanwhile; a failure leaves the running firmware
 * as it is, and the Gateway tries again later. */

#include <stdbool.h>
#include <stdint.h>

#include "surround_proto.h"

/* gateway_ip in network byte order (the link's peer). Ignored while an
 * install is already running. */
void spk_update_start(uint32_t gateway_ip, const surround_fw_update_t *m);

/* An install is running: this speaker sits out any stream. */
bool spk_update_running(void);

/* The Gateway accepted this speaker (first PING or TIME): a firmware that
 * came from an update is good and stays (app rollback). Cheap after the
 * first call. */
void spk_update_confirm(void);
