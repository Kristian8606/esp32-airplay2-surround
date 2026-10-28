#include "realtime_receiver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "alac_decoder.h"
#include "audio_format_log.h"
#include "audio_crypto.h"
#include "audio_diag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network/socket_utils.h"
#include "network/ptp_clock.h"
#include "airplay_log.h"

#define RT_PACKET_MAX             8192U
#define RT_PCM_CAPACITY_FRAMES    1024U

/* Realtime ingress is intentionally split. The DATA task does no crypto or
 * decode work; it only drains lwIP into a preallocated packet pool. CONTROL
 * receives D7/PT84 and forwards RTX into the same worker queue. ALAC_WORK is
 * the sole owner of decrypt + ALAC decoder state. RESEND owns missing state. */
#define RT_DATA_RX_STACK          4096U
#define RT_CTRL_RX_STACK          4096U
#define RT_WORK_STACK             6144U
#define RT_RESEND_STACK           4096U
#define RT_DATA_RX_PRIORITY       7
#define RT_CTRL_RX_PRIORITY       6
#define RT_WORK_PRIORITY          6
#define RT_RESEND_PRIORITY        6
#define RT_TASK_CORE              0

#define RT_DATA_POOL_SLOTS        64U
#define RT_RTX_POOL_SLOTS         32U
#define RT_WORK_QUEUE_SLOTS       96U
#define RT_RESEND_EVENT_SLOTS     512U
#define RT_SEEN_SLOTS             1024U
#define RT_MISSING_SLOTS          512U

#define RT_FIRST_PACKET_LOGS      5U
#define RT_SYNC_PT                84U
#define RT_AP2_ANCHOR_PT          87U
#define RT_RETRANSMIT_PT          86U
#define RT_RESEND_REQUEST_PT      85U
#define RT_MAX_NACK_COUNT         32U
/* Match Shairport Sync defaults: wait 100 ms before the first request, retry
 * at 250 ms intervals, and stop issuing NEW requests 100 ms before output.
 * Do not declare final loss there: an already-requested RTX may still arrive.
 * Final loss is the ordered-staging commit boundary below. */
#define RT_RESEND_FIRST_MS        100U
#define RT_RESEND_RETRY_MS        250U
#define RT_RESEND_LAST_REQUEST_MS 100U
#define RT_FINAL_LOSS_MARGIN_MS   ((uint32_t)(REALTIME_RECOVERY_FINAL_MARGIN_US / 1000LL))
#define RT_RESEND_SCAN_MS         10U
#define RT_SOCKET_TIMEOUT_MS      100U

static const char *TAG = "airplay_rt";

/* Reserved once during audio init, before RTSP sessions fragment internal RAM.
 * Keep these allocations across codec changes and Wi-Fi scan release/reinit. */
static StackType_t *s_data_task_stack;
static StaticTask_t *s_data_task_tcb;
static TaskHandle_t s_data_task;

