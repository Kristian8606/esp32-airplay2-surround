#include "apple_clock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "apple_clock_core.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "apple_clock";
#define NS_PER_S 1000000000LL
#define REQ_MIN_NS 125000000LL
#define REQ_TIMEOUT_NS NS_PER_S
#define SYNC_FRESH_NS 500000000LL
#define RATE_MEMORY 4U

typedef struct { uint64_t id; int32_t ppb; } rate_mem_t;
static rate_mem_t s_mem[RATE_MEMORY];
static uint32_t s_mem_next;

/* Devices that announce themselves as the master (port identity = GM). */
#define MASTERS 8U
#define MASTER_FRESH_NS (5LL * NS_PER_S)
typedef struct {
  uint32_t ip;
  uint64_t gm;
  uint16_t port;
  uint8_t sdo, domain;
  int64_t seen_ns;
} master_t;
static master_t s_masters[MASTERS];
static uint32_t s_masters_next;

/* Only the transport task writes the core and protocol state. */
static struct {
  int event_fd, general_fd;
  acc_t *core;
  uint64_t clock_id, source, gm;
  uint16_t source_port, sender_steps;
  /* The sender names the master; peer is the master itself, 0 until it
   * announces itself to us. */
  uint32_t sender, peer, generation;
  bool realtime, announced, sync_waiting, outstanding;
  uint8_t sdo, domain;
  uint16_t sync_seq, req_seq;
  int64_t sync_rx, sync_corr, t1, t2, t3, next_req, period;
  uint32_t sent, answered, send_errors;
} s;

/* RTSP control and the published snapshot, shared with other tasks. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct { uint32_t ip, generation; bool realtime; } s_selection;
static apple_clock_line_t s_line;
static bool s_started;

static int64_t now_ns(void) { return esp_timer_get_time() * 1000LL; }
static uint64_t be64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int64_t correction(const uint8_t *p) { return (int64_t)be64(p + 8) / 65536LL; }
static bool timestamp(const uint8_t *p, int64_t *out) {
  uint64_t sec = 0;
  for (int i = 0; i < 6; ++i) sec = (sec << 8) | p[i];
  uint32_t ns = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) |
                ((uint32_t)p[8] << 8) | p[9];
  if (ns >= NS_PER_S || sec > (uint64_t)(INT64_MAX - ns) / NS_PER_S) return false;
  *out = (int64_t)(sec * NS_PER_S + ns);
  return true;
}
static bool corrected(int64_t ts, int64_t corr, int64_t *out) {
  return !__builtin_add_overflow(ts, corr, out) && *out >= 0;
}
static void remember(void) {
  acc_line_t l;
  acc_get_line(s.core, &l);
  if (!s.gm || l.span_s < 60 || !l.points) return;
  uint32_t k = s_mem_next % RATE_MEMORY;
  for (uint32_t i = 0; i < RATE_MEMORY; ++i)
    if (s_mem[i].id == s.gm) { k = i; break; }
  if (s_mem[k].id != s.gm) s_mem_next++;
  s_mem[k] = (rate_mem_t){s.gm, l.est_rate_ppb};
}
#define IP4(ip) ((const uint8_t *)&(ip))[0], ((const uint8_t *)&(ip))[1], \
                ((const uint8_t *)&(ip))[2], ((const uint8_t *)&(ip))[3]

/* A new master or measured device: the fit starts again from the rate
 * remembered for the master (remember() the old fit first). */
static void restart(void) {
  int32_t ppb = 0, sigma = 30000;
  for (uint32_t i = 0; i < RATE_MEMORY; ++i)
    if (s.gm && s_mem[i].id == s.gm) { ppb = s_mem[i].ppb; sigma = 2000; break; }
  acc_restart(s.core, ppb, sigma);
  if (!s.peer)
    ESP_LOGI(TAG, "master %016llx: new clock, initial rate %+.3f ppm, waiting "
             "for it to announce itself", (unsigned long long)s.gm, ppb / 1000.0);
  else if (s.peer != s.sender)
    ESP_LOGI(TAG, "master %016llx: new clock, initial rate %+.3f ppm, measured "
             "at the master %u.%u.%u.%u (the sender relays it, %u steps)",
             (unsigned long long)s.gm, ppb / 1000.0, IP4(s.peer),
             (unsigned)s.sender_steps);
  else
    ESP_LOGI(TAG, "master %016llx: new clock, initial rate %+.3f ppm, measured "
             "at the master %u.%u.%u.%u (the sender itself)",
             (unsigned long long)s.gm, ppb / 1000.0, IP4(s.peer));
}

