#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "audio_diag.h"
#include "ptp_clock.h"
#include "ptp_clock_engine.h"
#include "airplay_log.h"

static const char *TAG = "ptp_clock";

// PTP multicast addresses and ports
#define PTP_MULTICAST_ADDR "224.0.1.129"
#define PTP_EVENT_PORT     319
#define PTP_GENERAL_PORT   320

// PTP message types
#define PTP_MSG_SYNC       0x0
#define PTP_MSG_DELAY_REQ  0x1
#define PTP_MSG_FOLLOW_UP  0x8
#define PTP_MSG_DELAY_RESP 0x9
#define PTP_MSG_ANNOUNCE   0xB

// PTP header size and timestamp offset
#define PTP_HEADER_SIZE      34
#define PTP_TIMESTAMP_OFFSET 34
#define PTP_TIMESTAMP_SIZE   10

// Synchronization parameters
//
// Local lock diagnostics tolerate WiFi timestamp jitter. Buffered startup
// uses the upstream 400 ms mastership gate, independently of this flag.
#define LOCK_THRESHOLD_NS    50000000LL // 50ms - tolerant of WiFi jitter
#define MIN_SAMPLES_FOR_LOCK 4
#define LOCK_STABLE_TIME_MS  250 // 250ms of stable readings to declare lock
#define LOCK_TIMEOUT_MS      5000

/* Realtime readiness: the current GM estimator has a short continuous history
 * (Shairport/NQPTP). The realtime audio path keeps its previous LOCAL anchor
 * while a new estimator is acquiring. */
#define RT_MIN_MASTER_SAMPLES 4U
/* An estimate older than this must not create a new audio mapping. */
#define PTP_SAMPLE_STALE_MS   2000U
/* A stream SETUP keeps a settled estimate of the same sender (see
 * ptp_clock_start_timing): at least this much continuous history. */
#define PTP_KEEP_MIN_MASTERSHIP_MS 10000U

#define PTP_TASK_PRIORITY_LEGACY   6U
#define PTP_TASK_PRIORITY_REALTIME 8U

#define PTP_STEP_FILTER_NS        500000LL  /* 0.5 ms */
#define PTP_STEP_RAW_FILTER_NS   2000000LL /* 2.0 ms */
#define PTP_STEP_LOG_COOLDOWN_NS 1000000000LL

/* One outstanding two-step Sync. A Follow_Up carries the precise origin
 * timestamp of the corresponding Sync, but the local receive timestamp that
 * belongs in the offset calculation is the Sync receive time. */
typedef struct {
  bool awaiting;
  uint16_t seq;
  int64_t local_ns;
  int64_t correction_ns;
  uint64_t source_clock_id;
  uint32_t source_ip;
  int64_t process_lag_ns;
} ptp_pending_sync_t;

// PTP state
static struct {
  bool running;
  TaskHandle_t task_handle;
  int event_socket;
  int general_socket;

  /* Buffered AAC and realtime ALAC use the same NQPTP-derived estimator.
   * Socket admission / pairing stays in this file; filtering and source/GM
   * epochs live in the pure engine. */
  bool realtime_mode;
  uint32_t timing_peer_ip; // network byte order; 0 = no selected timer
  ptp_clock_engine_t engine;
  ptp_pending_sync_t pending;

  /* Buffered lock qualification (used by the GM handover rule). */
  bool locked;
  uint32_t lock_candidate_start_ms;
  uint32_t last_sync_ms;
  int64_t last_step_log_ns;

  /* Realtime: Announce GM changes after the first GM (audio epoch). */
  uint32_t gm_changes;

  // Media-anchor clock hint for diagnostics (0 = unspecified).
  uint64_t expected_clock_id;

  /* Diagnostic history within the selected RTSP peer's timing record. */
  uint64_t first_source_clock_id;
  bool source_mixed;
} ptp = {0};

/* PTP state is written by the PTP task on Core 0 and read/reset from RTSP/audio
 * control paths that can run concurrently with Core 1 playout. ESP32-S3 is a
 * 32-bit CPU, so 64-bit offset/clock-id fields must not be read or written
 * lock-free across cores. Keep this lock extremely short: no socket I/O, task
 * delays or logging is performed while it is held. */
static portMUX_TYPE ptp_state_mux = portMUX_INITIALIZER_UNLOCKED;

/* Serialize init/stop/priority changes; the task alone closes live sockets. */
static StaticSemaphore_t ptp_lifecycle_storage;
static SemaphoreHandle_t ptp_lifecycle_mutex;
static SemaphoreHandle_t ptp_lifecycle_lock(void) {
  taskENTER_CRITICAL(&ptp_state_mux);
  if (!ptp_lifecycle_mutex)
    ptp_lifecycle_mutex = xSemaphoreCreateMutexStatic(&ptp_lifecycle_storage);
  SemaphoreHandle_t mutex = ptp_lifecycle_mutex;
  taskEXIT_CRITICAL(&ptp_state_mux);
  xSemaphoreTake(mutex, portMAX_DELAY);
  return mutex;
}

