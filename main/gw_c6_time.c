#include "gw_c6_time.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/gpio_etm.h"
#include "driver/gptimer.h"
#include "driver/gptimer_etm.h"
#include "esp_attr.h"
#include "esp_etm.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_c6_proto.h"
#include "sdkconfig.h"
#include "surround_proto.h"

static const char *TAG = "gw_c6_time";

#define PIN CONFIG_AIRPLAY_GW_TIME_PULSE_GPIO
/* The local GPTimer: PLL_F80M / 2, from the same crystal as esp_timer, so
 * the two differ only by an offset (3.2). */
#define LOCAL_HZ 40000000U
#define LOCAL_NS_PER_TICK 25
/* The C6 clock runs at 40 MHz too (HELLO.clock_hz). */
#define C6_NS_PER_TICK 25
/* Until HELLO says the measured one. */
#define NOMINAL_PERIOD ((uint64_t)LOCAL_HZ / 1000U * SURROUND_TIME_INTERVAL_MS)

#define ISR_RING 32
#define EDGE_RING 64
#define OFF_WINDOW 64          /* edges for the GPTimer -> esp_timer offset */
#define OFF_MAX_SPAN_US 20     /* a pair of reads further apart was interrupted */
#define LINE_N 64              /* pairs in the C6 -> local line (8 s) */
#define LINE_MIN 8
#define LINE_REJECT_NS 20000.0 /* a pair this far off the line is an outlier */
#define PAIR_TOL_PPM 200       /* crystals plus margin */
#define PAIR_TOL_TICKS 40      /* 1 us: capture jitter at both ends */
#define REJECT_RESET 4         /* rejected pairs in a row: pairing starts over */
#define UNPAIRED_REALIGN 16    /* TICKs in a row without their edge: renumber */
#define LATENCY_MAX_NS 2000000000LL
#define PTP_RING 64           /* ~3 s of Apple's PTP traffic */
#define PTP_MAX_AGE_US 2000000
#define HELLO_RETRY_MS 3000
#define STATS_LOG_MS 10000

/* ---- interrupt: read and store only ---- */

typedef struct {
  uint64_t cap; /* GPTimer at the edge (ETM capture) */
  uint64_t raw; /* GPTimer right after, read with esp_timer around it */
  int64_t t0_us, t1_us;
} isr_rec_t;

static gptimer_handle_t s_timer;
static isr_rec_t s_isr[ISR_RING];
static volatile uint32_t s_isr_n;

static void IRAM_ATTR on_edge(void *arg) {
  (void)arg;
  isr_rec_t r;
  gptimer_get_captured_count(s_timer, &r.cap); /* first: before any software read */
  r.t0_us = esp_timer_get_time();
  gptimer_get_raw_count(s_timer, &r.raw);
  r.t1_us = esp_timer_get_time();
  const uint32_t n = s_isr_n;
  s_isr[n % ISR_RING] = r;
  s_isr_n = n + 1;
}

/* ---- state, under s_lock ---- */

typedef struct {
  uint32_t k;     /* the edge's number, counted as the C6 counts */
  uint64_t cap;
  int64_t local_ns;
} edge_t;

static SemaphoreHandle_t s_lock;
static uint32_t s_isr_done;

static int64_t s_off_win[OFF_WINDOW]; /* (esp_timer ns) - raw * 25 */
static uint32_t s_off_n, s_off_head;
static int64_t s_off_sum;

static edge_t s_edge[EDGE_RING];
static uint32_t s_edge_n;
static bool s_have_prev_cap;
static uint64_t s_prev_cap;
static uint32_t s_k;

static bool s_have_pair;
static uint64_t s_pair_c6, s_pair_cap;
static uint32_t s_reject_run, s_unpaired_run;

/* The C6 -> local line: y = local - ref_local - (c6 - ref_c6) * 25 against
 * x = c6 - ref_c6 (C6 ticks), both relative so doubles keep the precision. */
