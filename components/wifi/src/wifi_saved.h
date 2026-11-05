/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* The saved networks, read once at start (README.md: "Stack of the event loop
 * task"). Only ESP-IDF's log and settings.h: the host tests run it. */

#include <stddef.h>

#include "settings.h"
#include "wifi_logic.h"

/* Copies the saved networks into net[] and points saved[] at them, in their
 * order. One that can't be read (an error, or an empty SSID) is left out
 * with a warning; this never fails. Returns how many were copied, 0 to
 * SETTINGS_MAX_WIFI_NETWORKS: with 0 the module works as with no saved
 * network. Main task only (it reads settings). */
size_t wifi_saved_load(settings_wifi_network_t net[SETTINGS_MAX_WIFI_NETWORKS],
                       wl_saved_t saved[SETTINGS_MAX_WIFI_NETWORKS]);