static bool ptp_is_running(void) {
  return __atomic_load_n(&ptp.running, __ATOMIC_ACQUIRE);
}

// Parse 8-byte clockIdentity (big-endian) from PTP sourcePortIdentity
// (header bytes 20-27).
static uint64_t parse_ptp_clock_id(const uint8_t *data) {
  uint64_t id = 0;
  for (int i = 0; i < 8; i++) {
    id = (id << 8) | data[20 + i];
  }
  return id;
}

// PTPv2 Announce grandmasterIdentity is bytes 53..60.
static uint64_t parse_announce_grandmaster_id(const uint8_t *data, size_t len) {
  if (!data || len < 61U) return 0;
  uint64_t id = 0;
  for (int i = 0; i < 8; ++i) id = (id << 8) | data[53 + i];
  return id;
}

// Parse 48-bit seconds + 32-bit nanoseconds from PTP timestamp
static uint64_t parse_ptp_timestamp_ns(const uint8_t *data) {
  // Seconds: 6 bytes big-endian
  uint64_t seconds = 0;
  for (int i = 0; i < 6; i++) {
    seconds = (seconds << 8) | data[i];
  }

  // Nanoseconds: 4 bytes big-endian
  uint32_t nanos = ((uint32_t)data[6] << 24) | ((uint32_t)data[7] << 16) |
                   ((uint32_t)data[8] << 8) | (uint32_t)data[9];

  return seconds * 1000000000ULL + nanos;
}

// Get local time in nanoseconds (from esp_timer)
static inline int64_t get_local_time_ns(void) {
  return (int64_t)esp_timer_get_time() * 1000LL;
}

/* PTP correctionField is a signed 64-bit fixed-point value with 16 fractional
 * bits. Return whole nanoseconds. */
static int64_t parse_ptp_correction_ns(const uint8_t *data, size_t len) {
  if (!data || len < 16U) return 0;

  uint64_t raw = 0;
  for (size_t i = 8U; i < 16U; ++i) raw = (raw << 8) | data[i];
  return ((int64_t)raw) / 65536LL;
}

// Since we are a passive PTP listener (no DELAY_REQ/DELAY_RESP), every
// measured offset contains a one-way network delay bias:
//   offset_measured = true_offset - delay.
// LARGER offsets come from SHORTER delays and are MORE accurate; the engine
// converges to the minimum-delay offset (nqptp behaviour).
typedef struct {
  bool emit;
  const char *reason;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t epoch;
  int64_t raw_offset_ns;
  int64_t filtered_offset_ns;
  int64_t raw_filter_delta_ns;
  int64_t filtered_step_ns;
  int64_t rx_lag_ns;
  uint16_t seq;
} ptp_step_log_t;

static int64_t abs_i64(int64_t v) {
  if (v >= 0) return v;
  if (v == INT64_MIN) return INT64_MAX;
  return -v;
}

static void log_ptp_step(const ptp_step_log_t *ev) {
  if (!ev || !ev->emit || !ev->reason) return;
  AP_LOGW(TAG,
           "PTP | STEP | reason=%s | src=%016llx | gm=%016llx | epoch=%lu | "
           "raw=%+.3fms | filt=%+.3fms | ptpD=%+.3fms | dFilt=%+.3fms | "
           "rxLag=%.3fms | seq=%u",
           ev->reason,
           (unsigned long long)ev->source_clock_id,
           (unsigned long long)ev->grandmaster_clock_id,
           (unsigned long)ev->epoch,
           (double)ev->raw_offset_ns / 1000000.0,
           (double)ev->filtered_offset_ns / 1000000.0,
           (double)ev->raw_filter_delta_ns / 1000000.0,
           (double)ev->filtered_step_ns / 1000000.0,
           (double)ev->rx_lag_ns / 1000000.0,
           (unsigned)ev->seq);
}

static void fill_step_log_locked(ptp_step_log_t *ev, const char *reason,
                                 const ptp_clock_engine_sample_t *r,
                                 int64_t rx_lag_ns, uint16_t seq) {
  ev->emit = true;
  ev->reason = reason;
  ev->source_clock_id = ptp.engine.source_clock_id;
  ev->grandmaster_clock_id = ptp.engine.grandmaster_clock_id;
  ev->epoch = ptp.engine.epoch;
  ev->raw_offset_ns = r->raw_offset_ns;
  ev->filtered_offset_ns = r->output_offset_ns;
  const bool crawl = r->crawl_started || r->crawl_ended;
  ev->raw_filter_delta_ns = crawl ? r->crawl_move_ns : r->raw_filter_delta_ns;
  ev->filtered_step_ns = crawl ? r->crawl_move_ns : r->filtered_step_ns;
  ev->rx_lag_ns = rx_lag_ns;
  ev->seq = seq;
}