static uint64_t s_ref_c6;
static int64_t s_ref_local;
static double s_lx[LINE_N], s_ly[LINE_N];
static uint32_t s_ln, s_lhead;
static double s_lmx, s_lmy, s_lslope, s_lrms;

/* The newest edge with its TSF, for tsf_fit: only the newest, so the
 * speakers' TIME is as fresh as the last edge (an older one waiting behind
 * it made TIME ~0.4 s old). */
static struct {
  int64_t local_ns, tsf_ns;
  bool have;
} s_sample;

typedef struct {
  uint8_t type;
  uint16_t seq;
  uint8_t port_id[10];
  uint64_t c6_ticks;
  int64_t arrival_us;
} ptp_rec_t;
static ptp_rec_t s_ptp[PTP_RING];
static uint32_t s_ptp_n;

static uint64_t s_period; /* C6 ticks per pulse, from HELLO */
static gw_c6_time_stats_t s_st;
static bool s_proto_warned;

/* ---- GPTimer -> esp_timer ---- */

static void off_add(const isr_rec_t *r) {
  if (r->t1_us - r->t0_us > OFF_MAX_SPAN_US) return;
  const int64_t off = (r->t0_us + r->t1_us) * 500LL - (int64_t)r->raw * LOCAL_NS_PER_TICK;
  if (s_off_n == OFF_WINDOW) {
    s_off_sum -= s_off_win[s_off_head];
  } else {
    s_off_n++;
  }
  s_off_win[s_off_head] = off;
  s_off_sum += off;
  s_off_head = (s_off_head + 1) % OFF_WINDOW;
}

static bool cap_to_local(uint64_t cap, int64_t *local_ns) {
  if (s_off_n == 0) return false;
  *local_ns = s_off_sum / (int64_t)s_off_n + (int64_t)cap * LOCAL_NS_PER_TICK;
  return true;
}

/* ---- edges ---- */

static void pairing_reset(void) {
  s_have_pair = false;
  s_reject_run = 0;
  s_ln = s_lhead = 0;
}

static void numbering_restart(void) {
  s_edge_n = 0;
  s_k = 0;
  s_st.restarts++;
  pairing_reset();
}

static void edge_add(const isr_rec_t *r) {
  off_add(r);
  if (s_have_prev_cap) {
    const uint64_t period = s_period ? s_period : NOMINAL_PERIOD;
    const uint64_t d = r->cap - s_prev_cap;
    const uint64_t n = (d + period / 2) / period;
    const int64_t err = (int64_t)d - (int64_t)(n * period);
    if (n >= 1 && n <= 8 && llabs(err) < (int64_t)(period / 200)) {
      s_k += (uint32_t)n;
      s_st.edges_missed += (uint32_t)(n - 1);
    } else {
      /* Not the pulse (a glitch while the C6 starts, or the pulse began
       * again): the next regular edges are numbered from here, 0. */
      numbering_restart();
    }
  }
  s_prev_cap = r->cap;
  s_have_prev_cap = true;
  s_st.edges++;
  int64_t local_ns = 0;
  if (!cap_to_local(r->cap, &local_ns)) return;
  edge_t *e = &s_edge[s_edge_n % EDGE_RING];
  e->k = s_k;
  e->cap = r->cap;
  e->local_ns = local_ns;
  s_edge_n++;
}

static void edges_update(void) {
  const uint32_t n = s_isr_n;
  if (n - s_isr_done > ISR_RING) {
    s_isr_done = n - ISR_RING; /* overrun: the oldest are gone */
  }
  while (s_isr_done != n) {
    isr_rec_t r = s_isr[s_isr_done % ISR_RING];
    edge_add(&r);
    s_isr_done++;
  }
}

static const edge_t *edge_by_seq(uint16_t seq) {
  const uint32_t m = s_edge_n < EDGE_RING ? s_edge_n : EDGE_RING;
  for (uint32_t i = 0; i < m; i++) {
    const edge_t *e = &s_edge[(s_edge_n - 1 - i) % EDGE_RING];
    if ((uint16_t)e->k == seq) return e;
  }
  return NULL;
}

