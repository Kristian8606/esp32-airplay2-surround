#pragma once

/* lwIP hooks of the Gateway (ESP_IDF_LWIP_HOOK_FILENAME, set for the lwip
 * component in main/CMakeLists.txt): included by lwip's own sources only. */

struct pbuf;
struct netif;

/* gw_ptp_probe.c: notes when lwIP takes in each PTP Sync. Never eats the
 * packet (returns 0). Runs in the TCP/IP task. */
int gw_ptp_probe_ip4_input(struct pbuf *p, struct netif *inp);

#define LWIP_HOOK_IP4_INPUT(p, inp) gw_ptp_probe_ip4_input((p), (inp))