/* Buffered lock qualification. Caller holds ptp_state_mux. */
static void update_lock_locked(const ptp_clock_engine_sample_t *r,
                               int64_t reception_ns) {
  if (r->restarted) {
    /* A new mastership: lock must be earned again. */
    ptp.locked = false;
    ptp.lock_candidate_start_ms = 0;
  }
  if (r->accepted_samples < MIN_SAMPLES_FOR_LOCK) return;
  const int64_t dev = abs_i64(r->raw_filter_delta_ns);
  if (dev < LOCK_THRESHOLD_NS) {
    if (!ptp.locked) {
      const uint32_t now_ms = (uint32_t)(reception_ns / 1000000LL);
      if (ptp.lock_candidate_start_ms == 0)
        ptp.lock_candidate_start_ms = now_ms;
      if ((now_ms - ptp.lock_candidate_start_ms) >= LOCK_STABLE_TIME_MS) {
        ptp.locked = true;
        ptp.lock_candidate_start_ms = 0;
      }
    }
  } else {
    ptp.lock_candidate_start_ms = 0;
    if (ptp.locked && dev > LOCK_THRESHOLD_NS * 4) ptp.locked = false;
  }
}

/* Feed one paired Sync/Follow_Up offset. Caller holds ptp_state_mux. */
static void add_sample_locked(int64_t raw_offset_ns, int64_t reception_ns,
                              int64_t rx_lag_ns, uint16_t seq,
                              ptp_step_log_t *step_ev) {
  const ptp_clock_engine_sample_t r = ptp_clock_engine_add_sample(
      &ptp.engine, raw_offset_ns, reception_ns);
  if (!r.accepted) return;
  ptp.last_sync_ms = (uint32_t)(reception_ns / 1000000LL);
  if (!ptp.realtime_mode) update_lock_locked(&r, reception_ns);

  if (r.crawl_started) {
    /* dFilt = planned move toward the new level. */
    fill_step_log_locked(step_ev, "clock-step-crawl", &r, rx_lag_ns, seq);
    ptp.last_step_log_ns = reception_ns;
  } else if (r.crawl_ended) {
    /* dFilt = total move. Aborted: the samples came back above the filter,
     * the run was delayed Syncs, not a step. */
    fill_step_log_locked(step_ev, r.crawl_aborted ? "crawl-aborted" : "crawl-done",
                         &r, rx_lag_ns, seq);
    ptp.last_step_log_ns = reception_ns;
  } else if (r.restarted) {
    fill_step_log_locked(step_ev, "gap-restart", &r, rx_lag_ns, seq);
    ptp.last_step_log_ns = reception_ns;
  } else if (r.accepted_samples >= MIN_SAMPLES_FOR_LOCK &&
             abs_i64(r.filtered_step_ns) >= PTP_STEP_FILTER_NS &&
             (ptp.last_step_log_ns == 0 ||
              reception_ns - ptp.last_step_log_ns >= PTP_STEP_LOG_COOLDOWN_NS)) {
    /* Permanent event log: only meaningful estimator motion, at most once
     * per second. Ordinary PTP counters remain in compile-time audio_diag. */
    fill_step_log_locked(step_ev,
                         abs_i64(r.raw_filter_delta_ns) >= PTP_STEP_RAW_FILTER_NS
                             ? "raw-filter-gap" : "offset",
                         &r, rx_lag_ns, seq);
    ptp.last_step_log_ns = reception_ns;
  }
}

/* A domain change starts a new estimator. Caller holds ptp_state_mux. */
static void domain_changed_locked(const char *reason,
                                  const ptp_clock_engine_domain_event_t *de,
                                  int64_t reception_ns, bool preserve_holdover,
                                  ptp_step_log_t *step_ev) {
  if (!de || (!de->source_changed && !de->gm_changed)) return;
  if (step_ev) {
    memset(step_ev, 0, sizeof(*step_ev));
    step_ev->emit = true;
    step_ev->reason = reason;
    step_ev->source_clock_id = de->new_source_clock_id;
    step_ev->grandmaster_clock_id = de->new_grandmaster_clock_id;
    step_ev->epoch = de->epoch;
    /* Log the old active clock estimate instead of a misleading zero. */
    step_ev->raw_offset_ns = de->old_raw_offset_ns;
    step_ev->filtered_offset_ns = de->old_filtered_offset_ns;
    step_ev->raw_filter_delta_ns =
        step_ev->raw_offset_ns - step_ev->filtered_offset_ns;
  }
  ptp.last_step_log_ns = reception_ns;

  /* Never pair a pre-handover Sync with a post-handover Follow_Up. */
  memset(&ptp.pending, 0, sizeof(ptp.pending));

  if (!preserve_holdover) {
    /* A true timing-domain break: old clock state must not drive RTP
     * presentation for the new domain. A same-source buffered GM handover
     * keeps the lock as short holdover until the new GM qualifies. */
    ptp.locked = false;
    ptp.lock_candidate_start_ms = 0;
    ptp.last_sync_ms = 0;
  }
}

