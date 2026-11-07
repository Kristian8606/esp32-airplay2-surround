/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Announces the Gateway to AirPlay senders over mDNS: the _airplay._tcp and
 * _raop._tcp services on TCP 7000. See spec/spec.md. */

/* Starts mDNS, sets the host name and adds both services. Called once at
 * start-up, after Wi-Fi and the device identity are up; failures are logged
 * and the function returns. */
void mdns_airplay_init(void);