esp_err_t realtime_receiver_reserve_data_task_memory(void) {
  if (s_data_task_stack && s_data_task_tcb) return ESP_OK;
  StackType_t *stack = heap_caps_malloc(
      RT_DATA_RX_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  StaticTask_t *tcb = heap_caps_malloc(
      sizeof(*tcb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!stack || !tcb) {
    heap_caps_free(stack);
    heap_caps_free(tcb);
    ESP_LOGE(TAG, "alac_data memory reservation failed: stack=%u TCB=%u bytes",
             (unsigned)RT_DATA_RX_STACK, (unsigned)sizeof(*tcb));
    return ESP_ERR_NO_MEM;
  }
  s_data_task_stack = stack;
  s_data_task_tcb = tcb;
  ESP_LOGI(TAG, "alac_data memory reserved: internal stack=%u TCB=%u bytes",
           (unsigned)RT_DATA_RX_STACK, (unsigned)sizeof(*tcb));
  return ESP_OK;
}

typedef enum {
  RT_POOL_DATA = 0,
  RT_POOL_RTX = 1,
} rt_pool_kind_t;

typedef struct {
  uint16_t len;
  uint8_t pool_kind;
  bool retransmitted;
  uint8_t data[RT_PACKET_MAX];
} rt_packet_slot_t;

typedef struct {
  bool valid;
  uint32_t ext_seq;
} seen_slot_t;

typedef struct {
  bool active;
  uint32_t ext_seq;
  uint32_t missing_rtp;
  TickType_t missing_since;
  TickType_t last_nack;
  uint8_t nack_count;
} missing_slot_t;

typedef enum {
  RT_RESEND_EVENT_MISSING = 1,
  RT_RESEND_EVENT_RECEIVED = 2,
  RT_RESEND_EVENT_WAKE = 3,
} rt_resend_event_kind_t;

typedef struct {
  uint8_t kind;
  uint16_t count;
  uint32_t ext_seq;
  uint32_t rtp;
} rt_resend_event_t;

typedef struct {
  realtime_receiver_config_t cfg;
  int data_sock;
  int control_sock;
  volatile bool running;
  volatile uint32_t live_tasks;

  QueueHandle_t data_free_q;
  QueueHandle_t rtx_free_q;
  QueueHandle_t work_q;
  QueueHandle_t resend_event_q;
  /* Event payloads are task-only; keep queue control in internal RAM.
   * Both allocations survive stop/start, just like the other RT queues. */
  uint8_t *resend_event_storage;
  StaticQueue_t *resend_event_control;

  rt_packet_slot_t *data_pool;
  rt_packet_slot_t *rtx_pool;
  bool packet_workspace_external;
  uint8_t *control_packet;
  uint8_t *decrypt_buf;
  int16_t *pcm;
  seen_slot_t *seen;
  missing_slot_t *missing;
  uint16_t active_missing_count;

  uint32_t newest_ext_seq;
  bool newest_ext_valid;

  uint32_t decrypt_errors;
  uint32_t decode_errors;
  uint32_t dropped_data;
  uint32_t dropped_rtx;
  uint32_t missing_backpressure;
  uint32_t received_backpressure;
  struct sockaddr_in client_control_addr;
  bool client_control_valid;
  uint16_t nack_request_seq;
  /* AirPlay 2 realtime D7/PT=87 observation. Every validated D7 from the
   * ready current GM refreshes the running RTP<->ESP-local presentation map.
   * A refresh updates timing only; it never resets the RTP cursor/rings. */
  uint32_t d7_packets;
  uint32_t d7_malformed;
  uint32_t last_d7_frame1;
  uint64_t last_d7_raw_ptp_ns;
  uint64_t last_d7_clock_id;
  uint32_t d7_anchor_commit_count;
  uint32_t d7_last_committed_packet;
  uint32_t d7_defer_logged_epoch;

  uint64_t unhandled_control_seen[2];
  bool missing_overflow_logged;
} realtime_state_t;

static realtime_state_t s_rt = {
    .data_sock = -1,
    .control_sock = -1,
};

/* Protect the multi-field retransmit destination snapshot. The lock is never
 * held across sendto(), so packet/recovery work cannot block control updates. */
static portMUX_TYPE s_rt_control_mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool rt_running(void) {
  return __atomic_load_n(&s_rt.running, __ATOMIC_ACQUIRE);
}

static inline void rt_set_running(bool running) {
  __atomic_store_n(&s_rt.running, running, __ATOMIC_RELEASE);
}

static inline void rt_task_reserve(void) {
  (void)__atomic_add_fetch(&s_rt.live_tasks, 1U, __ATOMIC_ACQ_REL);
}

static inline void rt_task_release(void) {
  (void)__atomic_sub_fetch(&s_rt.live_tasks, 1U, __ATOMIC_ACQ_REL);
}

static inline bool all_tasks_stopped(void) {
  return __atomic_load_n(&s_rt.live_tasks, __ATOMIC_ACQUIRE) == 0U;
}

static void rt_request_stop(void) {
  rt_set_running(false);
  /* RESEND sleeps indefinitely while there are no holes. A wake event makes
   * every stop path, including worker-init failure, deterministic. */
  if (s_rt.resend_event_q) {
    const rt_resend_event_t wake = {.kind = RT_RESEND_EVENT_WAKE};
    (void)xQueueSendToFront(s_rt.resend_event_q, &wake, 0);
  }
}

static inline uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t read_be64(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static size_t rtp_payload_offset(const uint8_t *packet, size_t len) {
  if (!packet || len < 12U || (packet[0] >> 6) != 2U) {
    return 0;
  }

  const size_t cc = packet[0] & 0x0fU;
  size_t off = 12U + cc * 4U;
  if (off > len) {
    return 0;
  }

  if (packet[0] & 0x10U) { /* RTP extension */
    if (off + 4U > len) {
      return 0;
    }
    const uint16_t ext_words =
        (uint16_t)(((uint16_t)packet[off + 2] << 8) | packet[off + 3]);
    off += 4U + (size_t)ext_words * 4U;
    if (off > len) {
      return 0;
    }
  }
  return off;
}


static bool send_retransmit_request(uint16_t first_missing, uint16_t count) {
  if (count == 0) return false;
  if (count > RT_MAX_NACK_COUNT) count = RT_MAX_NACK_COUNT;

  int control_sock;
  bool target_valid;
  struct sockaddr_in target;
  taskENTER_CRITICAL(&s_rt_control_mux);
  control_sock = s_rt.control_sock;
  target_valid = s_rt.client_control_valid;
  target = s_rt.client_control_addr;
  taskEXIT_CRITICAL(&s_rt_control_mux);
  if (!target_valid || control_sock < 0) return false;

  uint8_t req[8] = {0};
  const uint16_t request_seq = ++s_rt.nack_request_seq;
  req[0] = 0x80;
  req[1] = 0x80U | RT_RESEND_REQUEST_PT;
  req[2] = (uint8_t)(request_seq >> 8);
  req[3] = (uint8_t)request_seq;
  req[4] = (uint8_t)(first_missing >> 8);
  req[5] = (uint8_t)first_missing;
  req[6] = (uint8_t)(count >> 8);
  req[7] = (uint8_t)count;

  return sendto(control_sock, req, sizeof(req), 0,
                (const struct sockaddr *)&target, sizeof(target)) ==
         (ssize_t)sizeof(req);
}

static bool decode_audio_packet(alac_decoder_t *decoder,
                                const uint8_t *packet, size_t packet_len,
                                bool retransmitted) {
  const size_t payload_off = rtp_payload_offset(packet, packet_len);
  if (payload_off == 0 || payload_off >= packet_len) {
    return false;
  }

  const uint8_t pt = packet[1] & 0x7fU;
  if (pt != 96U) {
    return false;
  }

  const uint16_t seq =
      (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
  const uint32_t rtp = ((uint32_t)packet[4] << 24) |
                       ((uint32_t)packet[5] << 16) |
                       ((uint32_t)packet[6] << 8) |
                       (uint32_t)packet[7];

  const uint8_t *payload = packet + payload_off;
  const size_t payload_len = packet_len - payload_off;
  int dec_len = audio_crypto_decrypt_rtp(
      &s_rt.cfg.encrypt, payload, payload_len, s_rt.decrypt_buf,
      RT_PACKET_MAX, packet, packet_len);
  if (dec_len <= 0) {
    s_rt.decrypt_errors++;
    if (s_rt.decrypt_errors <= RT_FIRST_PACKET_LOGS) {
      AP_LOGW(TAG,
               "decrypt failed seq=%u rtp=%" PRIu32 " packet=%u payload=%u%s",
               seq, rtp, (unsigned)packet_len, (unsigned)payload_len,
               retransmitted ? " retransmit" : "");
    }
    return false;
  }

  alac_decode_info_t info = {0};
  int frames = alac_decoder_decode(decoder, s_rt.decrypt_buf,
                                   (size_t)dec_len, s_rt.pcm,
                                   RT_PCM_CAPACITY_FRAMES, &info);
  if (frames <= 0) {
    s_rt.decode_errors++;
    if (s_rt.decode_errors <= RT_FIRST_PACKET_LOGS) {
      AP_LOGW(TAG, "ALAC decode failed seq=%u rtp=%" PRIu32 " bytes=%d%s",
               seq, rtp, dec_len, retransmitted ? " retransmit" : "");
    }
    return false;
  }

  if (!s_rt.cfg.pcm_sink ||
      !s_rt.cfg.pcm_sink(rtp, s_rt.pcm, (size_t)frames, info.channels,
                         s_rt.cfg.pcm_sink_ctx)) {
    return false;
  }
  return true;
}



static uint32_t unwrap_seq16(uint16_t seq, uint32_t reference) {
  int64_t candidate = (int64_t)(reference & 0xffff0000U) | (int64_t)seq;
  const int64_t ref = (int64_t)reference;
  if (candidate - ref > 32767LL) {
    candidate -= 65536LL;
  } else if (ref - candidate > 32768LL) {
    candidate += 65536LL;
  }
  if (candidate < 0) {
    candidate += (1LL << 32);
  }
  return (uint32_t)candidate;
}

static bool seen_has(uint32_t ext_seq) {
  if (!s_rt.seen) return false;
  seen_slot_t *slot = &s_rt.seen[ext_seq % RT_SEEN_SLOTS];
  return slot->valid && slot->ext_seq == ext_seq;
}

static void seen_mark(uint32_t ext_seq) {
  if (!s_rt.seen) return;
  seen_slot_t *slot = &s_rt.seen[ext_seq % RT_SEEN_SLOTS];
  slot->valid = true;
  slot->ext_seq = ext_seq;
}

static void release_packet_slot(rt_packet_slot_t *slot) {
  if (!slot) return;
  slot->len = 0;
  QueueHandle_t q = slot->pool_kind == RT_POOL_RTX ? s_rt.rtx_free_q
                                                   : s_rt.data_free_q;
  if (q) (void)xQueueSend(q, &slot, 0);
}

static bool queue_work_packet(rt_packet_slot_t *slot) {
  if (!slot || !s_rt.work_q) return false;
  const bool queued = xQueueSend(s_rt.work_q, &slot, 0) == pdTRUE;
  if (!queued) {
    uint32_t *counter = slot->pool_kind == RT_POOL_RTX
                            ? &s_rt.dropped_rtx : &s_rt.dropped_data;
    (void)__atomic_add_fetch(counter, 1U, __ATOMIC_RELAXED);
  }
  return queued;
}

static bool queue_resend_event(uint8_t kind, uint32_t ext_seq, uint16_t count,
                               uint32_t rtp) {
  if (!s_rt.resend_event_q || count == 0) return false;
  rt_resend_event_t ev = {
      .kind = kind, .count = count, .ext_seq = ext_seq, .rtp = rtp,
  };
  if (xQueueSend(s_rt.resend_event_q, &ev, 0) == pdTRUE) return true;
  uint32_t *counter = kind == RT_RESEND_EVENT_MISSING
                          ? &s_rt.missing_backpressure : &s_rt.received_backpressure;
  (void)__atomic_add_fetch(counter, 1U, __ATOMIC_RELAXED);
  /* Only WORK produces semantic events. Backpressure this consumer, never
   * UDP DATA/CTRL, instead of silently losing a missing/received transition.
   * One-tick retries remain cancellation-aware; RESEND owns the state and
   * drains bounded batches before servicing deadlines. */
  while (rt_running()) {
    if (xQueueSend(s_rt.resend_event_q, &ev, 1) == pdTRUE) return true;
  }
  return false;
}

static bool missing_time_to_play_us(const missing_slot_t *slot,
                                    int64_t *out_us) {
  if (!slot || !out_us || !s_rt.cfg.deadline_cb) {
    return false;
  }
  return s_rt.cfg.deadline_cb(slot->missing_rtp, out_us,
                              s_rt.cfg.deadline_ctx);
}

static missing_slot_t *missing_slot_for(uint32_t ext_seq) {
  return &s_rt.missing[ext_seq % RT_MISSING_SLOTS];
}



static void missing_clear_slot(missing_slot_t *slot) {
  if (!slot || !slot->active) return;
  memset(slot, 0, sizeof(*slot));
  if (s_rt.active_missing_count != 0U) {
    s_rt.active_missing_count--;
  }
}

static bool missing_find_min(uint32_t *min_ext) {
  if (!s_rt.missing || !min_ext) return false;
  bool found = false;
  uint32_t minv = UINT32_MAX;
  for (uint32_t i = 0; i < RT_MISSING_SLOTS; ++i) {
    const missing_slot_t *slot = &s_rt.missing[i];
    if (slot->active && (!found || slot->ext_seq < minv)) {
      minv = slot->ext_seq;
      found = true;
    }
  }
  if (found) *min_ext = minv;
  return found;
}

static void missing_add_range(uint32_t first_ext, uint16_t count,
                              uint32_t first_rtp, TickType_t now) {
  if (!s_rt.missing || count == 0) return;

  uint32_t min_active = 0;
  const bool have_min = missing_find_min(&min_active);
  /* A range cannot occupy more than the finite tracker. Avoid thousands of
   * alias/collision iterations on a large sequence jump. Keep oldest holes. */
  if (count > RT_MISSING_SLOTS) {
    count = RT_MISSING_SLOTS;
    if (!s_rt.missing_overflow_logged) {
      s_rt.missing_overflow_logged = true;
      AP_LOGW(TAG, "ALAC missing tracker overflow");
    }
  }
  const uint32_t frame_samples = s_rt.cfg.format.frame_size > 0
                                     ? (uint32_t)s_rt.cfg.format.frame_size
                                     : 352U;

  for (uint16_t i = 0; i < count; ++i) {
    const uint32_t ext = first_ext + i;
    if (have_min && ext > min_active &&
        ext - min_active >= RT_MISSING_SLOTS) {
      if (!s_rt.missing_overflow_logged) {
        s_rt.missing_overflow_logged = true;
        AP_LOGW(TAG, "ALAC missing tracker overflow");
      }
      break;
    }

    missing_slot_t *slot = missing_slot_for(ext);
    if (slot->active && slot->ext_seq != ext) {
      /* Keep the older unresolved hole rather than evicting it. A gap this
       * wide is already outside the configured recovery horizon, but it must
       * never cause a lower-audio resync/cursor jump. */
      if (!s_rt.missing_overflow_logged) {
        s_rt.missing_overflow_logged = true;
        AP_LOGW(TAG, "ALAC missing tracker overflow");
      }
      continue;
    }
    if (!slot->active) {
      memset(slot, 0, sizeof(*slot));
      slot->active = true;
      slot->ext_seq = ext;
      slot->missing_rtp = first_rtp + (uint32_t)i * frame_samples;
      slot->missing_since = now;
      if (s_rt.active_missing_count != UINT16_MAX) {
        s_rt.active_missing_count++;
      }
    }
  }
}

static void missing_mark_received(uint32_t ext_seq) {
  if (!s_rt.missing) return;
  missing_slot_t *slot = missing_slot_for(ext_seq);
  if (!slot->active || slot->ext_seq != ext_seq) return;
  missing_clear_slot(slot);
}

static bool missing_slot_due(const missing_slot_t *slot, TickType_t now) {
  if (!slot || !slot->active) return false;
  const TickType_t age = now - slot->missing_since;
  if (age < pdMS_TO_TICKS(RT_RESEND_FIRST_MS)) return false;

  int64_t time_to_play_us = 0;
  const bool deadline_valid = missing_time_to_play_us(slot, &time_to_play_us);
  if (deadline_valid &&
      time_to_play_us <= (int64_t)RT_RESEND_LAST_REQUEST_MS * 1000LL) {
    return false;
  }
  if (slot->nack_count == 0U) return true;
  if (!deadline_valid) return false;
  return (now - slot->last_nack) >= pdMS_TO_TICKS(RT_RESEND_RETRY_MS);
}

static void resend_process_event(const rt_resend_event_t *ev) {
  if (!ev) return;
  if (ev->kind == RT_RESEND_EVENT_MISSING) {
    missing_add_range(ev->ext_seq, ev->count, ev->rtp, xTaskGetTickCount());
  } else if (ev->kind == RT_RESEND_EVENT_RECEIVED) {
    missing_mark_received(ev->ext_seq);
  }
}

static void resend_giveup_expired(void) {
  if (!s_rt.missing) return;
  for (uint32_t i = 0; i < RT_MISSING_SLOTS; ++i) {
    missing_slot_t *slot = &s_rt.missing[i];
    if (!slot->active) continue;
    int64_t time_to_play_us = 0;
    if (!missing_time_to_play_us(slot, &time_to_play_us)) continue;
    if (time_to_play_us > (int64_t)RT_FINAL_LOSS_MARGIN_MS * 1000LL) continue;

    missing_clear_slot(slot);
  }
}

static void resend_scan_due(void) {
  if (!s_rt.missing) return;
  const TickType_t now = xTaskGetTickCount();

  uint32_t min_ext = 0;
  if (!missing_find_min(&min_ext)) return;

  /* Active holes are kept inside a bounded modular window. Walking from the
   * oldest active sequence preserves AirTunes' first_seq+count NACK format
   * while allowing non-contiguous holes to become separate requests. */
  for (uint32_t walked = 0; walked < RT_MISSING_SLOTS;) {
    const uint32_t ext = min_ext + walked;
    missing_slot_t *slot = missing_slot_for(ext);
    if (!slot->active || slot->ext_seq != ext || !missing_slot_due(slot, now)) {
      walked++;
      continue;
    }

    const uint32_t first_ext = ext;
    uint16_t count = 0;
    while ((uint32_t)count + walked < RT_MISSING_SLOTS &&
           count < RT_MAX_NACK_COUNT) {
      const uint32_t candidate_ext = first_ext + count;
      missing_slot_t *candidate = missing_slot_for(candidate_ext);
      if (!candidate->active || candidate->ext_seq != candidate_ext ||
          !missing_slot_due(candidate, now)) {
        break;
      }
      count++;
    }

    if (count != 0U &&
        send_retransmit_request((uint16_t)first_ext, count)) {
      for (uint16_t i = 0; i < count; ++i) {
        missing_slot_t *requested = missing_slot_for(first_ext + i);
        if (!requested->active || requested->ext_seq != first_ext + i) continue;
        requested->last_nack = now;
        if (requested->nack_count != UINT8_MAX) requested->nack_count++;
      }
    }
    walked += count ? count : 1U;
  }
}

static void reset_transport_tracking(void) {
  s_rt.newest_ext_seq = 0;
  s_rt.newest_ext_valid = false;
  if (s_rt.seen) memset(s_rt.seen, 0, RT_SEEN_SLOTS * sizeof(*s_rt.seen));
  if (s_rt.missing) memset(s_rt.missing, 0, RT_MISSING_SLOTS * sizeof(*s_rt.missing));
}

/* Admit each D7 with one coherent current-GM observation. The receiver keeps
 * its remote anchor and refreshes local presentation snapshots from live PTP
 * even between D7 packets. This is deliberately NOT a one-shot startup operation: D7 is the
 * sender's continuous RTP<->time observation, analogous to timing-anchor
 * refreshes on the buffered path. A GM transition never invalidates the old
 * local map; until the new GM is ready, conversion simply fails and playout
 * continues on the previous local anchor. */
static void service_d7_anchor(void) {
  if (s_rt.last_d7_raw_ptp_ns == 0 || s_rt.last_d7_clock_id == 0 ||
      s_rt.d7_packets == 0 || s_rt.d7_last_committed_packet == s_rt.d7_packets) {
    return;
  }

  uint64_t local_ns = 0;
  ptp_realtime_snapshot_t ps = {0};
  ptp_clock_get_realtime_snapshot(&ps);
  if (!ptp_clock_realtime_snapshot_to_local(&ps, s_rt.last_d7_clock_id,
                                           s_rt.last_d7_raw_ptp_ns, &local_ns)) {
    return;
  }
  audio_realtime_anchor_result_t anchor_result = {0};
  if (!audio_receiver_set_realtime_anchor_local(
          s_rt.last_d7_clock_id, ps.gm_change_count,
          ps.mastership_age_ms, s_rt.last_d7_raw_ptp_ns, local_ns,
          s_rt.last_d7_frame1, &anchor_result)) {
    /* A new GM is usable for PTP after ~400 ms, but an already-running media
     * phase is deliberately preserved for a longer settle window before the
     * new epoch is rebased. Keep retrying the latest D7 from the control-loop
     * timeout; no packet/ring/playout state is reset while waiting. */
    if (anchor_result.deferred &&
        s_rt.d7_defer_logged_epoch != ps.gm_change_count) {
      s_rt.d7_defer_logged_epoch = ps.gm_change_count;
      AP_LOGI(TAG,
               "ALAC GM MEDIA HOLD gm=%016" PRIx64 " epoch=%lu age=%lums "
               "keeping current RTP<->local phase until 1000ms",
               ps.master_clock_id, (unsigned long)ps.gm_change_count,
               (unsigned long)ps.mastership_age_ms);
    }
    return;
  }
  s_rt.d7_anchor_commit_count++;
  s_rt.d7_last_committed_packet = s_rt.d7_packets;

  if (s_rt.d7_anchor_commit_count == 1U) {
    AP_LOGI(TAG,
             "ALAC D7 LOCAL ANCHOR raw=%" PRIu64
             " rawLocal=%" PRIu64 " local=%" PRIu64
             " bias=%+.3fms"
             " rtp=%" PRIu32 " clock=%016" PRIx64
             " gm=%016" PRIx64 " epoch=%lu age=%lums samples=%lu",
             s_rt.last_d7_raw_ptp_ns, local_ns,
             anchor_result.effective_local_ns,
             (double)anchor_result.rebase_bias_ns / 1000000.0,
             s_rt.last_d7_frame1,
             s_rt.last_d7_clock_id, ps.master_clock_id,
             (unsigned long)ps.gm_change_count,
             (unsigned long)ps.mastership_age_ms,
             (unsigned long)ps.sample_count);
  }
}


static void process_control_packet(const uint8_t *buf, size_t len) {
  if (!buf || len < 4U) return;
  const uint8_t pt = buf[1] & 0x7fU;

  if (pt == RT_RETRANSMIT_PT) {
    if (len <= 4U || len - 4U > RT_PACKET_MAX || !s_rt.rtx_free_q) {
      return;
    }

    rt_packet_slot_t *slot = NULL;
    if (xQueueReceive(s_rt.rtx_free_q, &slot, 0) != pdTRUE ||
        !slot) {
      (void)__atomic_add_fetch(&s_rt.dropped_rtx, 1U, __ATOMIC_RELAXED);
      return;
    }
    slot->pool_kind = RT_POOL_RTX;
    slot->retransmitted = true;
    slot->len = (uint16_t)(len - 4U);
    memcpy(slot->data, buf + 4U, slot->len);
    if (!queue_work_packet(slot)) release_packet_slot(slot);
    goto done;
  }

  if (pt == RT_AP2_ANCHOR_PT) {
    if (len < 28U) {
      s_rt.d7_malformed++;
      AP_LOGW(TAG, "AP2 D7 malformed len=%u bad=%" PRIu32,
               (unsigned)len, s_rt.d7_malformed);
      goto done;
    }

    const uint32_t frame1 = read_be32(buf + 4);
    const uint64_t network_time_ns = read_be64(buf + 8);
    const uint64_t clock_id = read_be64(buf + 20);

    s_rt.last_d7_frame1 = frame1;
    s_rt.last_d7_raw_ptp_ns = network_time_ns;
    s_rt.last_d7_clock_id = clock_id;
    s_rt.d7_packets++;

    ptp_clock_note_realtime_d7(clock_id);
    service_d7_anchor();

#if defined(CONFIG_AIRPLAY_LOG_RUNTIME) && CONFIG_AIRPLAY_LOG_RUNTIME
    if (s_rt.d7_packets == 1U) {
      const uint32_t frame2 = read_be32(buf + 16);
      const uint32_t delta_frames = frame2 - frame1;
      const int sr = s_rt.cfg.format.sample_rate > 0
                         ? s_rt.cfg.format.sample_rate
                         : 44100;
      const double delta_ms = (double)delta_frames * 1000.0 / (double)sr;
      ptp_realtime_snapshot_t ps = {0};
      ptp_clock_get_realtime_snapshot(&ps);
      uint64_t local_ns = 0;
      (void)ptp_clock_realtime_snapshot_to_local(&ps, clock_id,
                                                  network_time_ns, &local_ns);
      AP_LOGI(TAG,
               "AP2 D7 n=%" PRIu32 " frame1=%" PRIu32
               " frame2=%" PRIu32 " delta=%" PRIu32 "(%.2fms)"
               " rawPTP=%" PRIu64 " clock=%016" PRIx64
               " gm=%016" PRIx64 " src=%016" PRIx64
               " ready=%d age=%lums samples=%lu local=%" PRIu64,
               s_rt.d7_packets, frame1, frame2, delta_frames, delta_ms,
               network_time_ns, clock_id, ps.master_clock_id,
               ps.source_clock_id, ps.master_ready ? 1 : 0,
               (unsigned long)ps.mastership_age_ms,
               (unsigned long)ps.sample_count, local_ns);
    }
#endif
    goto done;
  }

  if (pt == RT_SYNC_PT) {
    /* PT=84 is not used by the AirPlay 2 realtime playout map. Consume it
     * without maintaining a second passive timing state. D7/PTP remain the
     * authoritative timing inputs. */
    goto done;
  }

  {
    const unsigned word = pt >> 6;
    const uint64_t bit = 1ULL << (pt & 63U);
    if (word < 2U && (s_rt.unhandled_control_seen[word] & bit) == 0U) {
      s_rt.unhandled_control_seen[word] |= bit;
      AP_LOGI(TAG, "AP2 CTRL unhandled raw=0x%02x pt=%u len=%u",
               (unsigned)buf[1], (unsigned)pt, (unsigned)len);
    }
  }

done:
  return;
}

static void data_rx_task(void *arg) {
  (void)arg;
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_RT_DATA,
                                    xPortGetCoreID(), RT_DATA_RX_PRIORITY,
                                    RT_DATA_POOL_SLOTS);

  while (rt_running()) {
    rt_packet_slot_t *slot = NULL;
    if (xQueueReceive(s_rt.data_free_q, &slot, pdMS_TO_TICKS(20)) != pdTRUE ||
        !slot) {
      continue;
    }
    slot->pool_kind = RT_POOL_DATA;
    if (!rt_running() || s_rt.data_sock < 0) {
      release_packet_slot(slot);
      break;
    }

    const ssize_t n = recv(s_rt.data_sock, slot->data, RT_PACKET_MAX, 0);
    if (n <= 0) {
      release_packet_slot(slot);
      if (!rt_running()) break;
      continue;
    }

    slot->retransmitted = false;
    slot->len = (uint16_t)n;
    if (!queue_work_packet(slot)) {
      release_packet_slot(slot);
    }
  }

  /* The control path deletes this task only after it is truly suspended.
   * Self-deletion would leave idle-task cleanup referencing the static TCB
   * after live_tasks reached zero and the next session reused that memory. */
  for (;;) vTaskSuspend(NULL);
}

static void control_rx_task(void *arg) {
  (void)arg;
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_RT_CTRL,
                                    xPortGetCoreID(), RT_CTRL_RX_PRIORITY,
                                    RT_RTX_POOL_SLOTS);
  while (rt_running() && s_rt.control_sock >= 0) {
    const ssize_t n = recv(s_rt.control_sock, s_rt.control_packet, RT_PACKET_MAX, 0);
    if (n <= 0) {
      if (!rt_running()) break;
      /* A D7 can arrive just before the 400 ms GM-ready boundary. Retry the
       * SAME observation on the 100 ms socket timeout so startup/handover does
       * not wait for another D7 packet. The committed-packet guard above makes
       * this idempotent. */
      service_d7_anchor();
      continue;
    }
    process_control_packet(s_rt.control_packet, (size_t)n);
  }
  rt_task_release();
  vTaskDelete(NULL);
}

static void worker_note_gap(uint32_t previous_ext, uint32_t current_ext,
                            uint32_t current_rtp) {
  if (current_ext <= previous_ext + 1U) return;
  const uint32_t gap32 = current_ext - previous_ext - 1U;
  const uint16_t gap = gap32 > UINT16_MAX ? UINT16_MAX : (uint16_t)gap32;
  const uint32_t frame_samples = s_rt.cfg.format.frame_size > 0
                                     ? (uint32_t)s_rt.cfg.format.frame_size
                                     : 352U;
  const uint32_t first_rtp = current_rtp - (uint32_t)gap * frame_samples;

  (void)queue_resend_event(RT_RESEND_EVENT_MISSING, previous_ext + 1U,
                           gap, first_rtp);
}

static void alac_worker_task(void *arg) {
  (void)arg;
  alac_decoder_config_t dcfg = {
      .sample_rate = s_rt.cfg.format.sample_rate,
      .channels = s_rt.cfg.format.channels,
      .bits_per_sample = s_rt.cfg.format.bits_per_sample,
      .frame_size = s_rt.cfg.format.frame_size,
  };
  alac_decoder_t *decoder = alac_decoder_create(&dcfg);
  bool playback_announced = false;
  if (!decoder) {
    ESP_LOGE(TAG, "failed to create ALAC decoder");
    rt_request_stop();
    rt_task_release();
    vTaskDelete(NULL);
    return;
  }

  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_RT_WORK,
                                    xPortGetCoreID(), RT_WORK_PRIORITY,
                                    (uint32_t)dcfg.frame_size);

  while (rt_running() || (s_rt.work_q && uxQueueMessagesWaiting(s_rt.work_q) != 0U)) {
    rt_packet_slot_t *slot = NULL;
    if (xQueueReceive(s_rt.work_q, &slot, pdMS_TO_TICKS(20)) != pdTRUE || !slot) {
      continue;
    }
    if (!rt_running()) {
      release_packet_slot(slot);
      continue;
    }

    const size_t packet_len = slot->len;
    if (packet_len < 12U || (slot->data[0] >> 6) != 2U) {
      release_packet_slot(slot);
      continue;
    }

    const uint16_t seq = read_be16(slot->data + 2);
    const uint32_t rtp = read_be32(slot->data + 4);
    uint32_t ext_seq = seq;
    bool may_clear_missing = slot->retransmitted;

    if (!s_rt.newest_ext_valid) {
      s_rt.newest_ext_seq = seq;
      s_rt.newest_ext_valid = true;
    } else {
      ext_seq = unwrap_seq16(seq, s_rt.newest_ext_seq);
      const uint32_t previous_newest = s_rt.newest_ext_seq;
      if (!slot->retransmitted && ext_seq > previous_newest) {
        worker_note_gap(previous_newest, ext_seq, rtp);
        s_rt.newest_ext_seq = ext_seq;
      } else if (ext_seq <= previous_newest) {
        may_clear_missing = true;
      }
    }

    if (seen_has(ext_seq)) {
      if (may_clear_missing) {
        (void)queue_resend_event(RT_RESEND_EVENT_RECEIVED, ext_seq, 1U, 0U);
      }
      release_packet_slot(slot);
      continue;
    }

    const bool decoded = decode_audio_packet(decoder, slot->data, packet_len,
                                             slot->retransmitted);
    if (decoded) {
      if (!playback_announced) {
        audio_log_stereo_playback(TAG, "ALAC", dcfg.sample_rate,
                                  dcfg.bits_per_sample, "realtime");
        playback_announced = true;
      }
      seen_mark(ext_seq);
      if (may_clear_missing) {
        (void)queue_resend_event(RT_RESEND_EVENT_RECEIVED, ext_seq, 1U, 0U);
      }
    } else {
      /* A packet that arrived but failed decrypt/decode is still a media hole.
       * Ask Apple for it again instead of pretending the sequence was valid. */
      (void)queue_resend_event(RT_RESEND_EVENT_MISSING, ext_seq, 1U, rtp);
    }

    release_packet_slot(slot);
  }

  alac_decoder_destroy(decoder);
  rt_task_release();
  vTaskDelete(NULL);
}

static void resend_task(void *arg) {
  (void)arg;
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_RT_RESEND,
                                    xPortGetCoreID(), RT_RESEND_PRIORITY,
                                    RT_RESEND_SCAN_MS);
  while (rt_running()) {
    rt_resend_event_t ev = {0};
    const TickType_t wait_ticks =
        s_rt.active_missing_count == 0U
            ? portMAX_DELAY
            : pdMS_TO_TICKS(RT_RESEND_SCAN_MS);

    if (xQueueReceive(s_rt.resend_event_q, &ev, wait_ticks) == pdTRUE) {
      resend_process_event(&ev);
      /* Deadline service must run even if WORK continuously fills events. */
      for (unsigned batch = 1; batch < 32U && rt_running(); ++batch) {
        if (xQueueReceive(s_rt.resend_event_q, &ev, 0) != pdTRUE) break;
        resend_process_event(&ev);
      }
    }
    if (!rt_running()) break;

    /* With no unresolved holes there is nothing to age, request or expire.
     * Stay blocked on the event queue instead of scanning 512 slots every
     * RT_RESEND_SCAN_MS. A MISSING event wakes this task immediately. */
    if (s_rt.active_missing_count == 0U) continue;

    /* D7 state and continuous sender-anchor refreshes are owned by CTRL_RX. Keeping
     * a single owner removes the former CTRL_RX/RESEND double-commit race. */
    resend_giveup_expired();
    if (s_rt.active_missing_count != 0U) {
      resend_scan_due();
    }

  }

  rt_task_release();
  vTaskDelete(NULL);
}