/* NQPTP development monitors only the first IP in T <ip>. SETPEERS
 * membership and sourcePortIdentity are not timing-source selectors. */
static bool source_admitted_locked(uint64_t source_clock_id,
                                   uint32_t source_ip) {
  if (source_clock_id != 0 && ptp.timing_peer_ip != 0 &&
      source_ip == ptp.timing_peer_ip)
    return true;
  return false;
}

static void note_source_locked(uint64_t source_clock_id) {
  if (ptp.first_source_clock_id == 0) {
    ptp.first_source_clock_id = source_clock_id;
  } else if (ptp.first_source_clock_id != source_clock_id) {
    ptp.source_mixed = true;
  }
}

static void process_announce(const uint8_t *data, size_t len,
                             uint32_t source_ip, int64_t reception_ns) {
  const uint64_t source_clock = parse_ptp_clock_id(data);
  const uint64_t gm = parse_announce_grandmaster_id(data, len);
  if (gm == 0) return;

  ptp_step_log_t step_ev = {0};
  taskENTER_CRITICAL(&ptp_state_mux);
  if (source_admitted_locked(source_clock, source_ip)) {
    note_source_locked(source_clock);
    /* Realtime timing follows the GM only: a source identity change on the
     * selected peer keeps the estimator (the peer may forward another GM). */
    const uint64_t domain_source =
        ptp.realtime_mode && ptp.engine.source_clock_id != 0
            ? ptp.engine.source_clock_id : source_clock;
    ptp_clock_engine_domain_event_t de = {0};
    if (ptp_clock_engine_set_domain(&ptp.engine, domain_source, gm, &de) &&
        (de.gm_changed || de.source_changed)) {
      if (ptp.realtime_mode) {
        ptp.gm_changes++;
        domain_changed_locked("gm-change", &de, reception_ns, false, &step_ev);
      } else {
        const bool gm_holdover = de.gm_changed && !de.source_changed &&
                                 de.old_estimator_valid && ptp.locked &&
                                 ptp.last_sync_ms != 0;
        domain_changed_locked(gm_holdover ? "gm-handover"
                                          : (de.gm_changed ? "gm-change"
                                                           : "source-change"),
                              &de, reception_ns, gm_holdover, &step_ev);
      }
    }
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_ptp_step(&step_ev);
}

static void process_sync(const uint8_t *data, size_t len, uint16_t seq,
                         uint32_t source_ip, int64_t reception_ns) {
  if (reception_ns <= 0) return;
  const int64_t processing_lag_ns = get_local_time_ns() - reception_ns;
  const uint64_t source_clock = parse_ptp_clock_id(data);
  const int64_t sync_correction_ns = parse_ptp_correction_ns(data, len);
  ptp_step_log_t step_ev = {0};

  taskENTER_CRITICAL(&ptp_state_mux);
  if (!source_admitted_locked(source_clock, source_ip)) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  note_source_locked(source_clock);
  /* Before any Announce the GM is unknown: timing starts anyway (the
   * mastership clock runs) and the Announce only names the GM later.
   * Buffered: a new source identity is a new domain. Realtime: the first
   * source is kept (see process_announce). */
  if (ptp.engine.source_clock_id == 0 ||
      (!ptp.realtime_mode && ptp.engine.source_clock_id != source_clock)) {
    ptp_clock_engine_domain_event_t de = {0};
    if (ptp_clock_engine_set_domain(&ptp.engine, source_clock, 0, &de))
      domain_changed_locked("source-change", &de, reception_ns, false,
                            &step_ev);
  }

  /* A new Sync before the previous pair completed: the old pair can no
   * longer be used safely. Replace it with the newest Sync. */
  ptp.pending.awaiting = true;
  ptp.pending.seq = seq;
  ptp.pending.local_ns = reception_ns;
  ptp.pending.correction_ns = sync_correction_ns;
  ptp.pending.source_clock_id = source_clock;
  ptp.pending.source_ip = source_ip;
  ptp.pending.process_lag_ns = processing_lag_ns > 0 ? processing_lag_ns : 0;

  const uint16_t flags = ((uint16_t)data[6] << 8) | data[7];
  const bool two_step = (flags & 0x0200) != 0;
  if (!two_step && len >= PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) {
    const uint64_t ptp_time_ns =
        parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
    if (ptp_time_ns <= INT64_MAX) {
      const int64_t raw_offset = (int64_t)ptp_time_ns + sync_correction_ns -
                                 reception_ns;
      add_sample_locked(raw_offset, reception_ns, ptp.pending.process_lag_ns,
                        seq, &step_ev);
    }
    ptp.pending.awaiting = false;
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_ptp_step(&step_ev);
}

static void process_followup(const uint8_t *data, size_t len, uint16_t seq,
                             uint32_t source_ip, int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) return;
  const uint64_t followup_source = parse_ptp_clock_id(data);
  const int64_t followup_correction_ns = parse_ptp_correction_ns(data, len);
  const uint64_t ptp_time_ns =
      parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
  if (ptp_time_ns > INT64_MAX) return;

  ptp_step_log_t step_ev = {0};
  taskENTER_CRITICAL(&ptp_state_mux);
  if (!source_admitted_locked(followup_source, source_ip)) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  if (!ptp.pending.awaiting) {
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }
  if (seq != ptp.pending.seq || source_ip != ptp.pending.source_ip ||
      followup_source != ptp.pending.source_clock_id) {
    /* Keep the pending Sync: an out-of-order/old Follow_Up must not destroy
     * the chance to receive the correct Follow_Up for it. */
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }

  const int64_t sync_local_ns = ptp.pending.local_ns;
  const int64_t raw_offset = (int64_t)ptp_time_ns + ptp.pending.correction_ns +
                             followup_correction_ns - sync_local_ns;
  ptp.pending.awaiting = false;
  /* The engine works in Sync reception time: that is when the measured
   * offset was true. */
  add_sample_locked(raw_offset, sync_local_ns, ptp.pending.process_lag_ns, seq,
                    &step_ev);
  taskEXIT_CRITICAL(&ptp_state_mux);
  log_ptp_step(&step_ev);
}

static void process_ptp_message(const uint8_t *data, size_t len,
                                bool is_event_port, uint32_t source_ip,
                                int64_t reception_ns) {
  if (len < PTP_HEADER_SIZE) return;
  const uint8_t msg_type = data[0] & 0x0F;
  const uint16_t seq = ((uint16_t)data[30] << 8) | data[31];

  switch (msg_type) {
  case PTP_MSG_SYNC:
    if (is_event_port) process_sync(data, len, seq, source_ip, reception_ns);
    break;
  case PTP_MSG_FOLLOW_UP:
    if (!is_event_port)
      process_followup(data, len, seq, source_ip, reception_ns);
    break;
  case PTP_MSG_ANNOUNCE:
    if (!is_event_port)
      process_announce(data, len, source_ip, reception_ns);
    break;
  default:
    break;
  }
}


// Create and bind multicast socket
static int create_ptp_socket(uint16_t port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return -1;
  }

  // Allow address reuse
  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // Bind to port
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind to port %d: %d", port, errno);
    close(sock);
    return -1;
  }

  // Join multicast group
  struct ip_mreq mreq = {0};
  mreq.imr_multiaddr.s_addr = inet_addr(PTP_MULTICAST_ADDR);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);

  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    ESP_LOGE(TAG, "Failed to join multicast group: %d", errno);
    close(sock);
    return -1;
  }

  // Set receive timeout
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  return sock;
}