static master_t *master_of(uint64_t gm, int64_t now) {
  for (uint32_t i = 0; i < MASTERS; ++i)
    if (s_masters[i].gm == gm && now - s_masters[i].seen_ns < MASTER_FRESH_NS)
      return &s_masters[i];
  return NULL;
}

/* Only the master gives the time: measured at the device that announces
 * itself as the master the sender names. A sender that relays it adds its
 * own error: an iPhone two steps from a HomePod master gave a line 5 ms off
 * for 12 s and 7 steps in ten minutes on the board (v4.5.38); the iPhone as
 * its own master, 0 steps. True when the device changed. */
static bool choose_peer(int64_t now) {
  const master_t *m = master_of(s.gm, now);
  const uint32_t want = m ? m->ip : 0;
  if (want == s.peer) return false;
  s.peer = want;
  s.sync_waiting = s.outstanding = false;
  s.next_req = 0;
  s.period = REQ_MIN_NS;
  s.announced = m != NULL;
  if (m) {
    s.source = s.gm; s.source_port = m->port;
    s.sdo = m->sdo; s.domain = m->domain;
  }
  return true;
}

void apple_clock_select(uint32_t ip, bool realtime) {
  if (!ip) return;
  taskENTER_CRITICAL(&s_mux);
  if (s_selection.ip != ip) {
    s_selection.ip = ip;
    s_selection.generation++;
    s_line.ready = false; /* no old peer's time after SETUP */
  }
  s_selection.realtime = realtime;
  s_line.realtime = realtime;
  taskEXIT_CRITICAL(&s_mux);
}
void apple_clock_release(void) {
  /* The selected clock survives TEARDOWN/SETUP between tracks. */
  taskENTER_CRITICAL(&s_mux);
  s_selection.realtime = false;
  s_line.realtime = false;
  taskEXIT_CRITICAL(&s_mux);
}
static void selection(void) {
  taskENTER_CRITICAL(&s_mux);
  const uint32_t gen = s_selection.generation, ip = s_selection.ip;
  s.realtime = s_selection.realtime;
  taskEXIT_CRITICAL(&s_mux);
  if (gen == s.generation) return;
  s.generation = gen;
  if (s.sender == ip) return; /* pre-SETUP acquisition of this same sender */
  s.sender = ip;
  /* Retain the fit until the sender's Announce establishes whether the GM
   * changed. */
}

static void request(int64_t t1, int64_t t2) {
  const int64_t now = now_ns();
  if (s.outstanding || now < s.next_req || now - t2 >= SYNC_FRESH_NS) return;
  uint8_t b[44] = {0};
  const uint16_t seq = ++s.req_seq;
  b[0] = s.sdo | 1U; b[1] = 2; b[3] = sizeof(b); b[4] = s.domain;
  for (int i = 0; i < 8; ++i) b[20+i] = (uint8_t)(s.clock_id >> (56-8*i));
  b[29] = 1; b[30] = seq >> 8; b[31] = seq; b[32] = 1; b[33] = 0x7f;
  const struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(319),
                                  .sin_addr.s_addr = s.peer};
  s.t1 = t1; s.t2 = t2; s.t3 = now_ns();
  s.next_req = s.t3 + s.period; /* also bound retries after a send error */
  if (sendto(s.event_fd, b, sizeof(b), MSG_DONTWAIT,
             (const struct sockaddr *)&dst, sizeof(dst)) != sizeof(b)) {
    s.send_errors++;
    return;
  }
  s.outstanding = true;
  s.sent++;
}

/* An Announce. A master announcing itself is remembered by its address;
 * the sender's names the master in use. */
static void announce(const uint8_t *d, uint32_t ip, uint64_t src, uint16_t port) {
  const uint64_t gm = be64(d+53);
  const uint16_t steps = be16(d+61);
  const int64_t now = now_ns();
  if (!gm) return;
  if (src == gm) {
    master_t *m = master_of(gm, INT64_MIN / 2);
    if (!m) m = &s_masters[s_masters_next++ % MASTERS];
    if (m->ip != ip || m->gm != gm)
      ESP_LOGI(TAG, "master %016llx announces itself from %u.%u.%u.%u",
               (unsigned long long)gm, IP4(ip));
    *m = (master_t){ip, gm, port, d[0] & 0xf0U, d[4], now};
  }
  bool fresh = false;
  if (ip == s.sender) {
    if (gm != s.gm) {
      remember();
      s.gm = gm;
      fresh = true;
    }
    s.sender_steps = steps;
  }
  if (choose_peer(now)) {
    if (!fresh) remember();
    fresh = true;
  }
  if (fresh) restart();
  if (!s.peer || ip != s.peer || src != s.gm) return;
  if (src != s.source || port != s.source_port || d[4] != s.domain ||
      (d[0] & 0xf0U) != s.sdo)
    s.sync_waiting = s.outstanding = false;
  s.source = src; s.source_port = port;
  s.sdo = d[0] & 0xf0U; s.domain = d[4]; s.announced = true;
}