/* The newest edge at or before t: for renumbering when TICKs find none. */
static const edge_t *edge_before(int64_t t_ns) {
  const uint32_t m = s_edge_n < EDGE_RING ? s_edge_n : EDGE_RING;
  for (uint32_t i = 0; i < m; i++) {
    const edge_t *e = &s_edge[(s_edge_n - 1 - i) % EDGE_RING];
    if (e->local_ns <= t_ns) return e;
  }
  return NULL;
}

/* ---- the C6 -> local line ---- */

static void line_recompute(void) {
  const uint32_t n = s_ln;
  double sx = 0, sy = 0;
  for (uint32_t i = 0; i < n; i++) { sx += s_lx[i]; sy += s_ly[i]; }
  s_lmx = sx / n;
  s_lmy = sy / n;
  double sxx = 0, sxy = 0;
  for (uint32_t i = 0; i < n; i++) {
    const double dx = s_lx[i] - s_lmx;
    sxx += dx * dx;
    sxy += dx * (s_ly[i] - s_lmy);
  }
  s_lslope = sxx > 0 ? sxy / sxx : 0;
  double sr = 0;
  for (uint32_t i = 0; i < n; i++) {
    const double r = s_ly[i] - (s_lmy + s_lslope * (s_lx[i] - s_lmx));
    sr += r * r;
  }
  s_lrms = sqrt(sr / n);
}

static bool line_add(uint64_t c6, int64_t local_ns) {
  if (s_ln == 0) {
    s_ref_c6 = c6;
    s_ref_local = local_ns;
  }
  const double x = (double)(int64_t)(c6 - s_ref_c6);
  const double y = (double)(local_ns - s_ref_local) - x * C6_NS_PER_TICK;
  if (s_ln >= LINE_MIN &&
      fabs(y - (s_lmy + s_lslope * (x - s_lmx))) > LINE_REJECT_NS) {
    return false;
  }
  s_lx[s_lhead] = x;
  s_ly[s_lhead] = y;
  s_lhead = (s_lhead + 1) % LINE_N;
  if (s_ln < LINE_N) s_ln++;
  line_recompute();
  s_st.line_rms_ns = s_lrms;
  s_st.line_ppm = s_lslope / C6_NS_PER_TICK * 1e6;
  return true;
}

/* ---- messages from the C6 ---- */

static bool proto_ok(const uint8_t *d, size_t len) {
  if (len >= 1 && d[0] == GW_C6_PROTO) return true;
  if (!s_proto_warned) {
    ESP_LOGE(TAG, "the C6 speaks protocol %d, this Gateway %d: no time from it",
             len ? d[0] : -1, GW_C6_PROTO);
    s_proto_warned = true;
  }
  return false;
}

static void on_hello(uint32_t id, const uint8_t *d, size_t len, void *ctx) {
  (void)id;
  (void)ctx;
  if (!proto_ok(d, len) || len != sizeof(gw_c6_hello_t)) return;
  gw_c6_hello_t h;
  memcpy(&h, d, sizeof(h));
  h.name[sizeof(h.name) - 1] = '\0';
  h.version[sizeof(h.version) - 1] = '\0';
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool first = !s_st.hello;
  s_st.hello = true;
  strlcpy(s_st.c6_name, h.name, sizeof(s_st.c6_name));
  strlcpy(s_st.c6_version, h.version, sizeof(s_st.c6_version));
  s_st.c6_features = h.features;
  if (h.pulse_period) s_period = h.pulse_period;
  xSemaphoreGive(s_lock);
  if (first) {
    ESP_LOGI(TAG, "C6 firmware %s %s, clock %" PRIu32 " Hz, pulse period %" PRIu32
                  " ticks, features 0x%02x",
             h.name, h.version, h.clock_hz, h.pulse_period, h.features);
    if ((h.features & (GW_C6_FEAT_PULSE | GW_C6_FEAT_TSF)) != (GW_C6_FEAT_PULSE | GW_C6_FEAT_TSF)) {
      ESP_LOGE(TAG, "the C6 has no %s: no TIME for the speakers",
               (h.features & GW_C6_FEAT_PULSE) ? "TSF" : "time pulse");
    }
  }
}

