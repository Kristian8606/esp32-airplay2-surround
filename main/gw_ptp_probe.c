#include "gw_ptp_probe.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gw_lwip_hooks.h"
#include "lwip/pbuf.h"

static const char *TAG = "gw_ptp_probe";

#define PROBE_PTP_EVENT_PORT 319U
#define PROBE_PTP_SYNC       0U
/* Syncs seen by the driver or lwIP but not yet read by the PTP task. */
#define PROBE_RX_SLOTS       32U
/* Syncs of one log window (8 per second, 10 s, with room). */
#define PROBE_WINDOW         128U

typedef struct {
  uint16_t seq;
  bool used;
  int64_t drv_ns; /* the Wi-Fi task handed the frame to lwIP */
  int64_t ip_ns;  /* lwIP's TCP/IP task took it in */
} rx_slot_t;

/* Written by the Wi-Fi and TCP/IP tasks, read by the PTP task. */
static rx_slot_t s_rx[PROBE_RX_SLOTS];
static uint32_t s_rx_next;
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;

/* One window, written by the PTP task, read by the speakers task. */
static struct {
  int32_t mbox_us[PROBE_WINDOW]; /* lwIP stamp - driver stamp */
  int32_t task_us[PROBE_WINDOW]; /* PTP task stamp - lwIP stamp */
  int64_t drv_off[PROBE_WINDOW]; /* sender time - driver stamp */
  int64_t ptp_off[PROBE_WINDOW]; /* sender time - PTP task stamp */
  uint32_t n, unmatched;
} s_win;
static portMUX_TYPE s_win_mux = portMUX_INITIALIZER_UNLOCKED;

static inline int64_t now_ns(void) { return (int64_t)esp_timer_get_time() * 1000LL; }

/* The sequence number of a PTP Sync in an IPv4 packet, from its UDP header
 * on. ip_hdr[0..9] and udp[0..39] as read from the packet. */
static bool sync_seq(const uint8_t *udp, uint16_t *seq) {
  const uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
  if (dport != PROBE_PTP_EVENT_PORT || (udp[8] & 0x0FU) != PROBE_PTP_SYNC)
    return false;
  *seq = (uint16_t)((udp[8 + 30] << 8) | udp[8 + 31]);
  return true;
}

/* The slot of seq, or a new one. Caller holds s_rx_mux. */
static rx_slot_t *slot_for(uint16_t seq) {
  for (uint32_t i = 0; i < PROBE_RX_SLOTS; ++i) {
    if (s_rx[i].used && s_rx[i].seq == seq) return &s_rx[i];
  }
  rx_slot_t *r = &s_rx[s_rx_next++ % PROBE_RX_SLOTS];
  *r = (rx_slot_t){.seq = seq, .used = true};
  return r;
}

void gw_ptp_probe_on_frame(const void *frame, size_t len) {
  const int64_t t = now_ns();
  const uint8_t *f = frame;
  /* Ethernet II, IPv4, UDP. */
  if (!f || len < 14U + 20U + 8U + 32U) return;
  if (f[12] != 0x08 || f[13] != 0x00) return;
  const uint8_t *ip = f + 14;
  const size_t ihl = (size_t)(ip[0] & 0x0FU) * 4U;
  if (ihl < 20U || ip[9] != 17U || len < 14U + ihl + 8U + 32U) return;
  uint16_t seq;
  if (!sync_seq(ip + ihl, &seq)) return;
  taskENTER_CRITICAL(&s_rx_mux);
  slot_for(seq)->drv_ns = t;
  taskEXIT_CRITICAL(&s_rx_mux);
}

int gw_ptp_probe_ip4_input(struct pbuf *p, struct netif *inp) {
  (void)inp;
  const int64_t t = now_ns();
  uint8_t ip[10];
  if (!p || pbuf_copy_partial(p, ip, sizeof(ip), 0) != sizeof(ip)) return 0;
  const uint16_t ihl = (uint16_t)((ip[0] & 0x0FU) * 4U);
  if (ihl < 20U || ip[9] != 17U) return 0;
  uint8_t udp[8 + 32];
  if (pbuf_copy_partial(p, udp, sizeof(udp), ihl) != sizeof(udp)) return 0;
  uint16_t seq;
  if (!sync_seq(udp, &seq)) return 0;
  taskENTER_CRITICAL(&s_rx_mux);
  slot_for(seq)->ip_ns = t;
  taskEXIT_CRITICAL(&s_rx_mux);
  return 0;
}