size_t realtime_receiver_packet_workspace_size(void) {
  const size_t align = _Alignof(rt_packet_slot_t);
  const size_t pools =
      (size_t)(RT_DATA_POOL_SLOTS + RT_RTX_POOL_SLOTS) *
      sizeof(rt_packet_slot_t);
  /* Include enough headroom to align an arbitrary caller-provided base. */
  return pools + (align - 1U);
}

esp_err_t realtime_receiver_set_packet_workspace(void *workspace,
                                                  size_t workspace_bytes) {
  if (!workspace) return ESP_ERR_INVALID_ARG;
  if (rt_running() || !all_tasks_stopped() || s_rt.data_pool || s_rt.rtx_pool) {
    return ESP_ERR_INVALID_STATE;
  }

  const uintptr_t raw = (uintptr_t)workspace;
  const uintptr_t align = (uintptr_t)_Alignof(rt_packet_slot_t);
  const uintptr_t aligned = (raw + align - 1U) & ~(align - 1U);
  const size_t skipped = (size_t)(aligned - raw);
  const size_t data_bytes =
      (size_t)RT_DATA_POOL_SLOTS * sizeof(rt_packet_slot_t);
  const size_t rtx_bytes =
      (size_t)RT_RTX_POOL_SLOTS * sizeof(rt_packet_slot_t);
  if (workspace_bytes < skipped ||
      workspace_bytes - skipped < data_bytes + rtx_bytes) {
    return ESP_ERR_INVALID_ARG;
  }

  s_rt.data_pool = (rt_packet_slot_t *)aligned;
  s_rt.rtx_pool = (rt_packet_slot_t *)(aligned + data_bytes);
  s_rt.packet_workspace_external = true;
  return ESP_OK;
}