static void sample_push(int64_t local_ns, int64_t tsf_ns) {
  s_sample.local_ns = local_ns;
  s_sample.tsf_ns = tsf_ns;
  s_sample.have = true;
}

static void on_tick_locked(const gw_c6_tick_t *t, int64_t arrival_ns) {
  edges_update();
  s_st.ticks++;
  const edge_t *e = edge_by_seq(t->seq);
  if (e && (arrival_ns - e->local_ns < -1000000LL || arrival_ns - e->local_ns > LATENCY_MAX_NS)) {
    e = NULL; /* that number belongs to another edge: the numbering is off */
  }
  if (!e) {
    s_st.unpaired++;
    if (++s_unpaired_run >= UNPAIRED_REALIGN) {
      /* The numbering does not match the C6's (the P4 missed its first
       * edges): take the newest edge before this TICK as its edge. Right
       * unless TICKs are a whole period late just now. */
      const edge_t *last = edge_before(arrival_ns);
      if (last) {
        const uint32_t shift = (uint16_t)(t->seq - (uint16_t)last->k);
        for (uint32_t i = 0; i < EDGE_RING; i++) s_edge[i].k += shift;
        s_k += shift;
        pairing_reset();
        s_st.restarts++;
        ESP_LOGW(TAG, "edge numbering realigned to the C6's (seq %u)", t->seq);
      }
      s_unpaired_run = 0;
    }
    return;
  }
  s_unpaired_run = 0;
  s_st.latency_ms = (double)(arrival_ns - e->local_ns) / 1e6;
  if (s_have_pair) {
    const int64_t dc6 = (int64_t)(t->edge_c6_ticks - s_pair_c6);
    const int64_t dcap = (int64_t)(e->cap - s_pair_cap);
    if (dc6 <= 0) return; /* the same edge again */
    const int64_t tol = dc6 / (1000000 / PAIR_TOL_PPM) + PAIR_TOL_TICKS;
    if (llabs(dcap - dc6) > tol) {
      s_st.rejected++;
      if (++s_reject_run >= REJECT_RESET) pairing_reset();
      return;
    }
  }
  s_reject_run = 0;
  s_have_pair = true;
  s_pair_c6 = t->edge_c6_ticks;
  s_pair_cap = e->cap;
  s_st.paired++;
  if (!line_add(t->edge_c6_ticks, e->local_ns)) {
    s_st.rejected++;
    return;
  }
  if ((t->flags & GW_C6_TICK_TSF_VALID) && t->edge_tsf_ns > 0) {
    sample_push(e->local_ns, (int64_t)t->edge_tsf_ns);
  } else {
    s_st.no_tsf++;
  }
}

/* PTP receive records (4.3), from a TICK or a PTP message. Under s_lock. */
static void ptp_records_add(const uint8_t *p, uint8_t n, int64_t arrival_us) {
  for (uint8_t i = 0; i < n; i++) {
    gw_c6_ptp_rec_t r;
    memcpy(&r, p + (size_t)i * sizeof(r), sizeof(r));
    if (r.dir != GW_C6_DIR_RX) continue;
    ptp_rec_t *e = &s_ptp[s_ptp_n % PTP_RING];
    e->type = r.type;
    e->seq = r.seq;
    memcpy(e->port_id, r.src_id, sizeof(e->port_id));
    e->c6_ticks = r.c6_ticks;
    e->arrival_us = arrival_us;
    s_ptp_n++;
    s_st.ptp_records++;
  }
}