// PTP task - listens for messages on both ports
static void ptp_task(void *pvParameters) {
  (void)pvParameters;
  /* Creation may schedule us before init has published the handle. Wait for
   * publication before any exit path can acknowledge completion. */
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  uint8_t buffer[256];

  /* These descriptors belong to this task until its cleanup. Stop requests
   * never close them, so select/recv cannot observe a reused descriptor. */
  const int event_socket = ptp.event_socket;
  const int general_socket = ptp.general_socket;
  while (ptp_is_running()) {
    fd_set read_fds;
    FD_ZERO(&read_fds);

    int max_fd = -1;
    if (event_socket >= 0) {
      FD_SET(event_socket, &read_fds);
      if (event_socket > max_fd) {
        max_fd = event_socket;
      }
    }
    if (general_socket >= 0) {
      FD_SET(general_socket, &read_fds);
      if (general_socket > max_fd) {
        max_fd = general_socket;
      }
    }

    if (max_fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

    if (!ptp_is_running()) break;

    if (ret < 0) {
      if (errno != EINTR) {
        ESP_LOGE(TAG, "select error: %d", errno);
      }
      continue;
    }

    if (ret == 0) {
      // Timeout - check if we lost lock due to no messages
    } else {
      /* Match nqptp's userspace timestamp point: sample the local clock once
       * immediately after select() reports readable PTP sockets, then use that
       * reception timestamp for this ready batch. */
      const int64_t reception_ns = get_local_time_ns();

      // Check event port (SYNC messages)
      if (event_socket >= 0 && FD_ISSET(event_socket, &read_fds)) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        ssize_t len = recvfrom(event_socket, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)&src, &src_len);
        if (len > 0) {
          process_ptp_message(buffer, (size_t)len, true,
                              src.sin_addr.s_addr, reception_ns);
        }
      }

      // Check general port (FOLLOW_UP messages)
      if (general_socket >= 0 && FD_ISSET(general_socket, &read_fds)) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        ssize_t len = recvfrom(general_socket, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)&src, &src_len);
        if (len > 0) {
          process_ptp_message(buffer, (size_t)len, false,
                              src.sin_addr.s_addr, reception_ns);
        }
      }
    }
  }

  // Cleanup
  if (ptp.event_socket >= 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
  }
  if (ptp.general_socket >= 0) {
    close(ptp.general_socket);
    ptp.general_socket = -1;
  }

  __atomic_store_n(&ptp.task_handle, NULL, __ATOMIC_RELEASE);
  vTaskDelete(NULL);
}