esp_err_t realtime_receiver_clear_packet_workspace(void) {
  if (rt_running() || !all_tasks_stopped()) return ESP_ERR_INVALID_STATE;

  /* Only detach caller-owned storage. If a future configuration lets the
   * receiver allocate these pools itself, their ownership remains here. */
  if (s_rt.packet_workspace_external) {
    s_rt.data_pool = NULL;
    s_rt.rtx_pool = NULL;
    s_rt.packet_workspace_external = false;
  }
  return ESP_OK;
}

static esp_err_t ensure_transport_resources(void) {
  if (!s_rt.data_pool) {
    s_rt.data_pool = heap_caps_calloc(RT_DATA_POOL_SLOTS, sizeof(*s_rt.data_pool),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.data_pool) s_rt.data_pool = calloc(RT_DATA_POOL_SLOTS, sizeof(*s_rt.data_pool));
  }
  if (!s_rt.rtx_pool) {
    s_rt.rtx_pool = heap_caps_calloc(RT_RTX_POOL_SLOTS, sizeof(*s_rt.rtx_pool),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.rtx_pool) s_rt.rtx_pool = calloc(RT_RTX_POOL_SLOTS, sizeof(*s_rt.rtx_pool));
  }
  if (!s_rt.control_packet) {
    s_rt.control_packet = heap_caps_malloc(RT_PACKET_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.control_packet) s_rt.control_packet = malloc(RT_PACKET_MAX);
  }
  if (!s_rt.decrypt_buf) {
    s_rt.decrypt_buf = heap_caps_malloc(RT_PACKET_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.decrypt_buf) s_rt.decrypt_buf = malloc(RT_PACKET_MAX);
  }
  if (!s_rt.pcm) {
    s_rt.pcm = heap_caps_malloc(RT_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_rt.pcm) s_rt.pcm = malloc(RT_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
  }
  if (!s_rt.seen) {
    s_rt.seen = heap_caps_calloc(RT_SEEN_SLOTS, sizeof(*s_rt.seen),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.seen) s_rt.seen = calloc(RT_SEEN_SLOTS, sizeof(*s_rt.seen));
  }
  if (!s_rt.missing) {
    s_rt.missing = heap_caps_calloc(RT_MISSING_SLOTS, sizeof(*s_rt.missing),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rt.missing) s_rt.missing = calloc(RT_MISSING_SLOTS, sizeof(*s_rt.missing));
  }

  if (!s_rt.data_free_q) s_rt.data_free_q = xQueueCreate(RT_DATA_POOL_SLOTS, sizeof(rt_packet_slot_t *));
  if (!s_rt.rtx_free_q) s_rt.rtx_free_q = xQueueCreate(RT_RTX_POOL_SLOTS, sizeof(rt_packet_slot_t *));
  if (!s_rt.work_q) s_rt.work_q = xQueueCreate(RT_WORK_QUEUE_SLOTS, sizeof(rt_packet_slot_t *));
  if (!s_rt.resend_event_q) {
    const size_t event_bytes =
        RT_RESEND_EVENT_SLOTS * sizeof(rt_resend_event_t);
    uint8_t *storage = heap_caps_malloc(
        event_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticQueue_t *control = heap_caps_malloc(
        sizeof(*control), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage || !control) {
      heap_caps_free(storage);
      heap_caps_free(control);
      ESP_LOGE(TAG, "ALAC resend queue allocation failed: PSRAM=%u internal=%u bytes",
               (unsigned)event_bytes, (unsigned)sizeof(*control));
      return ESP_ERR_NO_MEM;
    }
    QueueHandle_t queue = xQueueCreateStatic(
        RT_RESEND_EVENT_SLOTS, sizeof(rt_resend_event_t), storage, control);
    if (!queue) {
      heap_caps_free(storage);
      heap_caps_free(control);
      ESP_LOGE(TAG, "ALAC resend static queue creation failed");
      return ESP_FAIL;
    }
    s_rt.resend_event_storage = storage;
    s_rt.resend_event_control = control;
    s_rt.resend_event_q = queue;
    AP_LOGI(TAG, "ALAC resend queue: %u events, %u bytes in PSRAM; control internal",
             (unsigned)RT_RESEND_EVENT_SLOTS, (unsigned)event_bytes);
  }

  if (!s_rt.data_pool || !s_rt.rtx_pool || !s_rt.control_packet ||
      !s_rt.decrypt_buf || !s_rt.pcm || !s_rt.seen || !s_rt.missing ||
      !s_rt.data_free_q || !s_rt.rtx_free_q || !s_rt.work_q ||
      !s_rt.resend_event_q) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static void reset_transport_queues(void) {
  xQueueReset(s_rt.data_free_q);
  xQueueReset(s_rt.rtx_free_q);
  xQueueReset(s_rt.work_q);
  xQueueReset(s_rt.resend_event_q);

  for (uint32_t i = 0; i < RT_DATA_POOL_SLOTS; ++i) {
    rt_packet_slot_t *slot = &s_rt.data_pool[i];
    slot->len = 0;
    slot->retransmitted = false;
    slot->pool_kind = RT_POOL_DATA;
    (void)xQueueSend(s_rt.data_free_q, &slot, 0);
  }
  for (uint32_t i = 0; i < RT_RTX_POOL_SLOTS; ++i) {
    rt_packet_slot_t *slot = &s_rt.rtx_pool[i];
    slot->len = 0;
    slot->retransmitted = false;
    slot->pool_kind = RT_POOL_RTX;
    (void)xQueueSend(s_rt.rtx_free_q, &slot, 0);
  }
}

esp_err_t realtime_receiver_start(uint16_t data_port, uint16_t control_port,
                                  const realtime_receiver_config_t *config) {
  if (!config || !config->pcm_sink || data_port == 0 ||
      strcmp(config->format.codec, "ALAC") != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (rt_running()) return ESP_OK;
  if (!all_tasks_stopped()) return ESP_ERR_INVALID_STATE;
  esp_err_t reserve_err = realtime_receiver_reserve_data_task_memory();
  if (reserve_err != ESP_OK) return reserve_err;

  memset(&s_rt.cfg, 0, sizeof(s_rt.cfg));
  s_rt.cfg = *config;
  s_rt.decrypt_errors = 0;
  s_rt.decode_errors = 0;
  taskENTER_CRITICAL(&s_rt_control_mux);
  s_rt.client_control_valid = false;
  memset(&s_rt.client_control_addr, 0, sizeof(s_rt.client_control_addr));
  taskEXIT_CRITICAL(&s_rt_control_mux);
  s_rt.nack_request_seq = 0;
  s_rt.active_missing_count = 0;
  s_rt.d7_packets = 0;
  s_rt.d7_malformed = 0;
  s_rt.last_d7_frame1 = 0;
  s_rt.last_d7_raw_ptp_ns = 0;
  s_rt.last_d7_clock_id = 0;
  s_rt.d7_anchor_commit_count = 0;
  s_rt.d7_last_committed_packet = 0;
  s_rt.d7_defer_logged_epoch = UINT32_MAX;
  s_rt.unhandled_control_seen[0] = 0;
  s_rt.unhandled_control_seen[1] = 0;
  s_rt.missing_overflow_logged = false;
  __atomic_store_n(&s_rt.dropped_data, 0U, __ATOMIC_RELAXED);
  __atomic_store_n(&s_rt.dropped_rtx, 0U, __ATOMIC_RELAXED);
  __atomic_store_n(&s_rt.missing_backpressure, 0U, __ATOMIC_RELAXED);
  __atomic_store_n(&s_rt.received_backpressure, 0U, __ATOMIC_RELAXED);

  esp_err_t err = ensure_transport_resources();
  if (err != ESP_OK) return err;
  reset_transport_tracking();
  reset_transport_queues();

  uint16_t bound_data = 0;
  s_rt.data_sock = socket_utils_bind_udp(data_port, 0, 128 * 1024, &bound_data);
  if (s_rt.data_sock < 0 || bound_data != data_port) {
    realtime_receiver_stop();
    return ESP_FAIL;
  }
  struct timeval tv = {.tv_sec = 0, .tv_usec = RT_SOCKET_TIMEOUT_MS * 1000U};
  (void)setsockopt(s_rt.data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (control_port != 0) {
    uint16_t bound_control = 0;
    s_rt.control_sock = socket_utils_bind_udp(control_port, 0, 64 * 1024, &bound_control);
    if (s_rt.control_sock < 0 || bound_control != control_port) {
      realtime_receiver_stop();
      return ESP_FAIL;
    }
    (void)setsockopt(s_rt.control_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  rt_set_running(true);

  rt_task_reserve();
  if (xTaskCreatePinnedToCore(alac_worker_task, "alac_work", RT_WORK_STACK, NULL,
                              RT_WORK_PRIORITY, NULL, RT_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "Task start failed: alac_work stack=%u bytes "
                 "internalFree=%u internalLargest=%u internalMin=%u bytes",
             (unsigned)RT_WORK_STACK,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    rt_task_release();
    realtime_receiver_stop();
    return ESP_FAIL;
  }
  rt_task_reserve();
  if (xTaskCreatePinnedToCore(resend_task, "alac_resend", RT_RESEND_STACK, NULL,
                              RT_RESEND_PRIORITY, NULL, RT_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "Task start failed: alac_resend stack=%u bytes "
                 "internalFree=%u internalLargest=%u internalMin=%u bytes",
             (unsigned)RT_RESEND_STACK,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    rt_task_release();
    realtime_receiver_stop();
    return ESP_FAIL;
  }
  if (s_rt.control_sock >= 0) {
    rt_task_reserve();
    if (xTaskCreatePinnedToCore(control_rx_task, "alac_ctrl", RT_CTRL_RX_STACK,
                                NULL, RT_CTRL_RX_PRIORITY, NULL,
                                RT_TASK_CORE) != pdPASS) {
      ESP_LOGE(TAG, "Task start failed: alac_ctrl stack=%u bytes "
                   "internalFree=%u internalLargest=%u internalMin=%u bytes",
               (unsigned)RT_CTRL_RX_STACK,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      rt_task_release();
      realtime_receiver_stop();
      return ESP_FAIL;
    }
  }
  rt_task_reserve();
  s_data_task = xTaskCreateStaticPinnedToCore(
      data_rx_task, "alac_data", RT_DATA_RX_STACK, NULL, RT_DATA_RX_PRIORITY,
      s_data_task_stack, s_data_task_tcb, RT_TASK_CORE);
  if (!s_data_task) {
    ESP_LOGE(TAG, "Task start failed: alac_data stack=%u bytes "
                 "internalFree=%u internalLargest=%u internalMin=%u bytes",
             (unsigned)RT_DATA_RX_STACK,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    rt_task_release();
    realtime_receiver_stop();
    return ESP_FAIL;
  }

  return ESP_OK;
}

void realtime_receiver_stop(void) {
  rt_request_stop();
  /* Interrupt reads without releasing descriptor numbers while a producer
   * may still hold a socket snapshot. Close only after every task is idle. */
  if (s_rt.data_sock >= 0) shutdown(s_rt.data_sock, SHUT_RDWR);
  if (s_rt.control_sock >= 0) shutdown(s_rt.control_sock, SHUT_RDWR);

  for (int i = 0; !all_tasks_stopped() && i < 100; ++i) {
    /* No caller resumes this task. eSuspended means it has left the receive
     * loop and is no longer executing on either core. External deletion then
     * completes kernel/TLS cleanup synchronously before the TCB can be reused. */
    if (s_data_task && eTaskGetState(s_data_task) == eSuspended) {
      vTaskDelete(s_data_task);
      s_data_task = NULL;
      rt_task_release();
    }
    if (!all_tasks_stopped()) vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!all_tasks_stopped()) {
    ESP_LOGE(TAG, "Realtime stop timed out: liveTasks=%u dataTask=%p; restart blocked",
             (unsigned)__atomic_load_n(&s_rt.live_tasks, __ATOMIC_ACQUIRE),
             (void *)s_data_task);
  }

  if (all_tasks_stopped()) {
    if (s_rt.data_sock >= 0) close(s_rt.data_sock);
    if (s_rt.control_sock >= 0) close(s_rt.control_sock);
    s_rt.data_sock = s_rt.control_sock = -1;
    AUDIO_DIAG_LIFECYCLE_REALTIME_STOPPED();
  }
}

void realtime_receiver_set_client_control(uint32_t client_ip,
                                          uint16_t client_control_port) {
  struct sockaddr_in next = {0};
  const bool valid = client_ip != 0 && client_control_port != 0;
  if (valid) {
    next.sin_family = AF_INET;
    next.sin_addr.s_addr = client_ip;
    next.sin_port = htons(client_control_port);
  }

  taskENTER_CRITICAL(&s_rt_control_mux);
  s_rt.client_control_addr = next;
  s_rt.client_control_valid = valid;
  taskEXIT_CRITICAL(&s_rt_control_mux);
}

void realtime_receiver_get_usage(realtime_receiver_usage_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->work_queue_capacity = RT_WORK_QUEUE_SLOTS;
  out->dropped_data = __atomic_load_n(&s_rt.dropped_data, __ATOMIC_RELAXED);
  out->dropped_rtx = __atomic_load_n(&s_rt.dropped_rtx, __ATOMIC_RELAXED);
  out->missing_backpressure = __atomic_load_n(&s_rt.missing_backpressure, __ATOMIC_RELAXED);
  out->received_backpressure = __atomic_load_n(&s_rt.received_backpressure, __ATOMIC_RELAXED);
  if (s_rt.work_q)
    out->work_queue_depth = (uint32_t)uxQueueMessagesWaiting(s_rt.work_q);
}

bool realtime_receiver_is_running(void) { return rt_running(); }

bool realtime_receiver_is_idle(void) {
  return !rt_running() && all_tasks_stopped();
}
