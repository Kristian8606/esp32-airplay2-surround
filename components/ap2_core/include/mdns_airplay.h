/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Announces the Gateway to AirPlay senders over mDNS: the _airplay._tcp and
 * _raop._tcp services on TCP 7000. See spec/spec.md. */

/* Starts mDNS, sets the host name and adds both services. Called once at
 * start-up, after Wi-Fi and the device identity are up; failures are logged
 * and the function returns. */
void mdns_airplay_init(void);
void mdns_airplay_update_flags(void);
/* Republish both service instance names and the hostname without reboot. */
esp_err_t mdns_airplay_update_name(const char *name);
void mdns_airplay_set_group(const char *gid, bool contains_leader);