static void on_tick(uint32_t id, const uint8_t *d, size_t len, void *ctx) {
  (void)id;
  (void)ctx;
  const int64_t arrival_ns = esp_timer_get_time() * 1000LL;
  if (!proto_ok(d, len) || len < sizeof(gw_c6_tick_t)) return;
  gw_c6_tick_t t;
  memcpy(&t, d, sizeof(t));
  const bool recs_ok = len == sizeof(t) + (size_t)t.n_ptp * sizeof(gw_c6_ptp_rec_t);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  on_tick_locked(&t, arrival_ns);
  if (recs_ok) ptp_records_add(d + sizeof(t), t.n_ptp, arrival_ns / 1000LL);
  xSemaphoreGive(s_lock);
}

static void on_ptp(uint32_t id, const uint8_t *d, size_t len, void *ctx) {
  (void)id;
  (void)ctx;
  const int64_t arrival_us = esp_timer_get_time();
  if (!proto_ok(d, len) || len < sizeof(gw_c6_ptp_t)) return;
  gw_c6_ptp_t p;
  memcpy(&p, d, sizeof(p));
  if (len != sizeof(p) + (size_t)p.n_ptp * sizeof(gw_c6_ptp_rec_t)) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ptp_records_add(d + sizeof(p), p.n_ptp, arrival_us);
  xSemaphoreGive(s_lock);
}

/* ---- public ---- */

bool gw_c6_time_sample(int64_t *local_ns, int64_t *tsf_ns) {
  if (!s_lock) return false;
  bool ok = false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_sample.have) {
    *local_ns = s_sample.local_ns;
    *tsf_ns = s_sample.tsf_ns;
    s_sample.have = false;
    s_st.samples++;
    ok = true;
  }
  xSemaphoreGive(s_lock);
  return ok;
}

static bool c6_to_local_locked(uint64_t c6_ticks, int64_t *local_ns) {
  if (s_ln < LINE_MIN) return false;
  const double x = (double)(int64_t)(c6_ticks - s_ref_c6);
  const double y = s_lmy + s_lslope * (x - s_lmx);
  *local_ns = s_ref_local + (int64_t)llround(x * C6_NS_PER_TICK + y);
  return true;
}

bool gw_c6_time_c6_to_local(uint64_t c6_ticks, int64_t *local_ns) {
  if (!s_lock) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool ok = c6_to_local_locked(c6_ticks, local_ns);
  xSemaphoreGive(s_lock);
  return ok;
}

bool gw_c6_time_ptp_rx(uint8_t type, uint16_t seq, const uint8_t port_id[10],
                       int64_t socket_ns, int64_t *rx_ns) {
  (void)socket_ns;
  if (!s_lock) return false;
  const int64_t now_us = esp_timer_get_time();
  bool ok = false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const uint32_t m = s_ptp_n < PTP_RING ? s_ptp_n : PTP_RING;
  for (uint32_t i = 0; i < m; i++) {
    const ptp_rec_t *e = &s_ptp[(s_ptp_n - 1 - i) % PTP_RING];
    if (now_us - e->arrival_us > PTP_MAX_AGE_US) break; /* older ones too */
    if (e->type == type && e->seq == seq && memcmp(e->port_id, port_id, sizeof(e->port_id)) == 0) {
      ok = c6_to_local_locked(e->c6_ticks, rx_ns);
      break;
    }
  }
  if (ok) {
    s_st.ptp_found++;
  } else {
    s_st.ptp_missed++;
  }
  xSemaphoreGive(s_lock);
  return ok;
}