esp_err_t ptp_clock_init(void) {
  SemaphoreHandle_t lifecycle = ptp_lifecycle_lock();
  if (ptp_is_running() ||
      __atomic_load_n(&ptp.task_handle, __ATOMIC_ACQUIRE) != NULL) {
    xSemaphoreGive(lifecycle);
    return ESP_ERR_INVALID_STATE;
  }

  taskENTER_CRITICAL(&ptp_state_mux);
  memset(&ptp, 0, sizeof(ptp));
  ptp.event_socket = -1;
  ptp.general_socket = -1;
  ptp_clock_engine_init(&ptp.engine);
  taskEXIT_CRITICAL(&ptp_state_mux);

  ptp.event_socket = create_ptp_socket(PTP_EVENT_PORT);
  if (ptp.event_socket < 0) goto failed;
  ptp.general_socket = create_ptp_socket(PTP_GENERAL_PORT);
  if (ptp.general_socket < 0) goto failed;

  __atomic_store_n(&ptp.running, true, __ATOMIC_RELEASE);
  TaskHandle_t task_handle = NULL;
  BaseType_t ret = xTaskCreatePinnedToCore(ptp_task, "ptp_clock", 4096, NULL,
                                           PTP_TASK_PRIORITY_LEGACY,
                                           &task_handle, 0);
  if (ret != pdPASS) {
    __atomic_store_n(&ptp.running, false, __ATOMIC_RELEASE);
    ESP_LOGE(TAG, "Failed to create PTP task");
    goto failed;
  }
  __atomic_store_n(&ptp.task_handle, task_handle, __ATOMIC_RELEASE);
  xTaskNotifyGive(task_handle);
  xSemaphoreGive(lifecycle);
  return ESP_OK;

failed:
  if (ptp.event_socket >= 0) close(ptp.event_socket);
  if (ptp.general_socket >= 0) close(ptp.general_socket);
  ptp.event_socket = ptp.general_socket = -1;
  xSemaphoreGive(lifecycle);
  return ESP_FAIL;
}

/* Estimator reset; caller holds ptp_state_mux. Mode and timing peer stay. */
static void ptp_clear_estimators_locked(void) {
  ptp_clock_engine_init(&ptp.engine);
  memset(&ptp.pending, 0, sizeof(ptp.pending));
  ptp.locked = false;
  ptp.lock_candidate_start_ms = 0;
  ptp.last_sync_ms = 0;
  ptp.last_step_log_ns = 0;
  ptp.expected_clock_id = 0;
  ptp.first_source_clock_id = 0;
  ptp.source_mixed = false;
}

void ptp_clock_clear(void) {
  taskENTER_CRITICAL(&ptp_state_mux);
  ptp_clear_estimators_locked();
  taskEXIT_CRITICAL(&ptp_state_mux);
}

static uint32_t age_ms_since(int64_t now_ns, int64_t then_ns) {
  if (then_ns <= 0 || now_ns < then_ns) return UINT32_MAX;
  const uint64_t age_ms = (uint64_t)(now_ns - then_ns) / 1000000ULL;
  return age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
}

/* Realtime readiness of the current estimator; caller holds ptp_state_mux. */
static bool realtime_ready_locked(uint32_t sample_age_ms,
                                  uint32_t mastership_age_ms) {
  return ptp.engine.valid && ptp.engine.grandmaster_clock_id != 0 &&
         ptp.engine.accepted_samples >= RT_MIN_MASTER_SAMPLES &&
         mastership_age_ms != UINT32_MAX &&
         mastership_age_ms >= PTP_CLOCK_ENGINE_READY_MS &&
         sample_age_ms <= PTP_SAMPLE_STALE_MS;
}