static void message(const uint8_t *d, size_t len, bool event, uint32_t ip,
                    int64_t rx) {
  if (len < 34 || (d[1] & 15U) != 2 || be16(d+2) < 34 || be16(d+2) > len) return;
  len = be16(d+2);
  const uint8_t type = d[0] & 15U;
  const uint64_t src = be64(d+20);
  const uint16_t port = be16(d+28), seq = be16(d+30);
  if (!ip || !src || !port || src == s.clock_id) return;
  /* Before SETUP, the first Announce's device is the sender. Only the
   * master the sender names gives the time: other HomePods cannot. */
  if (!event && type == 11 && len >= 64) {
    if (!s.sender) s.sender = ip;
    announce(d, ip, src, port);
    return;
  }
  if (!s.peer || ip != s.peer || !s.announced || src != s.source || port != s.source_port ||
      d[4] != s.domain || (d[0] & 0xf0U) != s.sdo) return;
  int64_t ts;
  if (event && type == 0 && len >= 44) {
    s.sync_seq = seq; s.sync_rx = rx; s.sync_corr = correction(d);
    s.sync_waiting = (be16(d+6) & 0x0200U) != 0;
    if (!s.sync_waiting && timestamp(d+34, &ts) && corrected(ts, s.sync_corr, &ts))
      request(ts, rx);
  } else if (!event && type == 8 && len >= 44 && s.sync_waiting &&
             seq == s.sync_seq && timestamp(d+34, &ts)) {
    s.sync_waiting = false;
    if (corrected(ts, s.sync_corr, &ts) && corrected(ts, correction(d), &ts))
      request(ts, s.sync_rx);
  } else if (!event && type == 9 && len >= 54 && s.outstanding &&
             seq == s.req_seq && be64(d+44) == s.clock_id && be16(d+52) == 1 &&
             timestamp(d+34, &ts) && corrected(ts, correction(d), &ts)) {
    s.outstanding = false;
    s.answered++;
    /* Apple receiveTimestamp is its last Sync tick: correction is ADDED. */
    acc_add_exchange(s.core, s.t1, s.t2, s.t3, ts);
    const int8_t lmi = (int8_t)d[33];
    if (lmi >= -7 && lmi <= 4) {
      const int64_t period = lmi >= 0 ? NS_PER_S << lmi : NS_PER_S >> -lmi;
      s.period = period < REQ_MIN_NS ? REQ_MIN_NS : period;
      s.next_req = s.t3 + s.period;
    }
  }
}

void apple_clock_get(apple_clock_line_t *out) {
  if (!out) return;
  taskENTER_CRITICAL(&s_mux);
  *out = s_line;
  taskEXIT_CRITICAL(&s_mux);
}
int64_t apple_clock_at(const apple_clock_line_t *l, int64_t local_ns) {
  const int64_t dt = local_ns - l->base_local_ns;
  return l->base_apple_ns + dt + (int64_t)llround((double)dt * l->rate_ppb / 1e9);
}
bool apple_clock_to_local(const apple_clock_line_t *l, uint64_t id,
                          uint64_t apple_ns, uint64_t *local_ns) {
  if (!l || !local_ns || !l->ready || !id || id != l->grandmaster_clock_id ||
      apple_ns > INT64_MAX) return false;
  const double dt = ((double)((int64_t)apple_ns - l->base_apple_ns)) /
                    (1.0 + l->rate_ppb / 1e9);
  const double local = (double)l->base_local_ns + dt;
  if (local < 0 || local >= (double)INT64_MAX) return false;
  *local_ns = (uint64_t)llround(local);
  return true;
}

