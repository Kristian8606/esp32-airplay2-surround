#pragma once

/* Gateway diagnostic: where the delay of the Apple TV's PTP Syncs comes from.
 *
 * Each Sync gets three time stamps: when the Wi-Fi task hands the frame to
 * lwIP (wifi_set_sta_rx_probe), when lwIP's TCP/IP task takes it in (an IPv4
 * input hook, gw_lwip_hooks.h) and when the PTP task reads it after select()
 * (ptp_clock.c, the stamp the clock uses). The differences are the Gateway's
 * own delays: lwIP's queue, then socket and task scheduling. How far each
 * Sync lies below the best one of the window on the first stamp is the delay
 * before the Gateway's software: the air, the access point, the driver. Logged every 10 s; compare with the RELAY lines of the same time
 * (audio relayed or not). */

#include <stddef.h>
#include <stdint.h>

/* For wifi_set_sta_rx_probe(): every received frame, in the Wi-Fi task. */
void gw_ptp_probe_on_frame(const void *frame, size_t len);

/* For ptp_clock_set_sync_probe(): one Sync, its PTP-task stamp and offset. */
void gw_ptp_probe_on_sync(uint16_t seq, int64_t task_rx_ns,
                          int64_t raw_offset_ns);

/* Log the window and start a new one (speakers task, every 10 s). */
void gw_ptp_probe_log(void);