void ptp_clock_get_snapshot(ptp_clock_snapshot_t *snapshot) {
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));

  const int64_t now_ns = get_local_time_ns();
  const uint32_t now_ms = (uint32_t)(now_ns / 1000000LL);
  taskENTER_CRITICAL(&ptp_state_mux);

  /* Snapshot is the buffered timing read boundary, so expire a stale lock
   * here. Audio can then continue from its cached local anchor without treating
   * stale PTP as newly qualified. */
  if (!ptp.realtime_mode && ptp.locked && ptp.last_sync_ms > 0 &&
      (now_ms - ptp.last_sync_ms) > LOCK_TIMEOUT_MS) {
    ptp.locked = false;
    ptp.lock_candidate_start_ms = 0;
  }

  const ptp_clock_engine_t *e = &ptp.engine;
  snapshot->sample_age_ms = age_ms_since(now_ns, e->last_accepted_time_ns);
  snapshot->mastership_age_ms =
      e->valid ? age_ms_since(now_ns, e->mastership_start_time_ns) : 0U;
  if (snapshot->mastership_age_ms == UINT32_MAX) snapshot->mastership_age_ms = 0;
  snapshot->realtime_mode = ptp.realtime_mode;
  snapshot->locked = ptp.realtime_mode
      ? realtime_ready_locked(snapshot->sample_age_ms, snapshot->mastership_age_ms)
      : ptp.locked;
  snapshot->valid = ptp.realtime_mode ? snapshot->locked : e->valid;
  snapshot->source_mixed = ptp.source_mixed;
  snapshot->expected_clock_id = ptp.expected_clock_id;
  snapshot->source_clock_id = e->source_clock_id;
  snapshot->grandmaster_clock_id = e->grandmaster_clock_id;
  snapshot->epoch = ptp.realtime_mode
      ? (e->grandmaster_clock_id ? ptp.gm_changes + 1U : 0U) : e->epoch;
  snapshot->raw_offset_ns = e->raw_offset_ns;
  snapshot->filtered_offset_ns = e->output_offset_ns;
  snapshot->sample_count = e->accepted_samples;
  taskEXIT_CRITICAL(&ptp_state_mux);
  snapshot->raw_filter_delta_ns =
      snapshot->raw_offset_ns - snapshot->filtered_offset_ns;
}

void ptp_clock_start_timing(bool realtime, uint32_t timing_peer_ip) {
  SemaphoreHandle_t lifecycle = ptp_lifecycle_lock();
  const int64_t now_ns = get_local_time_ns();
  taskENTER_CRITICAL(&ptp_state_mux);
  /* NQPTP development handle_control_port_messages(T): clear the timing
   * records, select the RTSP sender, then begin timing (B), even when the IP
   * has not changed. Reference: 0a88cd7ca9325f023ca5d891714695167672bdb9.
   * Exception: the same sender in the same mode, whose samples never stopped
   * and whose estimate is settled. Some apps change tracks as TEARDOWN +
   * SETUP ~60 ms apart; a reset there started every track on ~1 s of
   * samples, 1-2 ms off, and the servo needed ~30 s to pull it back (seen on
   * the board). A new sender, a mode change, a gap or a young estimate still
   * start from scratch; GM changes and clock steps are handled by the
   * engine itself. */
  const ptp_clock_engine_t *e = &ptp.engine;
  const uint32_t sample_age_ms = age_ms_since(now_ns, e->last_accepted_time_ns);
  const uint32_t mastership_ms = age_ms_since(now_ns, e->mastership_start_time_ns);
  const bool keep = e->valid && timing_peer_ip != 0 &&
                    ptp.timing_peer_ip == timing_peer_ip &&
                    ptp.realtime_mode == realtime &&
                    sample_age_ms <= PTP_SAMPLE_STALE_MS &&
                    mastership_ms != UINT32_MAX &&
                    mastership_ms >= PTP_KEEP_MIN_MASTERSHIP_MS;
  const uint32_t kept_samples = e->accepted_samples;
  ptp.realtime_mode = realtime;
  ptp.timing_peer_ip = timing_peer_ip;
  if (!keep) ptp_clear_estimators_locked();
  TaskHandle_t task_handle =
      __atomic_load_n(&ptp.task_handle, __ATOMIC_ACQUIRE);
  taskEXIT_CRITICAL(&ptp_state_mux);
  if (keep) {
    AP_LOGI(TAG, "PTP: same sender, settled estimate kept at stream SETUP "
                 "(%lu s, %lu samples)",
            (unsigned long)(mastership_ms / 1000U), (unsigned long)kept_samples);
  }
  if (task_handle && ptp_is_running()) {
    vTaskPrioritySet(task_handle, realtime ? PTP_TASK_PRIORITY_REALTIME
                                         : PTP_TASK_PRIORITY_LEGACY);
  }
  xSemaphoreGive(lifecycle);
}