static int open_socket(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return -1;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  const struct sockaddr_in addr = {.sin_family = AF_INET,
      .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
  const struct ip_mreq group = {.imr_multiaddr.s_addr = inet_addr("224.0.1.129"),
                               .imr_interface.s_addr = htonl(INADDR_ANY)};
  if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) < 0) {
    ESP_LOGE(TAG, "PTP port %u unavailable: errno %d", port, errno);
    close(fd); return -1;
  }
  return fd;
}
static void clock_task(void *arg) {
  (void)arg;
  int64_t last_log = 0;
  uint32_t epoch = 0;
  bool ready = false;
  acc_stats_t prev = {0};
  for (;;) {
    selection();
    fd_set fds;
    FD_ZERO(&fds); FD_SET(s.event_fd, &fds); FD_SET(s.general_fd, &fds);
    struct timeval timeout = {.tv_usec = 20000};
    const int maxfd = s.event_fd > s.general_fd ? s.event_fd : s.general_fd;
    if (select(maxfd+1, &fds, NULL, NULL, &timeout) > 0) {
      selection();
      const int sockets[2] = {s.event_fd, s.general_fd};
      for (int i = 0; i < 2; ++i) {
        if (!FD_ISSET(sockets[i], &fds)) continue;
        for (int n = 0; n < 32; ++n) {
          uint8_t b[256]; struct sockaddr_in src;
          socklen_t slen = sizeof(src);
          const int64_t rx = now_ns();
          const int len = recvfrom(sockets[i], b, sizeof(b), MSG_DONTWAIT,
                                   (struct sockaddr *)&src, &slen);
          if (len < 0) break;
          message(b, (size_t)len, i == 0, src.sin_addr.s_addr, rx);
        }
      }
    }
    const int64_t now = now_ns();
    if (s.outstanding && now-s.t3 >= REQ_TIMEOUT_NS) s.outstanding = false;
    /* The fit runs here, after the batch: never between receiving a packet
     * and time-stamping the next. */
    acc_update(s.core, now);
    acc_line_t l;
    acc_get_line(s.core, &l);
    const apple_clock_line_t out = {.ready = l.ready && s.announced,
        .realtime = s.realtime, .source_clock_id = s.source,
        .grandmaster_clock_id = s.gm, .epoch = l.epoch,
        .base_local_ns = l.base_local_ns, .base_apple_ns = l.base_apple_ns,
        .rate_ppb = l.rate_ppb, .ready_since_ns = l.ready_since_ns, .samples = l.samples, .points = l.points};
    taskENTER_CRITICAL(&s_mux);
    if (s.generation == s_selection.generation) {
      s_line = out;
      s_line.realtime = s_selection.realtime;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (out.ready && (!ready || epoch != l.epoch))
      ESP_LOGI(TAG, "line ready epoch %u: %u exchanges, %u fit points, rate %+.3f ppm",
               (unsigned)l.epoch, (unsigned)l.samples, (unsigned)l.points,
               l.rate_ppb/1000.0);
    else if (ready && !out.ready)
      ESP_LOGW(TAG, "line unavailable: waiting for master or two-way exchanges");
    ready = out.ready; epoch = l.epoch;
    if (now-last_log >= 10*NS_PER_S) {
      last_log = now;
      remember();
      const acc_stats_t st = s.core->stats;
      ESP_LOGI(TAG, "%s epoch %u | rate %+.3f ppm, estimate %+.3f ms | "
          "floor RTT %.2f ms | %u points / %u s | seconds +%u -%u far %u | "
          "steps %u jumps %u | Delay_Req %u answered %u send errors %u",
          out.ready ? "ready" : "acquiring", (unsigned)l.epoch, l.rate_ppb/1000.0,
          l.est_minus_pub_ns/1e6, l.floor_rtt_ns/1e6, (unsigned)l.points,
          (unsigned)l.span_s, (unsigned)(st.seconds-prev.seconds),
          (unsigned)(st.poor-prev.poor), (unsigned)(st.far-prev.far),
          (unsigned)st.steps, (unsigned)st.jumps, (unsigned)s.sent, (unsigned)s.answered, (unsigned)s.send_errors);
      prev = st;
    }
  }
}

esp_err_t apple_clock_start(void) {
  if (s_started) return ESP_ERR_INVALID_STATE;
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return ESP_FAIL;
  s.core = heap_caps_calloc(1, sizeof(acc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s.core) return ESP_ERR_NO_MEM;
  acc_init(s.core, (int64_t)CONFIG_AIRPLAY_APPLE_CLOCK_ASYM_US * 1000LL);
  const uint8_t eui[8] = {mac[0],mac[1],mac[2],0xff,0xfe,mac[3],mac[4],mac[5]};
  for (int i = 0; i < 8; ++i) s.clock_id = (s.clock_id << 8) | eui[i];
  s.period = REQ_MIN_NS;
  s.event_fd = open_socket(319); s.general_fd = open_socket(320);
  if (s.event_fd >= 0 && s.general_fd >= 0 &&
      xTaskCreatePinnedToCore(clock_task, "apple_clock", 6144, NULL, 6, NULL, 0) == pdPASS) {
    s_started = true;
    ESP_LOGI(TAG, "two-way clock owns UDP 319/320, identity %016llx, asymmetry %d us",
             (unsigned long long)s.clock_id, CONFIG_AIRPLAY_APPLE_CLOCK_ASYM_US);
    return ESP_OK;
  }
  if (s.event_fd >= 0) close(s.event_fd);
  if (s.general_fd >= 0) close(s.general_fd);
  free(s.core); s.core = NULL; s.clock_id = 0;
  return ESP_FAIL;
}