void gw_ptp_probe_on_sync(uint16_t seq, int64_t task_rx_ns,
                          int64_t raw_offset_ns) {
  rx_slot_t r = {0};
  bool found = false;
  taskENTER_CRITICAL(&s_rx_mux);
  for (uint32_t i = 0; i < PROBE_RX_SLOTS; ++i) {
    rx_slot_t *s = &s_rx[i];
    if (s->used && s->seq == seq) {
      r = *s;
      s->used = false;
      found = r.drv_ns > 0 && r.ip_ns >= r.drv_ns && task_rx_ns >= r.ip_ns &&
              task_rx_ns - r.drv_ns < 1000000000LL;
      break;
    }
  }
  taskEXIT_CRITICAL(&s_rx_mux);

  taskENTER_CRITICAL(&s_win_mux);
  if (!found) {
    s_win.unmatched++;
  } else if (s_win.n < PROBE_WINDOW) {
    const uint32_t i = s_win.n++;
    s_win.mbox_us[i] = (int32_t)((r.ip_ns - r.drv_ns) / 1000LL);
    s_win.task_us[i] = (int32_t)((task_rx_ns - r.ip_ns) / 1000LL);
    s_win.ptp_off[i] = raw_offset_ns;
    s_win.drv_off[i] = raw_offset_ns + (task_rx_ns - r.drv_ns);
  }
  taskEXIT_CRITICAL(&s_win_mux);
}

static int cmp_i32(const void *a, const void *b) {
  const int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
  return x < y ? -1 : x > y;
}

typedef struct {
  int32_t med, p90, max;
} pct_t;

/* Median, 90th percentile and maximum of v[0..n) (sorted in place), in us. */
static pct_t stats(int32_t *v, uint32_t n) {
  qsort(v, n, sizeof(v[0]), cmp_i32);
  return (pct_t){v[n / 2U], v[(n * 9U) / 10U], v[n - 1U]};
}

/* Lateness against the best (highest) offset of the window, in us. Ten
 * seconds of crystal drift (< 1 ms at 100 ppm) is left in. */
static void lateness(const int64_t *off, uint32_t n, int32_t *out) {
  int64_t best = off[0];
  for (uint32_t i = 1; i < n; ++i)
    if (off[i] > best) best = off[i];
  for (uint32_t i = 0; i < n; ++i) out[i] = (int32_t)((best - off[i]) / 1000LL);
}

void gw_ptp_probe_log(void) {
  static int32_t mbox[PROBE_WINDOW], task[PROBE_WINDOW], air[PROBE_WINDOW],
      total[PROBE_WINDOW];
  static int64_t doff[PROBE_WINDOW], poff[PROBE_WINDOW];
  uint32_t n, unmatched;
  taskENTER_CRITICAL(&s_win_mux);
  n = s_win.n;
  unmatched = s_win.unmatched;
  memcpy(mbox, s_win.mbox_us, n * sizeof(mbox[0]));
  memcpy(task, s_win.task_us, n * sizeof(task[0]));
  memcpy(doff, s_win.drv_off, n * sizeof(doff[0]));
  memcpy(poff, s_win.ptp_off, n * sizeof(poff[0]));
  s_win.n = 0;
  s_win.unmatched = 0;
  taskEXIT_CRITICAL(&s_win_mux);
  if (n < 4U) {
    if (n || unmatched)
      ESP_LOGI(TAG, "PTP SYNC DELAY: %u Syncs stamped, %u not", (unsigned)n,
               (unsigned)unmatched);
    return;
  }
  lateness(doff, n, air);
  lateness(poff, n, total);
  const pct_t a = stats(air, n), m = stats(mbox, n), t = stats(task, n),
              all = stats(total, n);
  ESP_LOGI(TAG,
           "PTP SYNC DELAY ms med/p90/max (%u Syncs%s): Wi-Fi up to the "
           "driver %.2f/%.2f/%.2f | lwIP queue %.2f/%.2f/%.2f | socket + PTP "
           "task %.2f/%.2f/%.2f | total %.2f/%.2f/%.2f",
           (unsigned)n, unmatched ? ", some unstamped" : "", a.med / 1000.0,
           a.p90 / 1000.0, a.max / 1000.0, m.med / 1000.0, m.p90 / 1000.0,
           m.max / 1000.0, t.med / 1000.0, t.p90 / 1000.0, t.max / 1000.0,
           all.med / 1000.0, all.p90 / 1000.0, all.max / 1000.0);
}
