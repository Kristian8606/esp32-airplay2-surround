/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Functions ap2_core calls and each firmware implements itself. */

/* The device MAC that Apple sees on the network: "deviceid" of /info and
 * the HomeKit identity. Must be the same on every boot. */
esp_err_t ap2_port_device_mac(uint8_t mac[6]);

/* HomeKit (CONFIG_AIRPLAY_HOMEKIT): called after acknowledging an
 * authenticated removal from Apple Home. Hands the HomeKit reset (erase the
 * keys, reboot) to the firmware's own task and returns at once, without heap
 * allocation. */
void ap2_port_homekit_reset_schedule(void);