void ptp_clock_note_realtime_d7(uint64_t clock_id) {
  if (clock_id == 0) return;
  taskENTER_CRITICAL(&ptp_state_mux);
  if (ptp.realtime_mode) {
    ptp.expected_clock_id = clock_id; /* anchor hint only; no realtime filtering */
  }
  taskEXIT_CRITICAL(&ptp_state_mux);
}

void ptp_clock_get_realtime_snapshot(ptp_realtime_snapshot_t *snapshot) {
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));
  const int64_t now_ns = get_local_time_ns();
  taskENTER_CRITICAL(&ptp_state_mux);
  const ptp_clock_engine_t *e = &ptp.engine;
  snapshot->realtime_mode = ptp.realtime_mode;
  snapshot->master_clock_id = e->grandmaster_clock_id;
  snapshot->source_clock_id = e->source_clock_id;
  snapshot->master_offset_ns = e->output_offset_ns;
  snapshot->sample_count = e->accepted_samples;
  snapshot->gm_change_count = ptp.gm_changes;
  snapshot->sample_age_ms = age_ms_since(now_ns, e->last_accepted_time_ns);
  const uint32_t mastership_age_ms =
      e->valid ? age_ms_since(now_ns, e->mastership_start_time_ns) : UINT32_MAX;
  snapshot->mastership_age_ms =
      mastership_age_ms == UINT32_MAX ? 0U : mastership_age_ms;
  snapshot->master_ready =
      ptp.realtime_mode &&
      realtime_ready_locked(snapshot->sample_age_ms, mastership_age_ms);
  taskEXIT_CRITICAL(&ptp_state_mux);
}

bool ptp_clock_realtime_snapshot_to_local(
    const ptp_realtime_snapshot_t *snapshot, uint64_t clock_id,
    uint64_t remote_ns, uint64_t *local_ns) {
  if (!snapshot || !local_ns || !snapshot->realtime_mode ||
      !snapshot->master_ready || clock_id == 0 ||
      clock_id != snapshot->master_clock_id || remote_ns > INT64_MAX)
    return false;
  uint64_t local = 0;
  if (!ptp_clock_engine_remote_to_local(remote_ns, snapshot->master_offset_ns,
                                        &local) ||
      local > INT64_MAX)
    return false;
  *local_ns = local;
  return true;
}

void ptp_clock_set_external(const ptp_clock_external_t *ext) {
  taskENTER_CRITICAL(&ptp_state_mux);
  if (!ext) {
    ptp_clear_estimators_locked();
    taskEXIT_CRITICAL(&ptp_state_mux);
    return;
  }
  ptp_clock_engine_t *e = &ptp.engine;
  if (ptp.realtime_mode != ext->realtime) {
    /* Like ptp_clock_start_timing() switching modes: a fresh record. */
    ptp_clear_estimators_locked();
    ptp.gm_changes = 0;
  }
  if (!e->valid || e->source_clock_id != ext->source_clock_id ||
      e->grandmaster_clock_id != ext->grandmaster_clock_id) {
    /* A new timing domain, as a GM change would start on a PTP receiver. */
    if (ext->realtime && e->grandmaster_clock_id != 0 &&
        e->grandmaster_clock_id != ext->grandmaster_clock_id)
      ptp.gm_changes++;
    e->epoch = e->epoch + 1U ? e->epoch + 1U : 1U;
    e->source_clock_id = ext->source_clock_id;
    e->grandmaster_clock_id = ext->grandmaster_clock_id;
  }
  e->valid = true;
  e->raw_offset_ns = ext->offset_ns;
  e->filtered_offset_ns = ext->offset_ns;
  e->output_offset_ns = ext->offset_ns;
  e->last_accepted_time_ns = ext->sample_time_ns;
  e->mastership_start_time_ns =
      ext->sample_time_ns - (int64_t)ext->mastership_age_ms * 1000000LL;
  e->accepted_samples = ext->sample_count;
  ptp.realtime_mode = ext->realtime;
  ptp.locked = ext->locked;
  ptp.lock_candidate_start_ms = 0;
  ptp.last_sync_ms = (uint32_t)(ext->sample_time_ns / 1000000LL);
  taskEXIT_CRITICAL(&ptp_state_mux);
}

void ptp_clock_set_master_clock_id(uint64_t clock_id) {
  /* Shairport set_ptp_anchor_info only stores the media anchor. It never
   * resets NQPTP or selects packets by the anchor's grandmaster identity.
   * Keep the hint for diagnostics; T <IP> owns timing-source admission. */
  taskENTER_CRITICAL(&ptp_state_mux);
  ptp.expected_clock_id = clock_id;
  taskEXIT_CRITICAL(&ptp_state_mux);
}