void gw_c6_time_get_stats(gw_c6_time_stats_t *out) {
  if (!s_lock) {
    memset(out, 0, sizeof(*out));
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_st;
  xSemaphoreGive(s_lock);
}

static void status_task(void *arg) {
  (void)arg;
  int64_t last_req = 0, last_log = 0;
  bool said_waiting = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const int64_t now = esp_timer_get_time();
    gw_c6_time_stats_t st;
    gw_c6_time_get_stats(&st);
    if (!st.hello && now - last_req >= HELLO_RETRY_MS * 1000LL && now > 3000000LL) {
      uint8_t b = GW_C6_PROTO;
      (void)esp_hosted_send_custom_data(GW_C6_MSG_HELLO_REQ, &b, 1);
      last_req = now;
      if (now > 15000000LL && !said_waiting) {
        ESP_LOGW(TAG, "no answer from the C6: it does not run the Gateway's C6 "
                      "firmware, so there is no TIME for the speakers");
        said_waiting = true;
      }
    }
    if (now - last_log >= STATS_LOG_MS * 1000LL) {
      last_log = now;
      ESP_LOGI(TAG, "edges %" PRIu32 " (missed %" PRIu32 "), TICK %" PRIu32 ": paired %" PRIu32
                    ", unpaired %" PRIu32 ", rejected %" PRIu32 ", no TSF %" PRIu32
                    " | line C6->local rms %.0f ns, %+.3f ppm | samples %" PRIu32
                    ", latency %.1f ms, restarts %" PRIu32 " | PTP records %" PRIu32
                    ", Sync times given %" PRIu32 ", missed %" PRIu32,
               st.edges, st.edges_missed, st.ticks, st.paired, st.unpaired, st.rejected,
               st.no_tsf, st.line_rms_ns, st.line_ppm, st.samples, st.latency_ms, st.restarts,
               st.ptp_records, st.ptp_found, st.ptp_missed);
    }
  }
}

#define TRY(x)                                                     \
  do {                                                             \
    esp_err_t e_ = (x);                                            \
    if (e_ != ESP_OK) {                                            \
      ESP_LOGE(TAG, "%s: %s: no time from the C6", #x, esp_err_to_name(e_)); \
      return e_;                                                   \
    }                                                              \
  } while (0)

esp_err_t gw_c6_time_start(void) {
  if (s_lock) return ESP_OK;
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;

  /* 3.1: edge --ETM--> CAPTURE of the GPTimer; the interrupt on the same
   * edge reads it. */
  const gpio_config_t io = {.pin_bit_mask = 1ULL << PIN, .mode = GPIO_MODE_INPUT,
                            .pull_down_en = GPIO_PULLDOWN_ENABLE,
                            .intr_type = GPIO_INTR_POSEDGE};
  TRY(gpio_config(&io));
  const gptimer_config_t tc = {.clk_src = GPTIMER_CLK_SRC_PLL_F80M,
                               .direction = GPTIMER_COUNT_UP,
                               .resolution_hz = LOCAL_HZ};
  TRY(gptimer_new_timer(&tc, &s_timer));
  esp_etm_task_handle_t cap;
  const gptimer_etm_task_config_t tk = {.task_type = GPTIMER_ETM_TASK_CAPTURE};
  TRY(gptimer_new_etm_task(s_timer, &tk, &cap));
  esp_etm_event_handle_t ev;
  const gpio_etm_event_config_t gc = {.edge = GPIO_ETM_EVENT_EDGE_POS};
  TRY(gpio_new_etm_event(&gc, &ev));
  TRY(gpio_etm_event_bind_gpio(ev, PIN));
  esp_etm_channel_handle_t ch;
  const esp_etm_channel_config_t cc = {0};
  TRY(esp_etm_new_channel(&cc, &ch));
  TRY(esp_etm_channel_connect(ch, ev, cap));
  TRY(esp_etm_channel_enable(ch));
  TRY(gptimer_enable(s_timer));
  TRY(gptimer_start(s_timer));
  esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) TRY(err);
  TRY(gpio_isr_handler_add(PIN, on_edge, NULL));

  TRY(esp_hosted_register_custom_callback(GW_C6_MSG_HELLO, on_hello, NULL));
  TRY(esp_hosted_register_custom_callback(GW_C6_MSG_TICK, on_tick, NULL));
  TRY(esp_hosted_register_custom_callback(GW_C6_MSG_PTP, on_ptp, NULL));
  if (xTaskCreate(status_task, "gw_c6_time", 3072, NULL, 2, NULL) != pdPASS) {
    ESP_LOGE(TAG, "status task not created");
  }
  ESP_LOGI(TAG, "time pulse from the C6 on GPIO%d (ETM capture)", PIN);
  return ESP_OK;
}
