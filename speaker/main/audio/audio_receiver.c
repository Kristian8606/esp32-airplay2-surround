#include "audio_receiver.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aac_decoder.h"
#include "audio_format_log.h"
#include "alac_decoder.h"
#include "airplay_audio_formats.h"
#include "audio_eq.h"
#include "audio_fade.h"
#include "audio_loudness.h"
#include "audio_crypto.h"
#include "audio_diag.h"
#include "ap2_buffered_fifo.h"
#include "pcm_fifo.h"
#include "pcm_rtp_ring.h"
#include "audio_playout.h"
#include "realtime_receiver.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "network/ptp_clock.h"
#include "network/ptp_clock_engine.h"
#include "network/socket_utils.h"
#include "latency_cal.h"
#include "cal_playback.h"
#include "cal_chirp.h"
#include "settings.h"
#include "amp_control.h"
#include "airplay_log.h"

#define AP2_PACKET_MAX             8192U
#define AP2_RX_STACK               4096U
/* Every AAC stream decodes with FDK on this task. With the local FDK patch
 * (CCE work memory on the heap) its deepest path uses ~11 KiB of stack
 * (measured on the board: 9 KiB of 20 KiB never touched on 7.1). The stack
 * is taken from PSRAM: an internal-RAM stack measured no faster, and internal
 * RAM is scarcer (Wi-Fi). Internal RAM is only the fallback. Both are created
 * "WithCaps". */
#define AP2_PROCESS_STACK          20480U
#define AP2_PROCESS_STACK_PSRAM    1 /* created WithCaps: delete WithCaps */
#define AP2_PLAYOUT_STACK          4096U
#define AP2_RT_STAGE_STACK         4096U
#define AP2_STATUS_STACK             6144U
#define AP2_AAC_RX_CORE            1  /* buffered TCP reader */
#define AP2_PLAYOUT_CORE           1  /* I2S playout + ALAC staging */
/* Multichannel: FDK stays on Core0 too. On Core1 the I2S playout task (prio
 * 8, EQ/loudness per block) preempted it so often that it fell behind: the
 * PCM lead dropped from ~850 ms to 0-200 ms and playback crackled. */
#define AP2_BUFFERED_PROCESSOR_CORE 0 /* AAC decrypt/decode/EQ */
#define AP2_RX_PRIORITY            4  /* raw TCP reader (Core1, below playout and ALAC staging) */
#define AP2_DECODE_PRIORITY        4  /* AAC processor; RTSP control (prio 17, same core) preempts it */
#define AP2_NO_TIMING_WAIT_MS      20U /* Shairport: wait ~20 ms without valid timing */
#define AP2_MAX_DEFERRED_FLUSH      10U /* Shairport MAX_DEFERRED_FLUSH_REQUESTS */

#define AP2_PLAYOUT_PRIORITY       8
#define AP2_RT_STAGE_PRIORITY      7
#define AP2_STATUS_PRIORITY          1
/* Multichannel: FDK keeps Core0 ~90% busy at priority 4, which starved this
 * priority-1 task there (no audio_status for minutes); Core1 has idle time. */
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#define AP2_STATUS_CORE              1
#else
#define AP2_STATUS_CORE              0
#endif
#define AP2_STATUS_PERIOD_MS      2000U
/* Shairport development get_ptp_anchor_local_time_info accepts a timing
 * record after 400 ms of mastership. There is no extra local lock or 1 s
 * initial-anchor gate. Reference rtp.c:1355-1356 at
 * 8c146e25183a354ae96f91fa069c2e7173f75a71. */
#define AP2_PTP_MASTERSHIP_MIN_MS PTP_CLOCK_ENGINE_READY_MS
/* A PTP estimate without a sample for this long must not create or refresh a
 * local mapping; playback keeps the cached one (holdover) until PTP returns. */
#define AP2_PTP_SAMPLE_STALE_MS   2000U
/* Resync, Shairport "resync_threshold" semantics (default 0.050 s): if the
 * output stays out of sync by more than this for AP2_RESYNC_HOLD_US (about
 * Shairport's 40-sample sync history), late audio is dropped (the cursor
 * skips forward by the error) and early audio re-primes at the presentation
 * point. Smaller errors are left to the I2S clock servo (no stuffing). */
#define AP2_RESYNC_THRESHOLD_US      50000
#define AP2_RESYNC_HOLD_US         1000000LL
/* Buffered timing watchdog: anchor + play but the PTP mapping never
 * qualifies. Normal startup takes about 400 ms; give it 5 s, then log why and restart
 * the PTP estimator. At most AP2_TIMING_WD_MAX_RESETS per anchor timeline. */
#define AP2_TIMING_WD_TIMEOUT_MS  5000U
#define AP2_TIMING_WD_RETRY_MS    6000U
#define AP2_TIMING_WD_MAX_RESETS     3U
#define AP2_PCM_CAPACITY_FRAMES    1024U

#define AP2_PID_CALC_PERIOD_US    1000000LL  /* PID math at 1 Hz */
#define AP2_PID_TUNE_PERIOD_US    1000000LL  /* live divider write: <= 1 Hz */
/* The sender time comes from the Gateway through the access point's TSF
 * (spk_time.c), microsecond-steady, not from a noisy PTP estimate: the good
 * zone can be narrow. Every speaker follows the same time, so the tighter
 * each one holds it, the closer the speakers are to each other. */
#define AP2_PID_DEADBAND_US           300    /* +/-0.3 ms is already good */
#define AP2_PID_SOFTBAND_US           800    /* 0.3..0.8 ms => gentler P action */
#define AP2_PID_MAX_PPM               160
#define AP2_PID_MIN_TUNE_PPM            1  /* the divider is dithered: any ppm is exact */
#define AP2_PID_MAX_JUMP_PPM            80

/* Continuous-start alignment. A new generation enables I2S only once.
 * Every DMA descriptor is preloaded with a silent block; they establish the
 * actual running sample-clock phase. The first tagged EOF gives a precise PTP
 * observation; while the remaining silent blocks are still playing, the first
 * real block is queued from the RTP sample that belongs at the physical DMA
 * boundary after them. I2S is never disabled between phase measurement and
 * real audio, avoiding non-repeatable second-enable latency.
 * (Blocks are AUDIO_PLAYOUT_FRAMES = 480 frames, 10.9 ms.) */
#define AP2_START_SILENCE_FUTURE_BLOCKS    2U
#define AP2_START_REAL_BOUNDARY_BLOCKS \
  (AP2_START_SILENCE_FUTURE_BLOCKS + AUDIO_PLAYOUT_DMA_BLOCKS)
/* Longest wait for a probe EOF: a warm prime's probe plays after up to all
 * queued descriptors; allow two more blocks. */
#define AP2_START_ALIGN_TIMEOUT_US \
  ((int64_t)(AUDIO_PLAYOUT_DMA_BLOCKS + 2U) * AUDIO_PLAYOUT_FRAMES * \
   1000000LL / 44100LL + 10000LL) /* slowest I2S rate: longest wait */
#define AP2_START_PRIME_GUARD_BLOCKS        4U

/* PID servo around tagged DMA-EOF phase.
 * P reacts to phase error, I learns the steady crystal/rate bias, D damps
 * motion through zero.  The derivative is low-pass filtered because the PTP
 * timestamp itself has some jitter.  PID is evaluated more often than the
 * physical tune operation so control knowledge can evolve without retuning
 * the I2S clock every second. */
#define AP2_PID_KP_PPM_PER_MS          22.0
#define AP2_PID_KI_PPM_PER_MS_S         0.55
#define AP2_PID_KD_PPM_PER_MS_PER_S    55.0
#define AP2_PID_D_ALPHA                  0.20
#define AP2_PID_I_TERM_LIMIT_PPM       110.0
/* Outside the good zone (a clock step, a new anchor) the PID terms
 * are replaced by phase recovery: hold the learned clock (I) and approach 0
 * at -sync / AP2_RECOVER_TIME_S, like the centring loop. The full PID wound
 * its integrator up on such a step and overshot: a +3 ms step went to
 * -1.8 ms on the board and took ~90 s to settle (simulation: -0.5 ms, 82 s;
 * a -6 ms step overshot +1.6 ms, 164 s). With recovery: ~0.2 ms, 35-75 s. */
#define AP2_RECOVER_TIME_S              15.0
#define AP2_RECOVER_MAX_SLOPE_US_S     120.0
/* The integrator learns only within the soft band and only while sync is not
 * already returning towards 0 faster than this, so a transient is never
 * learned as a clock error. */
#define AP2_PID_CONVERGING_MS_S          0.02

/* Centring inside the good zone (speaker-to-speaker accuracy).
 * The PID alone only holds the clock inside the zone, so sync would park near
 * its edge and the crystal error would never be learned precisely. The
 * centring loop measures the real phase slope (least squares over the 1 Hz
 * sync samples since its last step) and steers the frequency so that sync
 * approaches 0 at <= AP2_CENTER_MAX_SLOPE_US_S and then stays flat.
 * It looks every second once the fit spans AP2_CENTER_MIN_FIT_US, and steps
 * by AP2_CENTER_GAIN of the error: a slope estimate always carries some
 * noise, and a full step on it made the clock hunt between two rates 40 ppm
 * apart (sync swung by ~1 ms). Without a step the fit keeps growing, so the
 * estimate gets better, up to AP2_CENTER_WINDOW_US.
 * 1 ppm of I2S rate change = 1 us/s of sync slope. */
#define AP2_CENTER_MIN_FIT_US         8000000LL /* shortest slope fit */
#define AP2_CENTER_WINDOW_US         20000000LL /* longest slope fit */
#define AP2_CENTER_QUIET_US                30    /* |sync| below: aim for 0 slope */
#define AP2_CENTER_TIME_S                 8.0   /* approach time constant */
#define AP2_CENTER_MAX_SLOPE_US_S        30.0   /* max wanted approach speed */
#define AP2_CENTER_MAX_STEP_PPM            10    /* max frequency step per fit */
/* Hysteresis: centring mode is entered inside the good zone and is kept
 * through brief excursions. It hands back to the PID only when |sync| stays
 * above 0.7 ms for 3 s, or immediately above 1.5 ms; dropping out on any
 * momentary D-estimate spike would never let a fit complete. */
#define AP2_CENTER_EXIT_US                700
#define AP2_CENTER_EXIT_HOLD_US       3000000LL
#define AP2_CENTER_EXIT_HARD_US          1500
/* A PTP filter step moves sync by >1 ms between two 1 s samples, while real
 * drift is < 0.1 ms/s; a window containing such a jump would give a false
 * slope (-83 us/s seen on the board). The jump is subtracted from the
 * following samples of the window, so the slope stays the true drift and the
 * end-of-window phase still includes the step. */
#define AP2_CENTER_JUMP_US                400
/* Fraction of the computed correction applied per fit. With the PTP
 * reference a full step was kept (0.5 only slowed recovery); with the TSF
 * reference fits come every second, and half steps settle without hunting. */
#define AP2_CENTER_GAIN                   0.5
/* Shairport Sync 5.5.2: audio_decoded_buffer_desired_length defaults to
 * 0.75 s; buffered blocks are decoded only inside target + 0.1 s. */
#define AP2_PCM_TARGET_MS            750U
#define AP2_REALTIME_PRIME_MS        100U
#define AP2_RT_GM_REBASE_SETTLE_MS  1000U
#define AP2_BUFFERED_STORE_REQUEST_BYTES AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES
#define AP2_BUFFERED_LEAD_MS       (AP2_PCM_TARGET_MS + 100U)
/* A re-sent stretch of timeline (AutoMix) replaces the PCM already published
 * from its RTP on, if that point is still this far ahead of the audible frame.
 * The playout task reads at most the DMA queue (~87 ms) ahead. */
#define AP2_REPLACE_MIN_LEAD_MS     200U
#define AP2_PHASE_HISTORY_SAMPLES        32U

/* ALAC reorder release point. Missing PCM stays absent in the raw RTP ring
 * while retransmission runs independently. Only when the physical PTP
 * deadline is this close do we commit one frame of silence through EQ. */
#define AP2_RT_STAGE_COMMIT_MARGIN_US REALTIME_RECOVERY_FINAL_MARGIN_US

static const char *TAG = "audio_receiver";
static const char *STATUS_TAG = "audio_status";

/* Updated by RTSP control on Core0, consumed by playout on Core1. */
static volatile int32_t s_volume_target_q15 = 32768;
/* Independent physical-output mute; survives stream changes, not reboot. */
static uint32_t s_output_mute_mask;

typedef struct {
  bool anchor_valid;
  bool playing;
  uint64_t anchor_clock_id;
  uint64_t anchor_ptp_ns;
  uint64_t anchor_local_ns; /* last valid ESP-local anchor for buffered holdover */
  uint64_t anchor_local_update_us; /* last qualified same-master refresh */
  bool anchor_local_valid;
  uint32_t anchor_rtp;
  uint32_t generation;          /* timing/playout epoch */
  uint32_t media_revision;      /* changes on every buffered anchor update */
  uint32_t pcm_generation;      /* buffered media-store epoch; realtime == generation */
  audio_format_t format;
  uint32_t format_generation;
  bool timeline_reset_pending;
  audio_stream_type_t stream_type;
  uint32_t playout_latency_samples;
} timing_snapshot_t;

typedef struct {
  /* Physical tagged-DMA phase used directly by the clock servo. */
  int32_t us;
  int64_t us_q4; /* EMA state in 1/16 us: no integer-division dead zone */
  uint32_t generation;
  bool valid;

  /* Passive Shairport-style robust phase observation. It is never fed into
   * PID, cursor movement or playout decisions in this version. */
  int32_t phase_samples[AP2_PHASE_HISTORY_SAMPLES];
  uint8_t phase_count;
  uint8_t phase_index;
} output_sync_state_t;

typedef struct {
  audio_format_t format;
  audio_encrypt_t encrypt;
  audio_stream_type_t stream_type;

  uint16_t port;
  volatile bool engine_running;
  volatile bool rx_running;

  SemaphoreHandle_t publish_mutex; /* producers/FLUSH only; never I2S */
  ap2_buffered_fifo_t *transport;
  TaskHandle_t processor_task;
  TaskHandle_t playout_task;
  TaskHandle_t realtime_stage_task;
  TaskHandle_t status_task;
  SemaphoreHandle_t status_wake;
  SemaphoreHandle_t playout_wake;
  esp_timer_handle_t playout_timer;
  uint8_t *buffered_packet;
  uint8_t *decrypt_buf;
  int16_t *decode_pcm;            /* Realtime ALAC16 staging only */
  int16_t *realtime_stage_pcm;    /* ALAC raw stage read (alias of decode_pcm) */
  float *pcm_f32;                 /* Decoded float32 block, EQ -> PCM FIFO */
  uint8_t *codec_workspace;       /* shared AAC payload / ALAC large buffers */
  size_t codec_workspace_size;
  bool realtime_workspace_bound;
  pcm_fifo_t *pcm_fifo;              /* final EQ'd PCM -> PTP/I2S, play order */
  pcm_rtp_ring_t *realtime_stage_ring; /* raw decoded ALAC, RTP-addressed */

  portMUX_TYPE state_mux;
  bool playing;
  bool anchor_valid;
  uint64_t anchor_clock_id;
  uint64_t anchor_ptp_ns;
  uint64_t anchor_local_ns; /* ESP monotonic time corresponding to anchor_rtp */
  uint64_t anchor_local_update_us; /* last qualified same-master local-anchor refresh */
  bool anchor_local_valid;
  uint32_t anchor_rtp;
  /* Realtime media-domain rebase. This is deliberately separate from PTP:
   * it only maps a new GM epoch onto the already-running local media phase. */
  bool rt_media_rebase_valid;
  uint64_t rt_media_rebase_clock_id;
  uint32_t rt_media_rebase_epoch;
  int64_t rt_media_rebase_bias_ns;
  int64_t rt_media_rebase_start_us;
  bool buffered_sender_anchor_valid;
  uint64_t buffered_sender_anchor_clock_id;
  uint64_t buffered_sender_anchor_ptp_ns;
  uint32_t buffered_sender_anchor_rtp;
  uint32_t generation;          /* timing/playout epoch */
  uint32_t media_revision;      /* changes on every buffered anchor update */
  uint32_t buffered_pcm_generation; /* survives buffered seek/anchor changes */
  uint32_t format_generation;
  bool timeline_reset_pending;
  uint32_t playout_latency_samples;
  volatile bool realtime_stage_running;
  volatile bool realtime_stage_idle;
  bool realtime_stage_cursor_valid;
  uint32_t realtime_stage_cursor_rtp;
  uint32_t realtime_stage_generation;
  /* Buffered transport control state belongs to one codec/session only.
   * FLUSH decisions must never reuse a sequence number from an older AAC
   * session after ALAC was active. */
  volatile bool i2s_flush_requested;
  volatile bool playout_servo_reset_requested;
  volatile uint32_t i2s_rate_wanted;
  /* Hard stream/session boundaries are requested by the RTSP/control core but
   * executed by the single-owner playout task. The sequence pair provides a
   * bounded acknowledgement so the next codec SETUP cannot race ahead of an
   * unprocessed I2S flush/reset. */
  volatile uint32_t playout_quiesce_req;
  volatile uint32_t playout_quiesce_ack;
  output_sync_state_t output_sync;

  /* Only the existing 1 Hz PID loop publishes these. The 2 s status
   * task reads them; no packet/decode/I2S-completion instrumentation is added. */
  volatile bool status_sync_valid;
  volatile int32_t status_sync_us;
  volatile bool status_phase_valid;
  volatile int32_t status_phase_us;
  volatile int32_t status_servo_ppm;
  volatile uint32_t status_sync_generation;
} ap2_state_t;

static ap2_state_t s = {
    .stream_type = AUDIO_STREAM_NONE,
    .generation = 1,
    .buffered_pcm_generation = 1,
    .format_generation = 1,
    .timeline_reset_pending = true,
    .state_mux = portMUX_INITIALIZER_UNLOCKED,
    .realtime_stage_idle = true,
};

/* Shairport Sync keeps FLUSHBUFFERED state alongside the connection and lets
 * the single buffered-audio processor apply it to the sequential packet
 * stream. Keep the same ownership here: the raw TCP FIFO contains bytes only
 * and knows nothing about RTP, FLUSH ranges or media cursors. */
typedef struct {
  uint32_t seq;
  uint32_t rtp;
  uint32_t ssrc;
  size_t len;
  uint32_t stream_epoch;
} buffered_packet_t;

typedef struct {
  bool in_use;
  bool active;
  uint32_t from_seq;
  uint32_t from_rtp;
  uint32_t until_seq;
  uint32_t until_rtp;
  bool timestamps_valid;
} buffered_deferred_flush_t;

typedef struct {
  SemaphoreHandle_t mutex;
  uint32_t revision; /* control commands invalidate in-flight head decisions */
  bool immediate_active;
  uint32_t immediate_until_seq;
  uint32_t immediate_until_rtp;

  buffered_deferred_flush_t deferred[AP2_MAX_DEFERRED_FLUSH];
} buffered_control_t;

typedef struct {
  bool drop;
  uint32_t revision;
  bool immediate_completed;
  bool immediate_overshoot;
  uint32_t immediate_target_seq;
} buffered_packet_decision_t;

typedef struct {
  bool immediate_active;
  uint32_t immediate_target_seq;
  uint32_t deferred_requests;
} buffered_control_snapshot_t;

static buffered_control_t s_buffered_control;

static void snapshot_state(timing_snapshot_t *out);
static bool wanted_rtp_now(const timing_snapshot_t *snap, uint32_t *out);

static inline uint32_t read_be32_local(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int32_t seq23_delta(uint32_t a, uint32_t b) {
  uint32_t d = (a - b) & 0x007fffffU;
  if (d & 0x00400000U) d |= 0xff800000U;
  return (int32_t)d;
}

static void buffered_control_reset(void) {
  if (!s_buffered_control.mutex) return;
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  if (++s_buffered_control.revision == 0U) ++s_buffered_control.revision;
  s_buffered_control.immediate_active = false;
  s_buffered_control.immediate_until_seq = 0;
  s_buffered_control.immediate_until_rtp = 0;
  memset(s_buffered_control.deferred, 0, sizeof(s_buffered_control.deferred));
  xSemaphoreGive(s_buffered_control.mutex);
}

static void buffered_control_snapshot(buffered_control_snapshot_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!s_buffered_control.mutex) return;
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  out->immediate_active = s_buffered_control.immediate_active;
  out->immediate_target_seq = s_buffered_control.immediate_until_seq;
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i)
    if (s_buffered_control.deferred[i].in_use) out->deferred_requests++;
  xSemaphoreGive(s_buffered_control.mutex);
}

/* Publication recheck: a FLUSH can arrive while AAC decode/EQ runs. This
 * predicate is intentionally side-effect-free; the sole consumer remains
 * responsible for range activation/completion. Caller holds control mutex. */
static bool buffered_control_packet_flushed_locked(uint32_t seq) {
  if (s_buffered_control.immediate_active &&
      seq23_delta(seq, s_buffered_control.immediate_until_seq) < 0) return true;
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    const buffered_deferred_flush_t *r = &s_buffered_control.deferred[i];
    if (r->in_use && seq23_delta(seq, r->from_seq) >= 0 &&
        seq23_delta(seq, r->until_seq) < 0) return true;
  }
  return false;
}

static bool buffered_control_must_drain(void) {
  if (!s_buffered_control.mutex) return false;
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  bool active = s_buffered_control.immediate_active;
  if (!active) {
    for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
      if (s_buffered_control.deferred[i].in_use &&
          s_buffered_control.deferred[i].active) {
        active = true;
        break;
      }
    }
  }
  xSemaphoreGive(s_buffered_control.mutex);
  return active;
}

/* Caller holds control mutex. Key presence distinguishes RTP zero at wrap
 * from an omitted/default timestamp. Ambiguous/reversed ranges cannot safely
 * select PCM, although a valid sequence interval can still select packets. */
static esp_err_t buffered_control_add_deferred_locked(
    uint32_t from_seq, uint32_t from_rtp, uint32_t until_seq,
    uint32_t until_rtp, bool timestamps_valid) {
  from_seq &= 0x007fffffU;
  until_seq &= 0x007fffffU;
  if (from_seq == until_seq) return ESP_OK; /* empty interval */
  if (seq23_delta(until_seq, from_seq) <= 0) return ESP_ERR_INVALID_ARG;

  int slot = -1;
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    if (!s_buffered_control.deferred[i].in_use) { slot = (int)i; break; }
  }
  if (slot < 0) return ESP_ERR_NO_MEM;
  s_buffered_control.deferred[slot] = (buffered_deferred_flush_t){
      .in_use = true, .active = false,
      .from_seq = from_seq, .from_rtp = from_rtp,
      .until_seq = until_seq, .until_rtp = until_rtp,
      .timestamps_valid = timestamps_valid &&
                          (int32_t)(until_rtp - from_rtp) > 0,
  };
  if (++s_buffered_control.revision == 0U) ++s_buffered_control.revision;
  return ESP_OK;
}

typedef struct {
  uint32_t from_seq, from_rtp, until_seq, until_rtp;
  uint8_t kind;
} flush_event_t;
enum { FLUSH_EV_ACTIVATE = 1, FLUSH_EV_END = 2, FLUSH_EV_OVERSHOOT = 3,
       FLUSH_EV_OBSOLETE = 5 };
typedef struct {
  const buffered_packet_t *packet;
  buffered_packet_decision_t *decision;
  flush_event_t events[AP2_MAX_DEFERRED_FLUSH * 2U];
  uint32_t event_count;
} buffered_classification_t;

/* Control -> FIFO locks are held. Only metadata/state operations here: no
 * logging, waiting or payload copying. Reconnect cannot invalidate the head
 * during activation/completion of a sequence request. */
static void buffered_control_classify_current_head(void *context) {
  buffered_classification_t *classification = context;
  const buffered_packet_t *packet = classification->packet;
  buffered_packet_decision_t *decision = classification->decision;
  flush_event_t *events = classification->events;
  uint32_t event_count = 0;

  decision->revision = s_buffered_control.revision;

  /* Shairport 5.5.2: immediate FLUSH consumes every sequential block before
   * flushUntilSeq. The endpoint block itself survives; overshoot also ends the
   * request and keeps the overshooting block. Completion cancels all deferred
   * requests. There is deliberately no RTP rescue, plausibility window or
   * byte-FIFO shortcut. */
  if (s_buffered_control.immediate_active) {
    const int32_t d = seq23_delta(packet->seq,
                                  s_buffered_control.immediate_until_seq);
    decision->immediate_target_seq = s_buffered_control.immediate_until_seq;
    if (d >= 0) {
      decision->immediate_completed = true;
      decision->immediate_overshoot = d > 0;
      s_buffered_control.immediate_active = false;
      memset(s_buffered_control.deferred, 0, sizeof(s_buffered_control.deferred));
    } else {
      decision->drop = true;
    }
  }

  /* Deferred [fromSeq, untilSeq) requests are evaluated against the same
   * sequential packet stream (Shairport). Endpoint and overshoot packets
   * survive. */
  for (uint32_t i = 0; i < AP2_MAX_DEFERRED_FLUSH; ++i) {
    buffered_deferred_flush_t *r = &s_buffered_control.deferred[i];
    if (!r->in_use) continue;

    const int32_t from_delta = seq23_delta(packet->seq, r->from_seq);
    const int32_t until_delta = seq23_delta(packet->seq, r->until_seq);
    if (until_delta >= 0) {
      if (event_count < AP2_MAX_DEFERRED_FLUSH * 2U)
        events[event_count++] = (flush_event_t){r->from_seq, r->from_rtp,
            r->until_seq, r->until_rtp,
            !r->active ? FLUSH_EV_OBSOLETE
                       : until_delta == 0 ? FLUSH_EV_END : FLUSH_EV_OVERSHOOT};
      r->active = false;
      r->in_use = false;
    } else if (r->active || from_delta == 0) {
      /* Shairport: a deferred request activates only on the packet whose
       * sequence number is exactly flushFromSeq. A request that arrives after
       * that packet stays inactive and simply ends at/after flushUntilSeq. */
      if (!r->active) {
        r->active = true;
        if (event_count < AP2_MAX_DEFERRED_FLUSH * 2U)
          events[event_count++] = (flush_event_t){r->from_seq, r->from_rtp,
              r->until_seq, r->until_rtp, FLUSH_EV_ACTIVATE};
      }
      decision->drop = true;
    }
  }

  classification->event_count = event_count;
}

static esp_err_t buffered_control_classify(
    const buffered_packet_t *packet, const ap2_buffered_fifo_head_t *head,
    buffered_packet_decision_t *decision) {
  if (!decision) return ESP_ERR_INVALID_ARG;
  memset(decision, 0, sizeof(*decision));
  if (!packet || !head || !s_buffered_control.mutex) return ESP_ERR_INVALID_ARG;
  buffered_classification_t classification = {
      .packet = packet, .decision = decision,
  };
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  const esp_err_t err = ap2_buffered_fifo_visit_head(
      s.transport, head, buffered_control_classify_current_head, &classification);
  xSemaphoreGive(s_buffered_control.mutex);
  if (err != ESP_OK) return err;

  for (uint32_t i = 0; i < classification.event_count; ++i) {
    const flush_event_t *e = &classification.events[i];
    AP_LOGI(TAG,
             "deferred FLUSH [%" PRIu32 "/%" PRIu32 " .. %" PRIu32 "/%" PRIu32 ") %s at seq=%" PRIu32 " rtp=%" PRIu32,
             e->from_seq, e->from_rtp, e->until_seq, e->until_rtp,
             e->kind == FLUSH_EV_ACTIVATE ? "activated"
             : e->kind == FLUSH_EV_OBSOLETE ? "already obsolete"
             : e->kind == FLUSH_EV_END ? "ended" : "ended by overshoot",
             packet->seq, packet->rtp);
  }
  return ESP_OK;
}


static inline void realtime_stage_kick(void) {
  TaskHandle_t task = s.realtime_stage_task;
  if (task) {
    xTaskNotifyGive(task);
  }
}

static TickType_t realtime_stage_wait_ticks(int64_t wait_us) {
  if (wait_us <= 0) return 0;
  const uint64_t tick_us = (uint64_t)portTICK_PERIOD_MS * 1000ULL;
  if (tick_us == 0) return 1;

  /* Floor to the previous RTOS tick so the notification timeout cannot move
   * the existing final staging margin later. A sub-tick remainder simply
   * wakes one tick early and re-evaluates the same deadline. */
  uint64_t ticks64 = (uint64_t)wait_us / tick_us;
  if (ticks64 == 0) ticks64 = 1;
  if (ticks64 >= (uint64_t)portMAX_DELAY) ticks64 = (uint64_t)portMAX_DELAY - 1ULL;
  return (TickType_t)ticks64;
}

/* Remove a GM handover compensation at 50 us/second. This is phase
 * convergence, not another hardware-rate controller. The initial handover
 * stays continuous; a real old-epoch error is no longer preserved forever. */
static int64_t realtime_remaining_bias(int64_t bias_ns, int64_t start_us,
                                        int64_t now_us) {
  if (bias_ns == 0 || now_us <= start_us) return bias_ns;
  const uint64_t elapsed_us = (uint64_t)now_us - (uint64_t)start_us;
  const uint64_t magnitude = bias_ns < 0
      ? (uint64_t)(-(bias_ns + 1)) + 1U : (uint64_t)bias_ns;
  /* 50 ppm = 0.05 ns per microsecond, i.e. 1 ns per 20 us. */
  const uint64_t correction_ns = elapsed_us / 20U;
  if (correction_ns >= magnitude) return 0;
  return bias_ns < 0 ? bias_ns + (int64_t)correction_ns
                     : bias_ns - (int64_t)correction_ns;
}

static void playout_wake(void) {
  if (s.playout_wake) xSemaphoreGive(s.playout_wake);
}

static void playout_timer_callback(void *arg) {
  (void)arg;
  playout_wake();
}

static void media_control_wake(void) {
  ap2_buffered_fifo_notify(s.transport);
  playout_wake();
}

/* Pure read: only playout refreshes the cached PTP mapping. */
static void snapshot_state_locked(timing_snapshot_t *out) {
  out->anchor_valid = s.anchor_valid;
  out->playing = s.playing;
  out->anchor_clock_id = s.anchor_clock_id;
  out->anchor_ptp_ns = s.anchor_ptp_ns;
  out->anchor_local_ns = s.anchor_local_ns;
  out->anchor_local_update_us = s.anchor_local_update_us;
  out->anchor_local_valid = s.anchor_local_valid;
  out->anchor_rtp = s.anchor_rtp;
  out->generation = s.generation;
  out->media_revision = s.media_revision;
  out->pcm_generation =
      s.stream_type == AUDIO_STREAM_BUFFERED ? s.buffered_pcm_generation
                                             : s.generation;
  out->format = s.format;
  out->format_generation = s.format_generation;
  out->timeline_reset_pending = s.timeline_reset_pending;
  out->stream_type = s.stream_type;
  out->playout_latency_samples = s.playout_latency_samples;
}

static void snapshot_state(timing_snapshot_t *out) {
  taskENTER_CRITICAL(&s.state_mux);
  snapshot_state_locked(out);
  taskEXIT_CRITICAL(&s.state_mux);
}

/* Buffered AAC: may this PTP snapshot map the anchor's remote time to local
 * time? `initial` = the anchor has no local mapping yet (start/new anchor). */
static bool buffered_ptp_qualified(const ptp_clock_snapshot_t *ps,
                                   uint64_t anchor_clock_id, bool initial) {
  (void)initial;
  if (!ps || ps->realtime_mode || !ps->valid) return false;
  if (ps->grandmaster_clock_id == 0 ||
      ps->grandmaster_clock_id != anchor_clock_id)
    return false;
  if (ps->sample_age_ms > AP2_PTP_SAMPLE_STALE_MS) return false;
  return ps->mastership_age_ms >= AP2_PTP_MASTERSHIP_MIN_MS;
}

/* First-audio measurement: anchor commit time per generation. */
static volatile int64_t s_anchor_commit_us = 0;
static volatile uint32_t s_anchor_commit_gen = 0;

/* Single writer for PTP refresh; control still owns anchor publication. */
static void refresh_timing_snapshot(timing_snapshot_t *out) {
  uint64_t clock_id;
  uint32_t epoch;
  int64_t bias_ns, bias_start_us;
  taskENTER_CRITICAL(&s.state_mux);
  snapshot_state_locked(out);
  const bool was_local_valid = out->anchor_local_valid;
  clock_id = s.anchor_clock_id;
  epoch = s.rt_media_rebase_epoch;
  bias_ns = s.rt_media_rebase_bias_ns;
  bias_start_us = s.rt_media_rebase_start_us;
  taskEXIT_CRITICAL(&s.state_mux);

  if (out->stream_type == AUDIO_STREAM_BUFFERED && out->anchor_valid &&
      !out->timeline_reset_pending) {
    ptp_clock_snapshot_t ps = {0};
    ptp_clock_get_snapshot(&ps);
    if (!ps.realtime_mode) {
      const uint64_t now_us = (uint64_t)esp_timer_get_time();
      const bool same_master = ps.grandmaster_clock_id != 0 &&
                               ps.grandmaster_clock_id == out->anchor_clock_id;
      const bool qualified_master = buffered_ptp_qualified(
          &ps, out->anchor_clock_id, !out->anchor_local_valid);

      if (same_master && qualified_master) {
        /* Shairport Sync semantics: while the advertised master is the same
         * clock that owns the RTSP anchor, continuously refresh the local
         * representation and remember exactly when that mapping was last
         * known-good. A transient GM may not move this cached local point. */
        uint64_t local_anchor = 0;
        if (ptp_clock_engine_remote_to_local(
                out->anchor_ptp_ns, ps.filtered_offset_ns, &local_anchor)) {
          bool refreshed = false;
          taskENTER_CRITICAL(&s.state_mux);
          if (s.stream_type == AUDIO_STREAM_BUFFERED && s.anchor_valid &&
              !s.timeline_reset_pending && s.generation == out->generation &&
              s.media_revision == out->media_revision &&
              s.anchor_rtp == out->anchor_rtp &&
              s.anchor_clock_id == out->anchor_clock_id &&
              s.anchor_ptp_ns == out->anchor_ptp_ns) {
            s.anchor_local_ns = local_anchor;
            s.anchor_local_update_us = now_us;
            s.anchor_local_valid = true;
            refreshed = true;
          }
          taskEXIT_CRITICAL(&s.state_mux);
          if (refreshed) {
            out->anchor_local_ns = local_anchor;
            out->anchor_local_update_us = now_us;
            out->anchor_local_valid = true;
          }
        }
      } else if (!same_master && ps.grandmaster_clock_id != 0 &&
                 out->anchor_local_valid && out->anchor_local_ns != 0 &&
                 out->anchor_local_update_us != 0) {
        /* Shairport does not time handover from first mismatch observation.
         * It measures from the last successful local-anchor update by the old
         * master. That timestamp stops moving as soon as the GM differs. */
        const uint64_t stale_age_us =
            now_us >= out->anchor_local_update_us
                ? now_us - out->anchor_local_update_us
                : 0;
        const uint32_t stale_age_ms =
            stale_age_us / 1000ULL > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)(stale_age_us / 1000ULL);


        /* Only a qualified new GM may take ownership. Rebuild its remote
         * anchor from the exact same cached ESP-local media point. */
        if (ps.valid &&
            ptp_clock_engine_handover_ready(
                out->anchor_local_valid, ps.locked, out->anchor_clock_id,
                ps.grandmaster_clock_id, ps.mastership_age_ms, stale_age_ms)) {
          uint64_t new_remote_anchor = 0;
          if (ptp_clock_engine_local_to_remote(
                  out->anchor_local_ns, ps.filtered_offset_ns,
                  &new_remote_anchor)) {
            bool committed = false;
            taskENTER_CRITICAL(&s.state_mux);
            if (s.stream_type == AUDIO_STREAM_BUFFERED && s.anchor_valid &&
                !s.timeline_reset_pending && s.generation == out->generation &&
                s.media_revision == out->media_revision &&
                s.anchor_rtp == out->anchor_rtp &&
                s.anchor_clock_id == out->anchor_clock_id &&
                s.anchor_ptp_ns == out->anchor_ptp_ns &&
                s.anchor_local_valid &&
                s.anchor_local_ns == out->anchor_local_ns &&
                s.anchor_local_update_us == out->anchor_local_update_us) {
              s.anchor_clock_id = ps.grandmaster_clock_id;
              s.anchor_ptp_ns = new_remote_anchor;
              /* Deliberately preserve anchor_local_ns and its old update time.
               * On the next qualified snapshot the now-current GM refreshes
               * them, matching Shairport's two-step handover behaviour. */
              committed = true;
            }
            taskEXIT_CRITICAL(&s.state_mux);
            if (committed) {
              out->anchor_clock_id = ps.grandmaster_clock_id;
              out->anchor_ptp_ns = new_remote_anchor;
            }
          }
        }
      }
    }
  }

  if (out->stream_type == AUDIO_STREAM_REALTIME && out->anchor_valid &&
      !out->timeline_reset_pending && out->anchor_ptp_ns != 0U) {
    ptp_realtime_snapshot_t ps;
    ptp_clock_get_realtime_snapshot(&ps);
    uint64_t local_ns;
    if (ps.gm_change_count == epoch &&
        ptp_clock_realtime_snapshot_to_local(
            &ps, clock_id, out->anchor_ptp_ns, &local_ns)) {
      const int64_t remaining = realtime_remaining_bias(
          bias_ns, bias_start_us, esp_timer_get_time());
      /* All normal timestamps are ESP uptime values. Reject overflow instead
       * of publishing an invalid deadline if control input is malformed. */
      if ((remaining > 0 && local_ns > (uint64_t)(INT64_MAX - remaining)) ||
          (remaining < 0 && local_ns < (uint64_t)(-(remaining + 1)) + 1U)) {
        snapshot_state(out);
        return;
      }
      const int64_t effective = (int64_t)local_ns + remaining;
      if (effective <= 0) { snapshot_state(out); return; }
      out->anchor_local_ns = (uint64_t)effective;
      /* Cache the most recent valid local map for a temporary PTP outage or
       * GM acquisition. Never overwrite an anchor changed during the lookup. */
      taskENTER_CRITICAL(&s.state_mux);
      if (s.stream_type == out->stream_type && s.generation == out->generation &&
          s.anchor_valid && !s.timeline_reset_pending &&
          s.anchor_clock_id == clock_id && s.rt_media_rebase_epoch == epoch &&
          s.anchor_rtp == out->anchor_rtp && s.anchor_ptp_ns == out->anchor_ptp_ns) {
        s.anchor_local_ns = out->anchor_local_ns;
      }
      taskEXIT_CRITICAL(&s.state_mux);
    }
  }
  /* Return the committed map, including any control change during lookup. */
  snapshot_state(out);
  if (!was_local_valid && out->anchor_local_valid)
    ap2_buffered_fifo_notify(s.transport);
}

static uint32_t next_generation(uint32_t generation) {
  generation++;
  return generation ? generation : 1U;
}

/* Whole-store buffered media invalidation. Used at real stream/session
 * boundaries and immediate FLUSH registration. Ordinary pause and harmless
 * anchor refresh preserve the decoded PCM already in the PCM FIFO. */
static uint32_t reset_buffered_pcm_store(void) {
  taskENTER_CRITICAL(&s.state_mux);
  /* All identities used by the shared PCM FIFO come from the timing epoch
   * allocator. A hard media reset is also a hard timing boundary; incrementing
   * a separate AAC counter could reuse the preceding ALAC FIFO generation. */
  const uint32_t new_gen = s.generation = next_generation(s.generation);
  if (s.pcm_fifo) {
    pcm_fifo_set_generation(s.pcm_fifo, new_gen);
  }
  s.buffered_pcm_generation = new_gen;
  taskEXIT_CRITICAL(&s.state_mux);
  return new_gen;
}

/* Pause/immediate FLUSH ends the current presentation timing now. TCP keeps
 * arriving into the raw FIFO while the anchor is invalid; the sequential
 * consumer alone decides whether each framed packet is discarded or decoded. */
static void mark_timeline_discontinuity(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.anchor_valid = false;
  s.timeline_reset_pending = true;
  s.anchor_local_update_us = 0;
  s.anchor_local_valid = false;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
}

/* PAUSE/rate=0 is a presentation-clock stop, not by itself a compressed-media
 * discontinuity. AirPlay may emit many pause/resume anchor updates while the
 * user holds the seek buttons. Only FLUSH/FLUSHBUFFERED owns media-cursor
 * invalidation. Preserve an already-pending media reset from a preceding
 * FLUSH, but do not create a new one just because playback paused. */
static void pause_presentation_timing(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.anchor_valid = false;
  s.anchor_local_update_us = 0;
  s.anchor_local_valid = false;
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
}

static uint32_t commit_anchor_epoch_locked(void) {
  uint32_t gen = s.generation;
  if (s.timeline_reset_pending) {
    gen = next_generation(gen);
    /* Timing/playout epoch changes are not automatically PCM-store resets.
     * Buffered AAC may keep the decoded PCM in the FIFO across an ordinary
     * pause/anchor refresh, but compressed packets behind the decoder are not a
     * rewind cache. Realtime ALAC still owns a generation-scoped raw stage ring
     * and the PCM FIFO, because its recovery/EQ chronology is one continuous
     * realtime epoch. */
    if (s.stream_type == AUDIO_STREAM_REALTIME) {
      if (s.pcm_fifo) {
        pcm_fifo_set_generation(s.pcm_fifo, gen);
      }
      if (s.realtime_stage_ring) {
        pcm_rtp_ring_set_generation(s.realtime_stage_ring, gen);
      }
      s.realtime_stage_cursor_valid = false;
      s.realtime_stage_generation = gen;
    }
    s.generation = gen;
    s.timeline_reset_pending = false;
  }
  return gen;
}

static inline int32_t rtp_delta(uint32_t a, uint32_t b) {
  return (int32_t)(a - b);
}

/* Presentation-clock boundary. Both codecs schedule against ESP monotonic
 * time. Buffered AAC continuously converts its remote anchor to local time
 * while its anchor GM is current, then freezes that local anchor across GM
 * acquisition exactly like Shairport Sync. */
static bool timing_clock_ready(const timing_snapshot_t *snap) {
  if (!snap || !snap->anchor_valid || snap->timeline_reset_pending) return false;
  if (snap->stream_type == AUDIO_STREAM_BUFFERED)
    return snap->anchor_local_valid && snap->anchor_local_ns != 0;
  return snap->anchor_local_ns != 0;
}

static uint64_t presentation_now_ns(const timing_snapshot_t *snap) {
  (void)snap;
  return (uint64_t)esp_timer_get_time() * 1000ULL;
}

/* Output latency after the ESP (DAC/DSP/amp), in microseconds.
 * Positive = the chain delays the sound, so the ESP must output that much
 * EARLIER. Applied here, the single point every presentation-time calculation
 * (start, cursor, sync measurement, realtime deadlines) goes through; the PTP
 * map (anchor_local_ns) itself is untouched. Like Shairport's
 * audio_backend_latency_offset_in_seconds, with the opposite sign convention
 * (ours is "how late the hardware is"). */
#define AP2_OUTPUT_LATENCY_MIN_US  (-100000)
#define AP2_OUTPUT_LATENCY_MAX_US  150000
static volatile int32_t s_output_latency_us = 0;
/* Surround speaker delay: the opposite sign, the speaker plays later. */
#define AP2_EXTRA_DELAY_MAX_US     100000
static volatile int32_t s_extra_delay_us = 0;

static uint64_t presentation_anchor_ns(const timing_snapshot_t *snap) {
  if (!snap) return 0;
  const int64_t off_ns =
      ((int64_t)__atomic_load_n(&s_output_latency_us, __ATOMIC_RELAXED) -
       (int64_t)__atomic_load_n(&s_extra_delay_us, __ATOMIC_RELAXED)) * 1000LL;
  return (uint64_t)((int64_t)snap->anchor_local_ns - off_ns);
}

static bool wanted_rtp_now(const timing_snapshot_t *snap, uint32_t *out) {
  if (!snap || !out || !snap->playing || !timing_clock_ready(snap)) return false;
  const int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
  const int64_t now_ns = (int64_t)presentation_now_ns(snap);
  const int64_t dt_ns = now_ns - (int64_t)presentation_anchor_ns(snap);
  int64_t ds = (dt_ns * (int64_t)sr) / 1000000000LL;
  ds -= (int64_t)snap->playout_latency_samples;
  *out = snap->anchor_rtp + (uint32_t)ds;
  return true;
}

static bool wanted_rtp_at_presentation_ns(const timing_snapshot_t *snap,
                                           uint64_t time_ns, uint32_t *out) {
  if (!snap || !out || !snap->playing || !timing_clock_ready(snap)) return false;
  const int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
  const int64_t dt_ns = (int64_t)time_ns - (int64_t)presentation_anchor_ns(snap);
  int64_t ds = (dt_ns * (int64_t)sr) / 1000000000LL;
  ds -= (int64_t)snap->playout_latency_samples;
  *out = snap->anchor_rtp + (uint32_t)ds;
  return true;
}

/* Permanent audio-status policy:
 * - exactly one low-priority task owns periodic audio-status logging;
 * - the hot packet/decode path maintains no counters/timers just for logging;
 * - the playout task publishes sync/ppm only at its already-existing 1 Hz PID;
 * - one persistent task sleeps on a semaphore while paused; control never waits. */
static double status_frames_to_ms(uint32_t frames, int sample_rate) {
  const uint32_t sr = sample_rate > 0 ? (uint32_t)sample_rate : 44100U;
  return ((double)frames * 1000.0) / (double)sr;
}

static void status_publish_sync(int32_t sync_us, int32_t phase_us,
                                int32_t servo_ppm, uint32_t generation) {
  __atomic_store_n(&s.status_sync_us, sync_us, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_phase_us, phase_us, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_servo_ppm, servo_ppm, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_sync_generation, generation, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_phase_valid, true, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_sync_valid, true, __ATOMIC_RELEASE);
}

static void status_invalidate_sync(void) {
  __atomic_store_n(&s.status_phase_valid, false, __ATOMIC_RELAXED);
  __atomic_store_n(&s.status_sync_valid, false, __ATOMIC_RELEASE);
}

static int32_t robust_phase_center_us(const output_sync_state_t *sync) {
  if (!sync || !sync->valid || sync->phase_count == 0) return 0;
  if (sync->phase_count < 4U) return sync->us;

  int32_t sorted[AP2_PHASE_HISTORY_SAMPLES];
  const uint32_t n = sync->phase_count;
  for (uint32_t i = 0; i < n; ++i) sorted[i] = sync->phase_samples[i];
  for (uint32_t i = 1; i < n; ++i) {
    const int32_t v = sorted[i];
    uint32_t j = i;
    while (j > 0 && sorted[j - 1U] > v) {
      sorted[j] = sorted[j - 1U];
      --j;
    }
    sorted[j] = v;
  }
  return (int32_t)(((int64_t)sorted[1] + (int64_t)sorted[n - 2U]) / 2LL);
}

/* Buffered AAC timing watchdog, run from the low-priority status task.
 * Decoding is gated on anchor_local_valid, which needs a LOCKED PTP estimator
 * on the anchor's master for >= 400 ms. If that never happens the stream is
 * silent while the FIFO fills (seen on the first session after boot).
 * Log the exact failing condition and reset the estimator. */
static void buffered_timing_watchdog(const timing_snapshot_t *snap) {
  static uint32_t wd_generation = 0;
  static int64_t wd_since_us = 0;
  static int64_t wd_last_reset_us = 0;
  static uint32_t wd_resets = 0;

  const bool waiting = snap->stream_type == AUDIO_STREAM_BUFFERED &&
                       snap->playing && snap->anchor_valid &&
                       !snap->timeline_reset_pending &&
                       !snap->anchor_local_valid;
  const int64_t now_us = esp_timer_get_time();
  if (!waiting) {
    wd_since_us = 0;
    if (snap->generation != wd_generation) wd_resets = 0;
    wd_generation = snap->generation;
    return;
  }
  if (snap->generation != wd_generation || wd_since_us == 0) {
    if (snap->generation != wd_generation) wd_resets = 0;
    wd_generation = snap->generation;
    wd_since_us = now_us;
    wd_last_reset_us = 0;
    return;
  }
  const int64_t waited_ms = (now_us - wd_since_us) / 1000LL;
  if (waited_ms < (int64_t)AP2_TIMING_WD_TIMEOUT_MS) return;
  if (wd_last_reset_us != 0 &&
      (now_us - wd_last_reset_us) / 1000LL < (int64_t)AP2_TIMING_WD_RETRY_MS)
    return;
  if (wd_resets >= AP2_TIMING_WD_MAX_RESETS) return;

  ptp_clock_snapshot_t ps = {0};
  ptp_clock_get_snapshot(&ps);
  const char *reason =
      ps.realtime_mode ? "ptp-in-realtime-mode"
      : !ps.valid ? "no-ptp-samples"
      : ps.source_mixed ? "ptp-source-mixed"
      : ps.grandmaster_clock_id != snap->anchor_clock_id ? "gm-differs-from-anchor"
      : ps.mastership_age_ms < AP2_PTP_MASTERSHIP_MIN_MS ? "mastership-too-young"
      : "anchor-map-failed";
  AP_LOGW(TAG,
           "TIMING WATCHDOG: anchor waiting %lld ms, reason=%s | valid=%d locked=%d "
           "mixed=%d rt=%d src=%016llx gm=%016llx anchor=%016llx age=%lums "
           "samples=%lu sampleAge=%lums -> PTP reset %lu/%u",
           (long long)waited_ms, reason, ps.valid, ps.locked, ps.source_mixed,
           ps.realtime_mode, (unsigned long long)ps.source_clock_id,
           (unsigned long long)ps.grandmaster_clock_id,
           (unsigned long long)snap->anchor_clock_id,
           (unsigned long)ps.mastership_age_ms, (unsigned long)ps.sample_count,
           (unsigned long)ps.sample_age_ms,
           (unsigned long)(wd_resets + 1U), (unsigned)AP2_TIMING_WD_MAX_RESETS);
  ptp_clock_clear();
  wd_resets++;
  wd_last_reset_us = now_us;
}

/* The playout task never logs. It posts numbers into these
 * single-writer mailboxes (seqlock) and the low-priority audio_status_task
 * formats and prints them. A log line can block on UART/USB for milliseconds;
 * that must never happen on the task that feeds I2S. */
typedef struct {
  volatile uint32_t seq;   /* odd while the writer is updating */
  volatile uint32_t count; /* events posted so far */
  int32_t v[5];
  const char *str;
} playout_evt_t;

enum {
  PEVT_START = 0,
  PEVT_RESYNC,
  PEVT_CENTER,
  PEVT_TUNE_FAIL,
  PEVT_FLUSH_FAIL,
  PEVT_SERVO_RESET_FAIL,
  PEVT_COUNT
};
static playout_evt_t s_pevt[PEVT_COUNT];

static void pevt_post(int kind, int32_t a, int32_t b, int32_t c, int32_t d,
                      int32_t e, const char *str) {
  playout_evt_t *ev = &s_pevt[kind];
  const uint32_t q = ev->seq;
  __atomic_store_n(&ev->seq, q + 1U, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  ev->v[0] = a; ev->v[1] = b; ev->v[2] = c; ev->v[3] = d; ev->v[4] = e;
  ev->str = str;
  ev->count = ev->count + 1U;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  __atomic_store_n(&ev->seq, q + 2U, __ATOMIC_RELAXED);
}

/* Reader side (status task). Returns true once per new event; `missed` is the
 * number of additional events of this kind since the previous report. */
static bool pevt_take(int kind, uint32_t *last_count, int32_t v[5],
                      const char **str, uint32_t *missed) {
  playout_evt_t *ev = &s_pevt[kind];
  for (int tries = 0; tries < 3; ++tries) {
    const uint32_t q1 = __atomic_load_n(&ev->seq, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (q1 & 1U) continue;
    const uint32_t count = ev->count;
    for (int i = 0; i < 5; ++i) v[i] = ev->v[i];
    const char *sp = ev->str;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&ev->seq, __ATOMIC_RELAXED) != q1) continue;
    if (count == *last_count) return false;
    *missed = count - *last_count - 1U;
    *last_count = count;
    if (str) *str = sp;
    return true;
  }
  return false; /* writer busy; pick it up on the next cycle */
}

/* Format and print what the playout task posted. */
static void status_print_playout_events(void) {
  static uint32_t last[PEVT_COUNT];
  int32_t v[5];
  const char *str = NULL;
  uint32_t missed = 0;
  if (pevt_take(PEVT_START, &last[PEVT_START], v, NULL, &missed))
    AP_LOGI(TAG, "PLAYOUT START gen=%ld: first audio %ld ms after anchor "
                  "(ptp locked=%ld mastership=%ldms samples=%ld)",
             (long)v[0], (long)v[1], (long)v[2], (long)v[3], (long)v[4]);
  if (pevt_take(PEVT_RESYNC, &last[PEVT_RESYNC], v, NULL, &missed))
    AP_LOGW(TAG, "RESYNC #%ld: sync error %+.2f ms beyond %.0f ms for %ld ms "
                  "-> %s (I2S servo %+ld ppm kept)",
             (long)v[0], (double)v[1] / 1000.0,
             (double)AP2_RESYNC_THRESHOLD_US / 1000.0, (long)v[2],
             v[4] > 0 ? "late: dropped the late audio" : "early: re-prime",
             (long)v[3]);
  if (pevt_take(PEVT_CENTER, &last[PEVT_CENTER], v, NULL, &missed))
    ESP_LOGI(TAG, "SERVO CENTER: sync %+.2f ms, slope %+.1f us/s -> I2S %+ld ppm%s",
             (double)v[0] / 1000.0, (double)v[1] / 10.0, (long)v[2],
             missed ? " (+earlier steps)" : "");
  if (pevt_take(PEVT_TUNE_FAIL, &last[PEVT_TUNE_FAIL], v, NULL, &missed))
    ESP_LOGW(TAG, "I2S clock tune to %+ld ppm failed: %s (failures=%ld); "
                  "drift is NOT being corrected",
             (long)v[0], esp_err_to_name((esp_err_t)v[1]), (long)v[2]);
  if (pevt_take(PEVT_FLUSH_FAIL, &last[PEVT_FLUSH_FAIL], v, &str, &missed))
    ESP_LOGE(TAG, "PLAYOUT FLUSH failed (%s): %s (x%lu)", str ? str : "unknown",
             esp_err_to_name((esp_err_t)v[0]), (unsigned long)(missed + 1U));
  if (pevt_take(PEVT_SERVO_RESET_FAIL, &last[PEVT_SERVO_RESET_FAIL], v, NULL, &missed))
    ESP_LOGW(TAG, "PLAYOUT SERVO RESET failed: %s (x%lu)",
             esp_err_to_name((esp_err_t)v[0]), (unsigned long)(missed + 1U));
}

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
/* PLAYOUT diagnostics (menuconfig), per status period. s_pd describes what
 * reaches the DAC; FIFO gap counters describe silence inserted on write,
 * which a later FLUSH can discard. Read and cleared by the status line. */
static struct {
  uint32_t max_gap_us;     /* longest time between two running DMA writes */
  uint32_t max_work_us;    /* longest PCM read + output stage of one block */
  uint32_t conceal_blocks; /* blocks with holes filled by silence */
  uint32_t conceal_frames; /* frames of that silence */
  uint32_t silent_blocks;  /* whole block missing: played as silence */
  uint32_t write_fails;    /* running write failed: flush and re-prime */
} s_pd;
#define PD_MAX(field, v)                                                    \
  do {                                                                      \
    const uint32_t pd_v = (uint32_t)(v);                                    \
    if (pd_v > __atomic_load_n(&s_pd.field, __ATOMIC_RELAXED))              \
      __atomic_store_n(&s_pd.field, pd_v, __ATOMIC_RELAXED);                \
  } while (0)
#define PD_ADD(field, v) __atomic_fetch_add(&s_pd.field, (uint32_t)(v), __ATOMIC_RELAXED)

static void status_print_playout_diag(void) {
  uint32_t underruns = 0, tag_faults = 0;
  audio_playout_diag_take(&underruns, &tag_faults);
  const uint32_t gap = __atomic_exchange_n(&s_pd.max_gap_us, 0U, __ATOMIC_RELAXED);
  const uint32_t work = __atomic_exchange_n(&s_pd.max_work_us, 0U, __ATOMIC_RELAXED);
  const uint32_t cb = __atomic_exchange_n(&s_pd.conceal_blocks, 0U, __ATOMIC_RELAXED);
  const uint32_t cf = __atomic_exchange_n(&s_pd.conceal_frames, 0U, __ATOMIC_RELAXED);
  const uint32_t sb = __atomic_exchange_n(&s_pd.silent_blocks, 0U, __ATOMIC_RELAXED);
  const uint32_t wf = __atomic_exchange_n(&s_pd.write_fails, 0U, __ATOMIC_RELAXED);
  uint32_t fifo_gaps = 0, fifo_gap_frames = 0;
  pcm_fifo_diag_take(s.pcm_fifo, &fifo_gaps, &fifo_gap_frames);
  /* Batching normally creates gaps between DMA writes. Use counted faults
   * to trigger warnings; the timing maxima provide context only. */
  const bool bad = underruns || tag_faults || cb || sb || wf || fifo_gaps;
  if (bad) {
    ESP_LOGW(STATUS_TAG,
             "PLAYOUT | gap max %lu us | work max %lu us | underrun %lu | "
             "tag fault %lu | holes %lu blk/%lu fr | silent %lu blk | rewrite %lu | "
             "fifo inserted %lu gaps/%lu fr",
             (unsigned long)gap, (unsigned long)work, (unsigned long)underruns,
             (unsigned long)tag_faults, (unsigned long)cb, (unsigned long)cf,
             (unsigned long)sb, (unsigned long)wf, (unsigned long)fifo_gaps,
             (unsigned long)fifo_gap_frames);
  }
}
#else
#define PD_MAX(field, v) do {} while (0)
#define PD_ADD(field, v) do {} while (0)
#endif

static void status_print_audio_events(void);

static void audio_status_task(void *arg) {
  (void)arg;
  bool stack_headroom_warned = false;
  uint32_t last_rt_pressure[4] = {0};
  int64_t next_status_us = 0;
  while (__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) {
    status_print_audio_events();
    status_print_playout_events();
    timing_snapshot_t snap;
    snapshot_state(&snap);
    const bool active = __atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE) && snap.playing &&
        (snap.stream_type == AUDIO_STREAM_BUFFERED ||
         snap.stream_type == AUDIO_STREAM_REALTIME);
    /* Events wake this task early; the status line keeps its own 2 s
     * deadline (restarting the full wait on every wake hid it for a minute
     * while the servo retuned every second). */
    TickType_t wait = portMAX_DELAY;
    if (active) {
      const int64_t now_us = esp_timer_get_time();
      const int64_t period_us = (int64_t)AP2_STATUS_PERIOD_MS * 1000LL;
      if (next_status_us == 0 || next_status_us - now_us > period_us)
        next_status_us = now_us + period_us;
      const int64_t left_us = next_status_us - now_us;
      wait = left_us > 0 ? pdMS_TO_TICKS((uint32_t)(left_us / 1000LL)) : 0;
    } else {
      next_status_us = 0;
    }
    if (xSemaphoreTake(s.status_wake, wait) == pdTRUE) {
      if (!__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) break;
      continue;
    }
    next_status_us += (int64_t)AP2_STATUS_PERIOD_MS * 1000LL;
    if (next_status_us < esp_timer_get_time()) next_status_us = 0;
    snapshot_state(&snap);
    if (!__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE) || !snap.playing ||
        (snap.stream_type != AUDIO_STREAM_BUFFERED &&
         snap.stream_type != AUDIO_STREAM_REALTIME)) continue;
    const audio_stream_type_t task_stream = snap.stream_type;
    buffered_timing_watchdog(&snap);
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
    status_print_playout_diag();
#endif

    const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
    uint32_t wanted = 0;
    const bool wanted_valid = wanted_rtp_now(&snap, &wanted);
    uint32_t pcm_frames = 0;
    if (wanted_valid && s.pcm_fifo) {
      pcm_frames = pcm_fifo_contiguous_frames(
          s.pcm_fifo, wanted, snap.pcm_generation, PCM_FIFO_FRAMES);
    }

    const bool sync_valid =
        __atomic_load_n(&s.status_sync_valid, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s.status_sync_generation, __ATOMIC_RELAXED) ==
            snap.generation;
    const bool phase_valid = sync_valid &&
        __atomic_load_n(&s.status_phase_valid, __ATOMIC_RELAXED);
    const int32_t sync_us =
        __atomic_load_n(&s.status_sync_us, __ATOMIC_RELAXED);
    const int32_t phase_us =
        __atomic_load_n(&s.status_phase_us, __ATOMIC_RELAXED);
    const int32_t correction_ppm =
        __atomic_load_n(&s.status_servo_ppm, __ATOMIC_RELAXED);

    ptp_clock_snapshot_t ps = {0};
    ptp_clock_get_snapshot(&ps);
    const uint32_t gm_short = (uint32_t)(ps.grandmaster_clock_id & 0xffffffffU);
    const double ptp_delta_ms = (double)ps.raw_filter_delta_ns / 1000000.0;
    char input_bits_buffer[16];
    const char *input_bits = audio_format_input_label(
        snap.format.codec, snap.format.bits_per_sample,
        input_bits_buffer, sizeof(input_bits_buffer));

    if (task_stream == AUDIO_STREAM_BUFFERED) {
      ap2_buffered_fifo_usage_t usage = {0};
      ap2_buffered_fifo_get_usage(s.transport, &usage);
      buffered_control_snapshot_t control = {0};
      buffered_control_snapshot(&control);

      /* Lightweight TCP ingress-rate diagnostic only. Transport internals are
       * intentionally independent from RTP/FLUSH control state. */
      static uint64_t rx_prev_bytes = 0;
      static int64_t rx_prev_us = 0;
      const int64_t rx_now = esp_timer_get_time();
      uint32_t rx_kbps = 0;
      if (usage.bytes_received < rx_prev_bytes) rx_prev_bytes = usage.bytes_received;
      if (rx_prev_us != 0 && rx_now > rx_prev_us)
        rx_kbps = (uint32_t)(((usage.bytes_received - rx_prev_bytes) * 1000ULL) /
                             (uint64_t)(rx_now - rx_prev_us));
      rx_prev_bytes = usage.bytes_received;
      rx_prev_us = rx_now;

      char control_suffix[80] = "";
      if (control.immediate_active) {
        snprintf(control_suffix, sizeof(control_suffix),
                 " | flush=%lu",
                 (unsigned long)control.immediate_target_seq);
      } else if (control.deferred_requests > 0) {
        snprintf(control_suffix, sizeof(control_suffix),
                 " | defer=%lu", (unsigned long)control.deferred_requests);
      }

      if (sync_valid && ps.valid) {
        ESP_LOGI(STATUS_TAG,
                 "%s | %d Hz | %s | sync=%+.2fms | i2s=%+ldppm | ptpD=%+.2fms | "
                 "gm=%08lx%s | fifo=%u/%uKiB rx=%luKB/s | pcm=%.0fms",
                 snap.format.codec, sr, input_bits,
                 (double)sync_us / 1000.0, (long)correction_ppm,
                 ptp_delta_ms, (unsigned long)gm_short, control_suffix,
                 (unsigned)(usage.used_bytes / 1024U),
                 (unsigned)(usage.capacity_bytes / 1024U), (unsigned long)rx_kbps,
                 status_frames_to_ms(pcm_frames, sr));
      } else {
        ESP_LOGI(STATUS_TAG,
                 "%s | %d Hz | %s | sync=%s | i2s=%+ldppm | ptpD=%s | "
                 "gm=%08lx%s | fifo=%u/%uKiB rx=%luKB/s | pcm=%.0fms",
                 snap.format.codec, sr, input_bits,
                 sync_valid ? "valid" : "n/a", (long)correction_ppm,
                 ps.valid ? "valid" : "n/a", (unsigned long)gm_short,
                 control_suffix, (unsigned)(usage.used_bytes / 1024U),
                 (unsigned)(usage.capacity_bytes / 1024U), (unsigned long)rx_kbps,
                 status_frames_to_ms(pcm_frames, sr));
      }
    } else if (task_stream == AUDIO_STREAM_REALTIME) {
      realtime_receiver_usage_t usage = {0};
      realtime_receiver_get_usage(&usage);
      const uint32_t pressure[4] = {usage.dropped_data, usage.dropped_rtx,
                                    usage.missing_backpressure,
                                    usage.received_backpressure};
      if (memcmp(pressure, last_rt_pressure, sizeof(pressure)) != 0) {
        memcpy(last_rt_pressure, pressure, sizeof(pressure));
        /* Status cadence bounds diagnostics even during a packet flood. */
        ESP_LOGW(STATUS_TAG,
                 "ALAC queue pressure: dataDrops=%lu rtxDrops=%lu missingWaits=%lu receivedWaits=%lu",
                 (unsigned long)usage.dropped_data,
                 (unsigned long)usage.dropped_rtx,
                 (unsigned long)usage.missing_backpressure,
                 (unsigned long)usage.received_backpressure);
      }

      bool raw_cursor_valid = false;
      uint32_t raw_cursor = 0;
      taskENTER_CRITICAL(&s.state_mux);
      raw_cursor_valid = s.realtime_stage_cursor_valid &&
                         s.realtime_stage_generation == snap.generation;
      raw_cursor = s.realtime_stage_cursor_rtp;
      taskEXIT_CRITICAL(&s.state_mux);

      uint32_t raw_frames = 0;
      if (raw_cursor_valid && s.realtime_stage_ring) {
        raw_frames = pcm_rtp_ring_contiguous_frames(
            s.realtime_stage_ring, raw_cursor, snap.generation,
            PCM_RTP_RING_FRAMES);
      }
      if (sync_valid && phase_valid && ps.valid) {
        ESP_LOGI(STATUS_TAG,
                 "ALAC | %d Hz | %s | sync=%+.2fms | phase=%+.2fms | i2s=%+ldppm | "
                 "ptpD=%+.2fms | gm=%08lx | epoch=%lu | frameq=%lu/%lu | "
                 "raw=%.0fms | pcm=%.0fms",
                 sr, input_bits, (double)sync_us / 1000.0, (double)phase_us / 1000.0,
                 (long)correction_ppm, ptp_delta_ms,
                 (unsigned long)gm_short, (unsigned long)ps.epoch,
                 (unsigned long)usage.work_queue_depth,
                 (unsigned long)usage.work_queue_capacity,
                 status_frames_to_ms(raw_frames, sr),
                 status_frames_to_ms(pcm_frames, sr));
      } else {
        ESP_LOGI(STATUS_TAG,
                 "ALAC | %d Hz | %s | sync=%s | phase=%s | i2s=%+ldppm | ptpD=%s | "
                 "gm=%08lx | epoch=%lu | frameq=%lu/%lu | raw=%.0fms | pcm=%.0fms",
                 sr, input_bits, sync_valid ? "valid" : "n/a",
                 phase_valid ? "valid" : "n/a",
                 (long)correction_ppm, ps.valid ? "valid" : "n/a",
                 (unsigned long)gm_short, (unsigned long)ps.epoch,
                 (unsigned long)usage.work_queue_depth,
                 (unsigned long)usage.work_queue_capacity,
                 status_frames_to_ms(raw_frames, sr),
                 status_frames_to_ms(pcm_frames, sr));
      }
    }

    /* Keep this diagnostic cheap and rate-limited. On ESP-IDF the task stack
     * watermark is reported in bytes. Warn once while headroom is below 1 KiB
     * and re-arm only after there is comfortable margin again. */
    const UBaseType_t stack_headroom = uxTaskGetStackHighWaterMark(NULL);
    if (stack_headroom < 1024U) {
      if (!stack_headroom_warned) {
        ESP_LOGW(STATUS_TAG, "status task stack headroom low: %u bytes",
                 (unsigned)stack_headroom);
        stack_headroom_warned = true;
      }
    } else if (stack_headroom >= 1536U) {
      stack_headroom_warned = false;
    }
  }

  s.status_task = NULL;
  /* Match the WithCaps creation API so its PSRAM stack is freed on exit. */
  vTaskDeleteWithCaps(NULL);
}

static void audio_status_notify(void) {
  if (s.status_wake) xSemaphoreGive(s.status_wake);
}

/* ---- AUDIO EVT: every event that can be heard as a click, pop or gap ----
 * Producers (playout, AAC processor, ALAC staging) only copy a few numbers
 * into this small ring under a spinlock and wake the status task, which
 * prints them. Nothing here blocks or logs on the I2S feeding path. */
typedef enum {
  AEVT_I2S_STOP = 0,  /* str = reason, a = generation */
  AEVT_PLAY_START,    /* a = generation, b = first RTP, c = stream type */
  AEVT_SILENCE_BEGIN, /* a = RTP: whole playout blocks played as zeros */
  AEVT_SILENCE_END,   /* a = RTP, b = silent blocks */
  AEVT_CONCEAL,       /* a = RTP, b = missing frames in a playout block */
  AEVT_TUNE,          /* a = old ppm, b = new ppm */
  AEVT_AAC_MUTE,      /* a = seq, b = RTP, c = RTP gap (0: first of sequence) */
  AEVT_AAC_LOST,      /* str = why, a = seq */
  AEVT_ALAC_SILENCE,  /* a = RTP, b = frames (recovery window ran out) */
  AEVT_I2S_IDLE,      /* str = reason, a = generation, b = fade-out ms (0: none):
                         I2S keeps running and plays zeros */
  AEVT_AAC_FIRST_PACKET, /* a = seq, b = RTP, c = anchor age ms, d = lead us */
  AEVT_AAC_FIRST_DECODE, /* a = seq, b = lead us, c = elapsed ms, d = late count */
} aevt_kind_t;

#if defined(CONFIG_AIRPLAY_LOG_AUDIO_EVENTS) && CONFIG_AIRPLAY_LOG_AUDIO_EVENTS
typedef struct {
  int64_t t_us;
  const char *str;
  int32_t a, b, c, d;
  uint8_t kind;
} aevt_t;

#define AEVT_CAP 32U
static aevt_t s_aevt[AEVT_CAP];
static uint32_t s_aevt_head;
static uint32_t s_aevt_tail;
static uint32_t s_aevt_dropped;
static portMUX_TYPE s_aevt_mux = portMUX_INITIALIZER_UNLOCKED;

static void aevt_post(aevt_kind_t kind, const char *str, int32_t a, int32_t b,
                      int32_t c, int32_t d) {
  const int64_t now_us = esp_timer_get_time();
  taskENTER_CRITICAL(&s_aevt_mux);
  const uint32_t next = (s_aevt_head + 1U) % AEVT_CAP;
  if (next == s_aevt_tail) {
    s_aevt_dropped++;
  } else {
    aevt_t *e = &s_aevt[s_aevt_head];
    e->t_us = now_us;
    e->str = str;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
    e->kind = (uint8_t)kind;
    s_aevt_head = next;
  }
  taskEXIT_CRITICAL(&s_aevt_mux);
  audio_status_notify();
}

#define AEVT_TUNE_LOG_US 10000000LL

static void status_print_audio_events(void) {
  static int64_t last_stop_us = 0;
  static int64_t last_idle_us = 0;
  static int64_t tune_logged_us = INT64_MIN / 2;
  static int32_t tune_from = 0;
  static int32_t tune_to = 0;
  static uint32_t tune_steps = 0;
  for (;;) {
    aevt_t e = {0};
    uint32_t dropped = 0;
    bool have = false;
    taskENTER_CRITICAL(&s_aevt_mux);
    if (s_aevt_tail != s_aevt_head) {
      e = s_aevt[s_aevt_tail];
      s_aevt_tail = (s_aevt_tail + 1U) % AEVT_CAP;
      have = true;
    }
    dropped = s_aevt_dropped;
    s_aevt_dropped = 0;
    taskEXIT_CRITICAL(&s_aevt_mux);
    if (dropped)
      ESP_LOGW(TAG, "AUDIO EVT: %lu events lost (ring full)",
               (unsigned long)dropped);
    if (!have) {
      const int64_t now_us = esp_timer_get_time();
      if (tune_steps != 0 && now_us - tune_logged_us >= AEVT_TUNE_LOG_US) {
        ESP_LOGI(TAG, "AUDIO EVT %lu ms: I2S clock retune %+ld -> %+ld ppm"
                      " in %lu steps over the last %ld s (LIVE, I2S not stopped)",
                 (unsigned long)(now_us / 1000LL), (long)tune_from,
                 (long)tune_to, (unsigned long)tune_steps,
                 (long)((now_us - tune_logged_us) / 1000000LL));
        tune_logged_us = now_us;
        tune_steps = 0;
      }
      return;
    }

    const unsigned long ms = (unsigned long)(e.t_us / 1000LL);
    switch ((aevt_kind_t)e.kind) {
      case AEVT_I2S_STOP:
        last_stop_us = e.t_us;
        ESP_LOGW(TAG, "AUDIO EVT %lu ms: I2S STOP (clocks halted) reason=%s gen=%ld",
                 ms, e.str ? e.str : "?", (long)e.a);
        break;
      case AEVT_PLAY_START:
        if (e.d)
          ESP_LOGW(TAG, "AUDIO EVT %lu ms: PLAY START gen=%ld rtp=%lu %s"
                        " (I2S kept running, %ld ms of zeros before)",
                   ms, (long)e.a, (unsigned long)(uint32_t)e.b,
                   e.c == AUDIO_STREAM_REALTIME ? "ALAC" : "AAC",
                   last_idle_us ? (long)((e.t_us - last_idle_us) / 1000LL) : -1L);
        else
          ESP_LOGW(TAG, "AUDIO EVT %lu ms: I2S PLAY START gen=%ld rtp=%lu %s"
                        " (%ld ms after last I2S stop)",
                   ms, (long)e.a, (unsigned long)(uint32_t)e.b,
                   e.c == AUDIO_STREAM_REALTIME ? "ALAC" : "AAC",
                   last_stop_us ? (long)((e.t_us - last_stop_us) / 1000LL) : -1L);
        break;
      case AEVT_I2S_IDLE:
        last_idle_us = e.t_us;
        if (e.b > 0)
          ESP_LOGW(TAG, "AUDIO EVT %lu ms: I2S KEEPS RUNNING, playing zeros after a"
                        " %ld ms fade-out (%s) gen=%ld",
                   ms, (long)e.b, e.str ? e.str : "?", (long)e.a);
        else
          ESP_LOGW(TAG, "AUDIO EVT %lu ms: I2S KEEPS RUNNING, playing zeros (%s) gen=%ld",
                   ms, e.str ? e.str : "?", (long)e.a);
        break;
      case AEVT_SILENCE_BEGIN:
        ESP_LOGW(TAG, "AUDIO EVT %lu ms: SILENCE begins while running, no PCM at rtp=%lu",
                 ms, (unsigned long)(uint32_t)e.a);
        break;
      case AEVT_SILENCE_END:
        ESP_LOGW(TAG, "AUDIO EVT %lu ms: SILENCE ends at rtp=%lu after %ld blocks (%.1f ms)",
                 ms, (unsigned long)(uint32_t)e.a, (long)e.b,
                 (double)e.b * (double)AUDIO_PLAYOUT_FRAMES * 1000.0 /
                     (e.c > 0 ? e.c : 44100));
        break;
      case AEVT_CONCEAL:
        ESP_LOGW(TAG, "AUDIO EVT %lu ms: HOLE in block rtp=%lu, %ld frames played as silence",
                 ms, (unsigned long)(uint32_t)e.a, (long)e.b);
        break;
      case AEVT_TUNE:
        /* Phase recovery retunes up to once a second: print the first step,
         * then one summary line per AEVT_TUNE_LOG_US. */
        if (tune_steps == 0 && e.t_us - tune_logged_us >= AEVT_TUNE_LOG_US) {
          ESP_LOGI(TAG, "AUDIO EVT %lu ms: I2S clock retune %+ld -> %+ld ppm"
                        " (step %+ld, LIVE: one register write in %ld us,"
                        " I2S not stopped)",
                   ms, (long)e.a, (long)e.b, (long)(e.b - e.a), (long)e.c);
          tune_logged_us = e.t_us;
        } else {
          if (tune_steps == 0) tune_from = e.a;
          tune_to = e.b;
          ++tune_steps;
        }
        break;
      case AEVT_AAC_MUTE:
        if (e.c == 0)
          ESP_LOGI(TAG, "AUDIO EVT %lu ms: AAC first block of a play sequence muted"
                        " seq=%lu rtp=%lu (will be heard in %ld ms)",
                   ms, (unsigned long)(uint32_t)e.a, (unsigned long)(uint32_t)e.b,
                   (long)e.d);
        else
          ESP_LOGW(TAG, "AUDIO EVT %lu ms: AAC block MUTED after RTP gap %+ld seq=%lu rtp=%lu"
                        " (will be heard in %ld ms)",
                   ms, (long)e.c, (unsigned long)(uint32_t)e.a,
                   (unsigned long)(uint32_t)e.b, (long)e.d);
        break;
      case AEVT_AAC_LOST:
        ESP_LOGW(TAG, "AUDIO EVT %lu ms: AAC block LOST (%s) seq=%lu",
                 ms, e.str ? e.str : "?", (unsigned long)(uint32_t)e.a);
        break;
      case AEVT_ALAC_SILENCE:
        ESP_LOGW(TAG, "AUDIO EVT %lu ms: ALAC block missing, %ld frames of silence rtp=%lu"
                      " (will be heard in %ld ms)",
                 ms, (long)e.b, (unsigned long)(uint32_t)e.a, (long)e.d);
        break;
      case AEVT_AAC_FIRST_PACKET:
        ESP_LOGW(TAG, "START DIAG %lu ms: first packet seq=%lu rtp=%lu;"
                      " anchor time is %+ld ms from now (+ = past);"
                      " lead %+.1f ms (%s)",
                 ms, (unsigned long)(uint32_t)e.a,
                 (unsigned long)(uint32_t)e.b, (long)e.c,
                 (double)e.d / 1000.0,
                 e.d < 0 ? "LATE, will be skipped" : "in time");
        break;
      case AEVT_AAC_FIRST_DECODE:
        ESP_LOGW(TAG, "START DIAG %lu ms: first decoded seq=%lu admission lead %+.1f ms,"
                      " %ld ms after the first packet; before it %lu LATE packets skipped",
                 ms, (unsigned long)(uint32_t)e.a, (double)e.b / 1000.0,
                 (long)e.c, (unsigned long)(uint32_t)e.d);
        break;
    }
  }
}
#else
#define aevt_post(kind, str, a, b, c, d) \
  do { (void)(kind); (void)(str); (void)(a); (void)(b); (void)(c); (void)(d); } while (0)
static void status_print_audio_events(void) {}
#endif

static bool rtp_to_presentation_ns(const timing_snapshot_t *snap, uint32_t rtp,
                                    uint64_t *out_time_ns) {
  if (!snap || !out_time_ns || !timing_clock_ready(snap)) return false;
  const int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
  const int32_t ds = rtp_delta(rtp, snap->anchor_rtp);
  const int64_t dt_ns = ((int64_t)ds * 1000000000LL) / (int64_t)sr;
  const int64_t latency_ns =
      ((int64_t)snap->playout_latency_samples * 1000000000LL) / (int64_t)sr;
  const int64_t t = (int64_t)presentation_anchor_ns(snap) + dt_ns + latency_ns;
  if (t < 0) return false;
  *out_time_ns = (uint64_t)t;
  return true;
}

static int64_t completion_presentation_ns(const timing_snapshot_t *snap,
                                          const audio_playout_completion_t *done) {
  (void)snap;
  return done->done_local_us * 1000LL;
}

/* Pace DMA submission in ESP monotonic presentation time. */
static void wait_until_presentation_ns(const timing_snapshot_t *snap,
                                       uint64_t target_ns) {
  while (__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) {
    timing_snapshot_t current;
    snapshot_state(&current);
    if (!current.playing || !current.anchor_valid || current.timeline_reset_pending ||
        current.generation != snap->generation ||
        current.media_revision != snap->media_revision ||
        current.anchor_clock_id != snap->anchor_clock_id ||
        current.anchor_ptp_ns != snap->anchor_ptp_ns ||
        current.anchor_rtp != snap->anchor_rtp ||
        __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE)) break;
    const uint64_t now = presentation_now_ns(snap);
    if (now >= target_ns) break;
    const uint64_t remain_us = (target_ns - now) / 1000ULL;
    if (remain_us > 250U) {
      /* Wake slightly before the edge. Timer/control tokens are hints only;
       * re-read time and revision after every wake, including a stale token. */
      (void)esp_timer_stop(s.playout_timer);
      if (esp_timer_start_once(s.playout_timer, remain_us - 200U) == ESP_OK) {
        (void)xSemaphoreTake(s.playout_wake, pdMS_TO_TICKS(50));
        (void)esp_timer_stop(s.playout_timer);
      } else if (remain_us > (uint64_t)portTICK_PERIOD_MS * 1000U + 250U) {
        vTaskDelay(1);
      } else {
        esp_rom_delay_us(100);
      }
    } else {
      esp_rom_delay_us(remain_us > 40U ? 20U : 2U);
    }
  }
  (void)esp_timer_stop(s.playout_timer);
}

/* The buffered AAC side now follows Shairport Sync's topology: the TCP task
 * owns only a raw byte FIFO, while this single consumer owns packet framing,
 * FLUSH decisions, timing, decrypt/decode and publication into the PCM FIFO
 * (appended in play order, like Shairport's player queue). There is no
 * compressed-media search or rebind. */
typedef enum {
  PCM_STORE_DROPPED = 0,
  PCM_STORE_PUBLISHED,
} pcm_store_result_t;

static pcm_store_result_t pcm_store_with_backpressure(
    uint32_t rtp, const float *pcm, size_t frames, int channels,
    uint32_t pcm_generation, uint32_t packet_seq, uint32_t stream_epoch) {
  if (!pcm || channels != 2 || frames == 0 || frames > PCM_RTP_SLOT_FRAMES)
    return PCM_STORE_DROPPED;

  while (__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE)) {
    xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
    xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (!__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE) || snap.stream_type != AUDIO_STREAM_BUFFERED ||
        snap.pcm_generation != pcm_generation || snap.timeline_reset_pending ||
        !ap2_buffered_fifo_epoch_is_current(s.transport, stream_epoch) ||
        buffered_control_packet_flushed_locked(packet_seq)) {
      xSemaphoreGive(s.publish_mutex);
      xSemaphoreGive(s_buffered_control.mutex);
      return PCM_STORE_DROPPED;
    }
    /* The processor decodes a block only when its lead is inside the PCM
     * window (lead < AP2_BUFFERED_LEAD_MS), so there is room for it now:
     * publish it at its RTP address at once, like Shairport's
     * player_put_packet() right after decoding. Never park decoded samples
     * here waiting for play/anchor/PTP state: a pause or anchor update that
     * arrives while the block is being decoded must not hold it (and then
     * drop it on resume, which left a one-block hole in the music). The
     * lead/late checks apply only while the timeline is valid. */
    uint32_t wanted = 0;
    const bool wanted_valid = snap.playing && snap.anchor_valid &&
                              timing_clock_ready(&snap) &&
                              wanted_rtp_now(&snap, &wanted);
    if (wanted_valid) {
      const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
      const int32_t max_lead =
          (int32_t)(((int64_t)sr * AP2_BUFFERED_LEAD_MS) / 1000LL);
      if (rtp_delta(rtp, wanted) > max_lead) {
        xSemaphoreGive(s.publish_mutex);
        xSemaphoreGive(s_buffered_control.mutex);
        ap2_buffered_fifo_wait(s.transport, AP2_NO_TIMING_WAIT_MS);
        continue;
      }
      if (rtp_delta(rtp + (uint32_t)frames, wanted) <= 0) {
        xSemaphoreGive(s.publish_mutex);
        xSemaphoreGive(s_buffered_control.mutex);
        return PCM_STORE_DROPPED;
      }
    }

    bool stored = pcm_fifo_write(s.pcm_fifo, rtp, pcm, frames,
                                 pcm_generation, wanted, wanted_valid);
    if (stored && !ap2_buffered_fifo_epoch_is_current(s.transport, stream_epoch)) {
      /* Disconnect/reconnect during the copy must not leave old-stream PCM.
       * Publication mutex still excludes any replacement producer. */
      pcm_fifo_truncate(s.pcm_fifo, rtp, pcm_generation);
      stored = false;
    }
    xSemaphoreGive(s.publish_mutex);
    xSemaphoreGive(s_buffered_control.mutex);
    if (stored) {
      playout_wake();
      return PCM_STORE_PUBLISHED;
    }
    vTaskDelay(1);
  }
  return PCM_STORE_DROPPED;
}

/* AutoMix may send a stretch of timeline again (new sequence numbers, RTP
 * going back by up to ~0.4 s) while the earlier version still waits in the
 * PCM FIFO. Like the former RTP-addressed ring, the newer packets win: drop
 * the published PCM from rtp on and let the processor append the new audio
 * there. Shairport keeps the old blocks instead (its replacement path,
 * get_revised_seqno + clear_buffers_from, is commented out). Returns false
 * when rtp is too close to the playhead (or there is none): skip the packet
 * as before. */
static bool pcm_rewind_for_resend(uint32_t rtp, uint32_t pcm_generation,
                                  uint32_t packet_seq, uint32_t stream_epoch) {
  bool rewound = false;
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  timing_snapshot_t snap;
  snapshot_state(&snap);
  uint32_t wanted = 0;
  if (snap.stream_type == AUDIO_STREAM_BUFFERED &&
      snap.pcm_generation == pcm_generation && !snap.timeline_reset_pending &&
      ap2_buffered_fifo_epoch_is_current(s.transport, stream_epoch) &&
      !buffered_control_packet_flushed_locked(packet_seq) && snap.playing &&
      snap.anchor_valid && timing_clock_ready(&snap) &&
      wanted_rtp_now(&snap, &wanted)) {
    const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
    const int32_t min_lead =
        (int32_t)(((int64_t)sr * AP2_REPLACE_MIN_LEAD_MS) / 1000LL);
    if (rtp_delta(rtp, wanted) >= min_lead) {
      pcm_fifo_truncate(s.pcm_fifo, rtp, pcm_generation);
      rewound = true;
    }
  }
  xSemaphoreGive(s.publish_mutex);
  xSemaphoreGive(s_buffered_control.mutex);
  return rewound;
}

/* Pure centring decision (host-tested). Returns the frequency step in ppm to
 * add to the current I2S correction, or 0. `slope_us_s` is the measured sync
 * slope over the last window while the clock was NOT being retuned. */
/* Learned clock correction, remembered across sessions until reboot. The
 * needed ppm is the ESP crystal error plus the sender's: keyed by the anchor
 * clock (the sender's PTP identity); an unknown sender starts from the most
 * recent value, which still carries the ESP crystal part. Playout task only. */
#define AP2_LEARNED_CLOCKS 4U
static struct {
  uint64_t clock_id;
  int32_t ppm;
} s_learned[AP2_LEARNED_CLOCKS];
static uint32_t s_learned_next;
static int32_t s_learned_last;

static void learned_ppm_store(uint64_t clock_id, int32_t ppm) {
  s_learned_last = ppm;
  if (clock_id == 0) return;
  for (uint32_t i = 0; i < AP2_LEARNED_CLOCKS; ++i) {
    if (s_learned[i].clock_id == clock_id) {
      s_learned[i].ppm = ppm;
      return;
    }
  }
  s_learned[s_learned_next].clock_id = clock_id;
  s_learned[s_learned_next].ppm = ppm;
  s_learned_next = (s_learned_next + 1U) % AP2_LEARNED_CLOCKS;
}

static int32_t learned_ppm_lookup(uint64_t clock_id) {
  for (uint32_t i = 0; clock_id != 0 && i < AP2_LEARNED_CLOCKS; ++i) {
    if (s_learned[i].clock_id == clock_id) return s_learned[i].ppm;
  }
  return s_learned_last;
}

static int32_t servo_center_step(int32_t sync_us, double slope_us_s) {
  double wanted_slope = 0.0;
  if (sync_us > AP2_CENTER_QUIET_US || sync_us < -AP2_CENTER_QUIET_US) {
    wanted_slope = -(double)sync_us / AP2_CENTER_TIME_S;
    if (wanted_slope > AP2_CENTER_MAX_SLOPE_US_S)
      wanted_slope = AP2_CENTER_MAX_SLOPE_US_S;
    if (wanted_slope < -AP2_CENTER_MAX_SLOPE_US_S)
      wanted_slope = -AP2_CENTER_MAX_SLOPE_US_S;
  }
  double step = AP2_CENTER_GAIN * (wanted_slope - slope_us_s); /* +1 ppm -> +1 us/s */
  if (step > AP2_CENTER_MAX_STEP_PPM) step = AP2_CENTER_MAX_STEP_PPM;
  if (step < -AP2_CENTER_MAX_STEP_PPM) step = -AP2_CENTER_MAX_STEP_PPM;
  const int32_t out = (int32_t)(step >= 0.0 ? step + 0.5 : step - 0.5);
  /* Below the physical retune threshold a step would never be applied. */
  if (out < AP2_PID_MIN_TUNE_PPM && out > -AP2_PID_MIN_TUNE_PPM) return 0;
  return out;
}


/* Short nonblocking commit. No socket read, decode or wait occurs under the
 * control mutex. A newer command requires classifying this same FIFO head
 * again, instead of applying a decision from the previous timeline. */
static esp_err_t buffered_consume_head(const ap2_buffered_fifo_head_t *head,
                                        const buffered_packet_decision_t *decision,
                                        uint8_t *dst) {
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (s_buffered_control.revision == decision->revision) {
    err = dst ? ap2_buffered_fifo_read_head(s.transport, head, dst, AP2_PACKET_MAX)
              : ap2_buffered_fifo_discard_head(s.transport, head);
  }
  xSemaphoreGive(s_buffered_control.mutex);
  return err;
}

static bool buffered_select_format(
    const airplay_audio_format_t *format,
    const ap2_buffered_fifo_head_t *head,
    const buffered_packet_decision_t *decision, uint32_t packet_rtp,
    bool old_sequence, uint32_t pcm_generation) {
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  timing_snapshot_t snap;
  snapshot_state(&snap);
  bool valid = s_buffered_control.revision == decision->revision &&
      snap.stream_type == AUDIO_STREAM_BUFFERED &&
      snap.pcm_generation == pcm_generation &&
      __atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE) &&
      ap2_buffered_fifo_epoch_is_current(s.transport, head->stream_epoch);
  const bool rate_changed = snap.format.sample_rate != format->sample_rate;
  const bool rebase = rate_changed && old_sequence && snap.anchor_valid &&
      !snap.timeline_reset_pending;
  if (valid && rate_changed && old_sequence && snap.playing &&
      snap.anchor_valid && !snap.timeline_reset_pending) {
    timing_snapshot_t boundary = snap;
    if (!rebase) boundary.format.sample_rate = format->sample_rate;
    uint64_t presentation_ns = 0;
    if (!rtp_to_presentation_ns(&boundary, packet_rtp, &presentation_ns) ||
        (int64_t)presentation_ns - esp_timer_get_time() * 1000LL > 20000000LL)
      valid = false;
  }
  if (valid) {
    if (rebase) {
      const int64_t delta_ns =
          (int64_t)rtp_delta(packet_rtp, snap.anchor_rtp) * 1000000000LL /
          snap.format.sample_rate;
      const int64_t remote_ns = (int64_t)snap.anchor_ptp_ns + delta_ns;
      const int64_t local_ns = (int64_t)snap.anchor_local_ns + delta_ns;
      if (remote_ns < 0 || (snap.anchor_local_valid && local_ns < 0)) {
        valid = false;
      } else {
        taskENTER_CRITICAL(&s.state_mux);
        s.anchor_rtp = packet_rtp;
        s.anchor_ptp_ns = (uint64_t)remote_ns;
        if (s.anchor_local_valid) s.anchor_local_ns = (uint64_t)local_ns;
        s.media_revision = next_generation(s.media_revision);
        taskEXIT_CRITICAL(&s.state_mux);
      }
    }
  }
  if (valid) {
    if (rate_changed) {
      __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
      (void)reset_buffered_pcm_store();
    }
    audio_format_t next = {
        .sample_rate = format->sample_rate,
        .channels = 2,
        .bits_per_sample = format->bits_per_sample,
        .frame_size = format->frame_size,
    };
    strlcpy(next.codec, format->codec, sizeof(next.codec));
    audio_receiver_set_format(&next);
  }
  xSemaphoreGive(s.publish_mutex);
  xSemaphoreGive(s_buffered_control.mutex);
  if (valid && rebase)
    ESP_LOGI(TAG, "AUDIO RATE ANCHOR %d -> %d Hz, rebased at RTP=%" PRIu32,
             snap.format.sample_rate, format->sample_rate, packet_rtp);
  return valid;
}

static void ap2_buffered_processor_task(void *arg) {
  (void)arg;
  aac_decoder_t *decoder = NULL;
  alac_decoder_t *alac = NULL;
  uint32_t decoder_format_generation = 0;
  uint32_t current_ssrc = 0;
  bool playback_announced = false;
  uint32_t seen_stream_epoch = 0;
  bool authenticated_valid = false;
  ap2_buffered_fifo_head_t authenticated_head = {0};
  int authenticated_len = 0;

  /* Shairport's packets_played_in_this_sequence / expected_timestamp. A play
   * stop starts a new sequence; deferred FLUSH does not reset it, so the first
   * surviving packet is recognised by its RTP timestamp discontinuity. */
  uint32_t packets_played_in_sequence = 0;
  uint32_t expected_timestamp = 0;

  bool play_enabled = false;
  uint32_t seen_pcm_generation = 0;
  bool very_early_signalled = false;
  int64_t early_hold_since_us = 0;

  uint32_t empty_blocks = 0;
  uint32_t decrypt_failures = 0;
  uint32_t decode_failures = 0;
  uint32_t too_old_skipped = 0;
  uint32_t resend_replaced = 0;
  uint32_t unsupported_skipped = 0;
#if defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
  /* Every packet in TCP order (before FLUSH decisions): a sequence jump means
   * the sender skipped packets; a timestamp jump with consecutive sequence
   * numbers means it moved its timeline. */
  bool am_seq_valid = false;
  uint32_t am_last_seq = 0;
  uint32_t am_last_published_seq = 0;
  uint32_t am_late_dropped = 0;
#endif
  uint32_t unrecognised_ssrc_skipped = 0;
  buffered_packet_t packet = {0};
#if defined(CONFIG_AIRPLAY_LOG_AUDIO_EVENTS) && CONFIG_AIRPLAY_LOG_AUDIO_EVENTS
  /* START DIAG: how the first packets of a play sequence arrive relative to
   * their presentation time. Only enqueue numbers here; formatting and log
   * I/O belong to the status task so diagnostics cannot make a packet late. */
  bool sd_active = false;        /* collecting for the current sequence */
  bool sd_first_logged = false;
  uint32_t sd_dropped = 0;       /* packets consumed as late before the first decode */
  int64_t sd_first_us = 0;       /* local time of the first packet seen */
#endif

  /* Immutable for this producer lifetime; SETUP owns key publication. */
  audio_encrypt_t session_encrypt;
  taskENTER_CRITICAL(&s.state_mux);
  session_encrypt = s.encrypt;
  taskEXIT_CRITICAL(&s.state_mux);
  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_AAC_PROCESSOR,
                                    xPortGetCoreID(), AP2_DECODE_PRIORITY, 0U);

  while (__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE)) {
    timing_snapshot_t snap;
    snapshot_state(&snap);

    /* Immediate FLUSH invalidates PCM synchronously on the control core.
     * Observe its generation even if stop/resume both happened between loop
     * iterations. Ordinary pause preserves the decoded PCM in the FIFO (Apple
     * does not resend it), so a pause is not a new play sequence: the next
     * packet continues the RTP timeline and is not muted. A real gap after
     * the pause is still muted by the expected_timestamp check; FLUSH, seek
     * and track change change the PCM generation and reset here. */
    if (seen_pcm_generation != snap.pcm_generation) {
      packets_played_in_sequence = 0;
      expected_timestamp = 0;
      seen_pcm_generation = snap.pcm_generation;
#if defined(CONFIG_AIRPLAY_LOG_AUDIO_EVENTS) && CONFIG_AIRPLAY_LOG_AUDIO_EVENTS
      sd_active = false; /* next packet starts diagnostics for this PCM epoch */
#endif
    }
    if (!play_enabled && snap.playing) {
      AP_LOGD(TAG, "Play started; FIFO head retained for reclassification.");
      early_hold_since_us = 0;
      very_early_signalled = false;
    }
    play_enabled = snap.playing;

    if (!play_enabled && !buffered_control_must_drain()) {
      ap2_buffered_fifo_wait(s.transport, AP2_NO_TIMING_WAIT_MS);
      continue;
    }
    ap2_buffered_fifo_head_t head;
    const esp_err_t peek_err = ap2_buffered_fifo_peek_head(
        s.transport, AP2_PACKET_MAX, &head);
    if (peek_err != ESP_OK) {
      if (__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE)) ap2_buffered_fifo_wait(s.transport, AP2_NO_TIMING_WAIT_MS);
      continue;
    }
    packet.seq = read_be32_local(head.header) & 0x007fffffU;
    packet.rtp = read_be32_local(head.header + 4);
    packet.ssrc = read_be32_local(head.header + 8);
    packet.len = (size_t)head.wire_len - 2U;
    packet.stream_epoch = head.stream_epoch;

    if (seen_stream_epoch != 0 && packet.stream_epoch != seen_stream_epoch) {
      if (decoder) { aac_decoder_destroy(decoder); decoder = NULL; }
      if (alac) { alac_decoder_destroy(alac); alac = NULL; }
      decoder_format_generation = 0;
      current_ssrc = 0;
      playback_announced = false;
      packets_played_in_sequence = 0;
      expected_timestamp = 0;
#if defined(CONFIG_AIRPLAY_LOG_AUDIO_EVENTS) && CONFIG_AIRPLAY_LOG_AUDIO_EVENTS
      sd_active = false;
#endif
      /* New connection: stop I2S before the old PCM is invalidated. */
      __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
      media_control_wake();
      xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
      xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
      (void)reset_buffered_pcm_store();
      xSemaphoreGive(s.publish_mutex);
      xSemaphoreGive(s_buffered_control.mutex);
    }
    seen_stream_epoch = packet.stream_epoch;
#if defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
    if (!am_seq_valid || packet.seq != am_last_seq) {
      if (am_seq_valid && seq23_delta(packet.seq, am_last_seq) != 1) {
        buffered_control_snapshot_t fc = {0};
        buffered_control_snapshot(&fc);
        ESP_LOGW(TAG, "AUTOMIX SEQ JUMP %" PRIu32 " -> %" PRIu32 " (%+" PRId32
                      " packets) rtp=%" PRIu32 " | flush imm=%d deferred=%" PRIu32,
                 am_last_seq, packet.seq, seq23_delta(packet.seq, am_last_seq),
                 packet.rtp, fc.immediate_active ? 1 : 0,
                 (uint32_t)fc.deferred_requests);
      }
      am_last_seq = packet.seq;
      am_seq_valid = true;
    }
#endif

    /* FLUSH state is applied to the one sequentially-held packet before any
     * timing/decrypt/decode work. Re-run this on every pass so a control command
     * that arrives while an early packet is held acts on that exact packet. */
    buffered_packet_decision_t decision = {0};
    if (buffered_control_classify(&packet, &head, &decision) != ESP_OK) continue;
    if (decision.immediate_completed) {
      AP_LOGI(TAG, "AAC FLUSH complete target=%" PRIu32 " at seq=%" PRIu32 "%s",
               decision.immediate_target_seq, packet.seq,
               decision.immediate_overshoot ? " overshoot" : "");
    }
    if (decision.drop) {
      (void)buffered_consume_head(&head, &decision, NULL);
      early_hold_since_us = 0;
      continue;
    }

    /* Shairport never gives an unrecognised SSRC to the timing/decoder path;
     * such a block is simply consumed. ESP32 additionally skips recognised
     * encodings for which this build has no decoder. */
    const airplay_audio_format_t *payload_format =
        airplay_audio_format_by_ssrc(packet.ssrc);
    if (!payload_format) {
      (void)buffered_consume_head(&head, &decision, NULL);
      early_hold_since_us = 0;
      if ((++unrecognised_ssrc_skipped & 0xFFU) == 1U)
        ESP_LOGW(TAG,
                 "block seq=%lu with unrecognised SSRC 0x%08lx skipped (%lu so far)",
                 (unsigned long)packet.seq, (unsigned long)packet.ssrc,
                 (unsigned long)unrecognised_ssrc_skipped);
      continue;
    }

    if (!airplay_audio_format_supported(payload_format, AUDIO_STREAM_BUFFERED)) {
      (void)buffered_consume_head(&head, &decision, NULL);
      early_hold_since_us = 0;
      if ((++unsupported_skipped & 0xFFU) == 1U)
        ESP_LOGW(TAG, "unsupported payload format \"%s\"; skipping (%lu blocks)",
                 payload_format->name,
                 (unsigned long)unsupported_skipped);
      continue;
    }

    if (packet.ssrc != current_ssrc) {
      if (!authenticated_valid ||
          authenticated_head.read_serial != head.read_serial ||
          authenticated_head.stream_epoch != head.stream_epoch) {
        if (ap2_buffered_fifo_copy_head(s.transport, &head, s.buffered_packet,
                                         AP2_PACKET_MAX) != ESP_OK) continue;
        authenticated_len = audio_crypto_decrypt_buffered(
            &session_encrypt, s.buffered_packet, packet.len,
            s.decrypt_buf + AAC_DECODER_INPUT_HEADROOM, AP2_PACKET_MAX);
        authenticated_head = head;
        authenticated_valid = authenticated_len > 0;
        if (!authenticated_valid) {
          (void)buffered_consume_head(&head, &decision, NULL);
          if (authenticated_len < 0 && (++decrypt_failures & 0x3fU) == 1U)
            ESP_LOGW(TAG, "AUDIO FORMAT rejected: unauthenticated %s SSRC "
                          "0x%08" PRIx32 " at seq=%" PRIu32,
                     payload_format->name, packet.ssrc, packet.seq);
          continue;
        }
      }
      if (!buffered_select_format(payload_format, &head, &decision, packet.rtp,
                                    packets_played_in_sequence != 0,
                                    seen_pcm_generation)) {
        ap2_buffered_fifo_wait(s.transport, AP2_NO_TIMING_WAIT_MS);
        continue;
      }
      const airplay_audio_format_t *previous =
          airplay_audio_format_by_ssrc(current_ssrc);
      ESP_LOGI(TAG, "AUDIO FORMAT %s -> %s; SSRC 0x%08" PRIx32
                    " -> 0x%08" PRIx32 ", seq=%" PRIu32 ", block=%d frames",
               previous ? previous->name : "SETUP/initial",
               payload_format->name, current_ssrc, packet.ssrc,
               packet.seq, payload_format->frame_size);
      current_ssrc = packet.ssrc;
      playback_announced = false;
      unsupported_skipped = 0;
      if (decoder) { aac_decoder_destroy(decoder); decoder = NULL; }
      if (alac) { alac_decoder_destroy(alac); alac = NULL; }
      decoder_format_generation = 0;
    }

    snapshot_state(&snap);
    /* Control may have stopped/resumed between loop-head and classification.
     * Reconcile history before the first AU of the newly visible PCM epoch. */
    if (seen_pcm_generation != snap.pcm_generation) {
      packets_played_in_sequence = 0;
      expected_timestamp = 0;
      seen_pcm_generation = snap.pcm_generation;
#if defined(CONFIG_AIRPLAY_LOG_AUDIO_EVENTS) && CONFIG_AIRPLAY_LOG_AUDIO_EVENTS
      sd_active = false;
#endif
    }
    if (snap.stream_type != AUDIO_STREAM_BUFFERED || !snap.playing ||
        !snap.anchor_valid || snap.timeline_reset_pending ||
        !timing_clock_ready(&snap)) {
      ap2_buffered_fifo_wait(s.transport, AP2_NO_TIMING_WAIT_MS);
      continue;
    }

    uint32_t wanted = 0;
    if (!wanted_rtp_now(&snap, &wanted)) {
      ap2_buffered_fifo_wait(s.transport, AP2_NO_TIMING_WAIT_MS);
      continue;
    }

    const uint32_t frame_samples = snap.format.frame_size > 0
                                       ? (uint32_t)snap.format.frame_size
                                       : 1024U;
    const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
    const int32_t max_lead =
        (int32_t)(((int64_t)sr * AP2_BUFFERED_LEAD_MS) / 1000LL);
    const int32_t lead = rtp_delta(packet.rtp, wanted);
#if defined(CONFIG_AIRPLAY_LOG_AUDIO_EVENTS) && CONFIG_AIRPLAY_LOG_AUDIO_EVENTS
    if (!sd_active && packets_played_in_sequence == 0) {
      /* A new play sequence (start, track change, FLUSH, new connection). */
      sd_active = true;
      sd_first_logged = false;
      sd_dropped = 0;
      sd_first_us = 0;
    }
    if (sd_active && !sd_first_logged) {
      /* > 0: the anchor's presentation time is already in the past. */
      const int64_t since_anchor_ms =
          ((int64_t)presentation_now_ns(&snap) -
           (int64_t)presentation_anchor_ns(&snap)) / 1000000LL;
      sd_first_logged = true;
      sd_first_us = esp_timer_get_time();
      aevt_post(AEVT_AAC_FIRST_PACKET, NULL, (int32_t)packet.seq,
                (int32_t)packet.rtp, (int32_t)since_anchor_ms,
                (int32_t)((int64_t)lead * 1000000LL / sr));
    }
    if (sd_active && lead < 0) {
      sd_dropped++;
    }
#endif

    /* Shairport only decrypts a buffered packet if its presentation start is
     * not already late. Consume late compressed packets without decoding; the
     * next surviving packet's RTP gap will cause the AAC block to be muted. */
    if (lead < 0) {
      (void)buffered_consume_head(&head, &decision, NULL);
      early_hold_since_us = 0;
      continue;
    }

    /* Decode only inside desired decoded-buffer length + 0.1 s. The current
     * packet remains at the raw FIFO head while waiting, so AutoMix can park
     * 90-120 seconds of compressed audio cheaply in the large FIFO. */
    if (lead >= max_lead) {
      const int32_t very_early_threshold =
          (int32_t)(((int64_t)sr * (AP2_PCM_TARGET_MS + 200U)) / 1000LL);
      if (!very_early_signalled && lead > very_early_threshold) {
        very_early_signalled = true;
        early_hold_since_us = esp_timer_get_time();
        AP_LOGW(TAG,
                 "AAC block seq=%lu rtp=%lu is %.1f s early; holding compressed packet",
                 (unsigned long)packet.seq, (unsigned long)packet.rtp,
                 (double)lead / (double)sr);
      }
      uint32_t wait_ms =
          (uint32_t)(((uint64_t)frame_samples * 2000ULL) / (uint64_t)sr);
      if (wait_ms == 0U) wait_ms = 1U;
      ap2_buffered_fifo_wait(s.transport, wait_ms);
      continue;
    }

    if (very_early_signalled) {
      if (early_hold_since_us != 0) {
        AP_LOGI(TAG, "early AAC block seq=%lu released after %.1f s",
                 (unsigned long)packet.seq,
                 (double)(esp_timer_get_time() - early_hold_since_us) / 1e6);
      }
      very_early_signalled = false;
      early_hold_since_us = 0;
    }

    /* Exact Shairport sequence semantics: mute the first AAC packet in a play
     * sequence, and also mute the first packet after an RTP timestamp gap.
     * A sequence-number gap alone is diagnostic; it does not rebuild or reset
     * the AAC decoder. */
    int32_t timestamp_difference = 0;
    const bool is_aac = payload_format->codec_type == 4;
    bool mute = is_aac && packets_played_in_sequence == 0;
    if (packets_played_in_sequence != 0) {
      timestamp_difference = rtp_delta(packet.rtp, expected_timestamp);
      if (timestamp_difference != 0) {
        mute = is_aac;
#if defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
        {
          buffered_control_snapshot_t fc = {0};
          buffered_control_snapshot(&fc);
          uint32_t now_rtp = 0;
          const bool now_valid = wanted_rtp_now(&snap, &now_rtp);
          ESP_LOGW(TAG, "AUTOMIX RTP JUMP %+" PRId32 " frames (%+.1f ms): last "
                        "published seq=%" PRIu32 " -> seq=%" PRIu32 " (%+" PRId32
                        " packets), expected rtp=%" PRIu32 " got %" PRIu32
                        " | lead %ld ms | flush imm=%d deferred=%" PRIu32
                        " | too old so far %" PRIu32 ", late dropped %" PRIu32,
                   timestamp_difference,
                   (double)timestamp_difference * 1000.0 / sr,
                   am_last_published_seq, packet.seq,
                   seq23_delta(packet.seq, am_last_published_seq),
                   expected_timestamp, packet.rtp,
                   now_valid ? (long)((int64_t)rtp_delta(packet.rtp, now_rtp) *
                                      1000LL / sr) : -1L,
                   fc.immediate_active ? 1 : 0, (uint32_t)fc.deferred_requests,
                   too_old_skipped, am_late_dropped);
        }
#endif
        AP_LOGD(TAG,
                 "AAC timestamp discontinuity seq=%lu actual=%lu expected=%lu diff=%ld",
                 (unsigned long)packet.seq, (unsigned long)packet.rtp,
                 (unsigned long)expected_timestamp,
                 (long)timestamp_difference);
      }
      if (timestamp_difference < 0 &&
          (uint32_t)(-(int64_t)timestamp_difference) > frame_samples &&
          pcm_rewind_for_resend(packet.rtp, snap.pcm_generation, packet.seq,
                                packet.stream_epoch)) {
        /* Re-sent timeline: this packet and the ones after it replace the
         * PCM from packet.rtp on. The block is still muted (the decoder's
         * history is from the other timeline). */
        ++resend_replaced;
#if defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
        ESP_LOGW(TAG, "AUTOMIX RESEND seq=%" PRIu32 " rtp=%" PRIu32
                      " (%+" PRId32 " frames): replaces PCM from here (%" PRIu32
                      " so far)",
                 packet.seq, packet.rtp, timestamp_difference, resend_replaced);
#else
        AP_LOGI(TAG, "re-sent timeline at seq=%lu (%ld frames) replaces PCM",
                (unsigned long)packet.seq, (long)timestamp_difference);
#endif
      } else if (timestamp_difference < 0 &&
          (uint32_t)(-(int64_t)timestamp_difference) > frame_samples) {
        (void)buffered_consume_head(&head, &decision, NULL);
#if defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
        ESP_LOGW(TAG, "AUTOMIX TOO OLD seq=%" PRIu32 " rtp=%" PRIu32
                      " (%+" PRId32 " frames before expected %" PRIu32 ") dropped",
                 packet.seq, packet.rtp, timestamp_difference, expected_timestamp);
#endif
        if ((++too_old_skipped & 0x3FU) == 1U)
          AP_LOGI(TAG,
                   "skipping block seq=%lu because it is too old (%ld frames; %lu so far)",
                   (unsigned long)packet.seq, (long)timestamp_difference,
                   (unsigned long)too_old_skipped);
        continue;
      }
    }

    if ((!decoder && !alac) ||
        decoder_format_generation != snap.format_generation) {
      if (decoder) aac_decoder_destroy(decoder);
      if (alac) alac_decoder_destroy(alac);
      decoder = NULL;
      alac = NULL;
      if (is_aac) {
        aac_decoder_config_t cfg = {
            .sample_rate = payload_format->sample_rate,
            .channels = payload_format->channels,
            .bits_per_sample = payload_format->bits_per_sample,
        };
        decoder = aac_decoder_create(&cfg);
      } else {
        alac_decoder_config_t cfg = {
            .sample_rate = payload_format->sample_rate,
            .channels = payload_format->channels,
            .bits_per_sample = payload_format->bits_per_sample,
            .frame_size = payload_format->frame_size,
        };
        alac = alac_decoder_create(&cfg);
      }
      if (!decoder && !alac) {
        vTaskDelay(1);
        continue; /* keep the FIFO head and retry */
      }
      decoder_format_generation = snap.format_generation;
    }

    if (buffered_consume_head(&head, &decision, s.buffered_packet) != ESP_OK)
      continue; /* new control command or transport epoch: inspect again */

    const bool already_authenticated = authenticated_valid &&
        authenticated_head.read_serial == head.read_serial &&
        authenticated_head.stream_epoch == head.stream_epoch;
    const int dec_len = already_authenticated ? authenticated_len :
        audio_crypto_decrypt_buffered(
            &session_encrypt, s.buffered_packet, packet.len,
            s.decrypt_buf + AAC_DECODER_INPUT_HEADROOM, AP2_PACKET_MAX);
    authenticated_valid = false;
    if (dec_len == 0) {
      if ((++empty_blocks & 0x3FU) == 1U)
        AP_LOGI(TAG, "%s empty block seq=%lu len=%u (skipped, total %lu)",
                 payload_format->codec, (unsigned long)packet.seq, (unsigned)packet.len,
                 (unsigned long)empty_blocks);
      continue;
    }
    if (dec_len < 0) {
      aevt_post(AEVT_AAC_LOST, "decrypt-error", (int32_t)packet.seq, 0, 0, 0);
      if ((++decrypt_failures & 0x3FU) == 1U)
        AP_LOGW(TAG, "%s decrypt/auth failed seq=%lu rtp=%lu len=%u (%lu total)",
                 payload_format->codec, (unsigned long)packet.seq, (unsigned long)packet.rtp,
                 (unsigned)packet.len, (unsigned long)decrypt_failures);
      continue;
    }

    int channels = 2;
    int frames;
    if (is_aac) {
      aac_decode_info_t info = {0};
      frames = aac_decoder_decode(
          decoder, s.decrypt_buf + AAC_DECODER_INPUT_HEADROOM,
          (size_t)dec_len, s.pcm_f32, AP2_PCM_CAPACITY_FRAMES, &info);
      channels = info.channels;
    } else {
      alac_decode_info_t info = {0};
      frames = alac_decoder_decode_float(
          alac, s.decrypt_buf + AAC_DECODER_INPUT_HEADROOM,
          (size_t)dec_len, s.pcm_f32, AP2_PCM_CAPACITY_FRAMES, &info);
      channels = info.channels;
    }
    if (frames <= 0 || frames > payload_format->frame_size || channels != 2) {
      aevt_post(AEVT_AAC_LOST, "decode-error", (int32_t)packet.seq, 0, 0, 0);
      if ((++decode_failures & 0x3FU) == 1U)
        AP_LOGW(TAG, "%s decode failed at seq=%lu (%lu total); packet dropped",
                 payload_format->codec, (unsigned long)packet.seq,
                 (unsigned long)decode_failures);
      continue;
    }

#if defined(CONFIG_AIRPLAY_LOG_AUDIO_EVENTS) && CONFIG_AIRPLAY_LOG_AUDIO_EVENTS
    if (sd_active) {
      sd_active = false;
      aevt_post(AEVT_AAC_FIRST_DECODE, NULL, (int32_t)packet.seq,
                (int32_t)((int64_t)lead * 1000000LL / sr),
                (int32_t)((esp_timer_get_time() - sd_first_us) / 1000LL),
                (int32_t)sd_dropped);
    }
#endif

    /* Muting is after AAC decode (so MDCT/codec history advances) but before
     * EQ, matching Shairport's intent: the decoder sees the real first block,
     * while the downstream audio chain receives silence for its duration. */
    if (mute) {
      {
        uint32_t now_rtp = 0;
        const int sr_ev = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
        const int32_t lead_ms = wanted_rtp_now(&snap, &now_rtp)
            ? (int32_t)(((int64_t)rtp_delta(packet.rtp, now_rtp) * 1000LL) / sr_ev)
            : -1;
        aevt_post(AEVT_AAC_MUTE, NULL, (int32_t)packet.seq, (int32_t)packet.rtp,
                  timestamp_difference, lead_ms);
      }
      memset(s.pcm_f32, 0,
             (size_t)frames * (size_t)channels * sizeof(float));
    }
    const pcm_store_result_t stored = pcm_store_with_backpressure(
        packet.rtp, s.pcm_f32, (size_t)frames, channels,
        snap.pcm_generation, packet.seq, packet.stream_epoch);
    if (stored == PCM_STORE_PUBLISHED) {
      if (!playback_announced) {
        if (payload_format->channels == 2)
          audio_log_stereo_playback(TAG, payload_format->codec,
                                    payload_format->sample_rate,
                                    payload_format->bits_per_sample, "buffered");
        playback_announced = true;
      }
      expected_timestamp = packet.rtp + (uint32_t)frames;
      packets_played_in_sequence++;
#if defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
      am_last_published_seq = packet.seq;
#endif
    } else {
#if defined(CONFIG_AIRPLAY_DIAG_AUTOMIX) && CONFIG_AIRPLAY_DIAG_AUTOMIX
      {
        timing_snapshot_t ds;
        snapshot_state(&ds);
        uint32_t now_rtp = 0;
        const bool now_valid = wanted_rtp_now(&ds, &now_rtp);
        ++am_late_dropped;
        ESP_LOGW(TAG, "AUTOMIX DECODED BLOCK NOT STORED seq=%" PRIu32
                      " rtp=%" PRIu32 " lead %ld ms (playing=%d anchor=%d "
                      "pcm_gen %s)",
                 packet.seq, packet.rtp,
                 now_valid ? (long)((int64_t)rtp_delta(packet.rtp, now_rtp) *
                                    1000LL / sr) : -1L,
                 ds.playing ? 1 : 0, ds.anchor_valid ? 1 : 0,
                 ds.pcm_generation == snap.pcm_generation ? "same" : "changed");
      }
#endif
      /* FLUSH/anchor changes may cancel its publication without changing the
       * PCM generation: the next surviving block starts a new play sequence. */
      expected_timestamp = 0;
      packets_played_in_sequence = 0;
    }

    /* No manual yield: RTSP control (priority 17) runs on the same core as
     * this processor (priority 4) and preempts refill bursts immediately. */
  }

  if (decoder) aac_decoder_destroy(decoder);
  if (alac) alac_decoder_destroy(alac);
  s.processor_task = NULL;
#if AP2_PROCESS_STACK_PSRAM
  vTaskDeleteWithCaps(NULL);
#else
  vTaskDelete(NULL);
#endif
}

static bool frame_is_fully_stale(uint32_t rtp, const timing_snapshot_t *snap,
                                 int64_t *end_delta_us) {
  uint32_t wanted;
  if (!wanted_rtp_now(snap, &wanted)) {
    return false;
  }
  uint32_t frame_samples = snap->format.frame_size > 0
                               ? (uint32_t)snap->format.frame_size
                               : 1024U;
  uint32_t end_rtp = rtp + frame_samples;
  int32_t remaining = rtp_delta(end_rtp, wanted);
  if (end_delta_us) {
    int sr = snap->format.sample_rate > 0 ? snap->format.sample_rate : 44100;
    *end_delta_us = ((int64_t)remaining * 1000000LL) / sr;
  }
  return remaining <= 0;
}

static bool realtime_playout_deadline(uint32_t rtp,
                                      int64_t *time_to_play_us, void *ctx) {
  (void)ctx;
  if (!time_to_play_us) return false;

  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME || !snap.playing ||
      !snap.anchor_valid || snap.timeline_reset_pending) {
    return false;
  }

  uint64_t target_time_ns = 0;
  if (!rtp_to_presentation_ns(&snap, rtp, &target_time_ns)) return false;

  const int64_t now_time_ns = (int64_t)presentation_now_ns(&snap);
  *time_to_play_us = ((int64_t)target_time_ns - now_time_ns) / 1000LL;
  return true;
}

static bool realtime_pcm_sink_locked(uint32_t rtp, int16_t *pcm, size_t frames,
                              int channels, void *ctx) {
  (void)ctx;
  if (!pcm || frames == 0 || channels != 2) {
    return false;
  }

  /* Realtime ALAC is decoded immediately by the UDP receiver. This sink does
   * not run EQ and does not write the PCM FIFO; it only places raw
   * PCM at its exact RTP address so later/RTX packets can fill holes without
   * blocking newer packets. */
  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME) {
    return false;
  }

  /* Never invent RTP<->PTP from packet arrival time. Before the first real
   * sender anchor, realtime PCM may be stored by RTP address in the prepared
   * generation, but staging/playout remain stopped until D7/SETRATE makes
   * anchor_valid true. After an unqualified FLUSH (timeline reset pending),
   * reject provisional PCM until the sender supplies a fresh real anchor. */
  if (snap.timeline_reset_pending) {
    return false;
  }

  if (snap.anchor_valid && frame_is_fully_stale(rtp, &snap, NULL)) {
    return false;
  }

  /* Once the chronological EQ cursor has passed an RTP range, a late RTX
   * cannot repair the stateful filter history. Drop it without touching raw
   * or final PCM. */
  bool cursor_valid = false;
  uint32_t cursor = 0;
  taskENTER_CRITICAL(&s.state_mux);
  if (s.realtime_stage_cursor_valid &&
      s.realtime_stage_generation == snap.generation) {
    cursor_valid = true;
    cursor = s.realtime_stage_cursor_rtp;
  }
  taskEXIT_CRITICAL(&s.state_mux);
  if (cursor_valid && rtp_delta(rtp + (uint32_t)frames, cursor) <= 0) {
    return true;
  }

  uint32_t wanted = 0;
  bool wanted_valid = wanted_rtp_now(&snap, &wanted);
  if (!s.realtime_stage_ring ||
      !pcm_rtp_ring_write(s.realtime_stage_ring, rtp, pcm, frames, channels,
                          snap.generation, wanted, wanted_valid)) {
    return false;
  }

  taskENTER_CRITICAL(&s.state_mux);
  if (s.generation != snap.generation || s.timeline_reset_pending ||
      s.stream_type != AUDIO_STREAM_REALTIME) {
    taskEXIT_CRITICAL(&s.state_mux);
    return false;
  }
  if (!s.realtime_stage_cursor_valid ||
      s.realtime_stage_generation != snap.generation) {
    s.realtime_stage_cursor_rtp = rtp;
    s.realtime_stage_generation = snap.generation;
    s.realtime_stage_cursor_valid = true;
  }
  taskEXIT_CRITICAL(&s.state_mux);

  /* Wake ordered staging only when new RAW PCM has actually been published.
   * Counting task notifications coalesce bursts and cannot lose a wakeup if
   * the write races with the task entering its blocked state. */
  realtime_stage_kick();
  return true;
}

static bool realtime_pcm_sink(uint32_t rtp, int16_t *pcm, size_t frames,
                              int channels, void *ctx) {
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  bool ok = realtime_pcm_sink_locked(rtp, pcm, frames, channels, ctx);
  xSemaphoreGive(s.publish_mutex);
  return ok;
}


static void realtime_stage_task(void *arg) {
  (void)arg;
  int16_t *raw = s.realtime_stage_pcm; /* raw 16-bit ALAC from the stage ring */
  float *pcm = s.pcm_f32;              /* float from here to DMA */

  AUDIO_DIAG_LIFECYCLE_TASK_STARTED(AUDIO_DIAG_TASK_ALAC_STAGE,
                                    xPortGetCoreID(), AP2_RT_STAGE_PRIORITY, 0U);

  bool alac_in_silence = false; /* log only the first of a silent run */
  __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);

  while (__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) {
    /* Stop acknowledgement and permission to enter EQ are one state change.
     * Control cannot observe old idle=true after this iteration was admitted. */
    taskENTER_CRITICAL(&s.state_mux);
    const bool stage_running =
        __atomic_load_n(&s.realtime_stage_running, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s.realtime_stage_idle, !stage_running, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&s.state_mux);
    if (!stage_running) {
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    timing_snapshot_t snap;
    snapshot_state(&snap);
    if (snap.stream_type != AUDIO_STREAM_REALTIME || !snap.anchor_valid ||
        snap.timeline_reset_pending || snap.format.frame_size <= 0 ||
        snap.format.frame_size > (int)AP2_PCM_CAPACITY_FRAMES) {
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    uint32_t cursor = 0;
    bool cursor_valid = false;
    taskENTER_CRITICAL(&s.state_mux);
    if (s.realtime_stage_cursor_valid &&
        s.realtime_stage_generation == snap.generation) {
      cursor = s.realtime_stage_cursor_rtp;
      cursor_valid = true;
    }
    taskEXIT_CRITICAL(&s.state_mux);
    if (!cursor_valid) {
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    const uint32_t frames = (uint32_t)snap.format.frame_size;
    int64_t time_to_play_us = 0;
    const bool have_deadline =
        realtime_playout_deadline(cursor, &time_to_play_us, NULL);
    bool have = pcm_rtp_ring_read(s.realtime_stage_ring, cursor, frames,
                                  snap.generation, raw);
    if (!have) {
      if (!have_deadline || time_to_play_us > AP2_RT_STAGE_COMMIT_MARGIN_US) {
        /* No polling: sleep until RAW PCM/RTX publication changes the ring,
         * or until the exact remaining recovery window reaches the existing
         * commit margin. If there is no valid deadline yet, a timing/state
         * transition will explicitly notify this task. */
        TickType_t wait_ticks = portMAX_DELAY;
        if (have_deadline) {
          wait_ticks = realtime_stage_wait_ticks(
              time_to_play_us - AP2_RT_STAGE_COMMIT_MARGIN_US);
        }
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
        continue;
      }

      /* Recovery time is exhausted. Preserve RTP duration with one silent
       * ALAC-sized PCM block. A later RTX is too late and is discarded by
       * realtime_pcm_sink(). */
      memset(pcm, 0, (size_t)frames * 2U * sizeof(float));
      if (!alac_in_silence)
        aevt_post(AEVT_ALAC_SILENCE, NULL, (int32_t)cursor, (int32_t)frames,
                  0, have_deadline ? (int32_t)(time_to_play_us / 1000LL) : -1);
      alac_in_silence = true;
    } else {
      audio_pcm16_to_float_block(raw, pcm, (size_t)frames * 2U);
      alac_in_silence = false;
    }

    bool published = false;
    while (__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) {
      xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
      taskENTER_CRITICAL(&s.state_mux);
      const bool current =
          __atomic_load_n(&s.realtime_stage_running, __ATOMIC_ACQUIRE) &&
          s.stream_type == AUDIO_STREAM_REALTIME &&
          s.generation == snap.generation && !s.timeline_reset_pending &&
          s.realtime_stage_generation == snap.generation &&
          s.realtime_stage_cursor_valid && s.realtime_stage_cursor_rtp == cursor;
      taskEXIT_CRITICAL(&s.state_mux);
      if (!current) {
        xSemaphoreGive(s.publish_mutex);
        break;
      }
      published = pcm_fifo_write(s.pcm_fifo, cursor, pcm, frames,
                                 snap.generation, 0U, false);
      if (published) {
        taskENTER_CRITICAL(&s.state_mux);
        if (s.realtime_stage_generation == snap.generation &&
            s.realtime_stage_cursor_valid && s.realtime_stage_cursor_rtp == cursor)
          s.realtime_stage_cursor_rtp = cursor + frames;
        taskEXIT_CRITICAL(&s.state_mux);
      }
      xSemaphoreGive(s.publish_mutex);
      if (published) break;
      vTaskDelay(1);
    }
    if (!published) continue; /* canceled by FLUSH/stop/generation change */

  }

  __atomic_store_n(&s.realtime_stage_idle, true, __ATOMIC_RELEASE);
  s.realtime_stage_task = NULL;
  vTaskDelete(NULL);
}


static bool completion_sync_us(const timing_snapshot_t *snap,
                               const audio_playout_completion_t *done,
                               int32_t *sync_us_out) {
  if (!snap || !done || !sync_us_out || done->generation != snap->generation ||
      !snap->anchor_valid || snap->timeline_reset_pending) {
    return false;
  }

  uint64_t target_end_ns = 0;
  if (!rtp_to_presentation_ns(snap, done->rtp + done->frames, &target_end_ns)) {
    return false;
  }

  const int64_t done_time_ns = completion_presentation_ns(snap, done);
  int64_t sync_us = ((int64_t)target_end_ns - done_time_ns) / 1000LL;
  if (sync_us > 2000000000LL) sync_us = 2000000000LL;
  if (sync_us < -2000000000LL) sync_us = -2000000000LL;
  *sync_us_out = (int32_t)sync_us;
  return true;
}

static void process_i2s_completions(const timing_snapshot_t *snap) {
  audio_playout_completion_t done;
  while (audio_playout_poll_completion(&done)) {
    int32_t sync_us = 0;
    if (!completion_sync_us(snap, &done, &sync_us)) continue;
    if (!s.output_sync.valid ||
        s.output_sync.generation != done.generation) {
      s.output_sync.us = sync_us;
      s.output_sync.us_q4 = (int64_t)sync_us * 16;
      s.output_sync.generation = done.generation;
      s.output_sync.valid = true;
      s.output_sync.phase_count = 0;
      s.output_sync.phase_index = 0;
    } else {
      /* EMA (1/8 per DMA block) as the PID input. */
      s.output_sync.us_q4 += ((int64_t)sync_us * 16 - s.output_sync.us_q4) / 8;
      const int64_t q = s.output_sync.us_q4;
      s.output_sync.us = (int32_t)((q >= 0 ? q + 8 : q - 8) / 16);
    }

    /* Passive robust phase history only. No controller reads these raw samples. */
    s.output_sync.phase_samples[s.output_sync.phase_index] = sync_us;
    s.output_sync.phase_index =
        (uint8_t)((s.output_sync.phase_index + 1U) % AP2_PHASE_HISTORY_SAMPLES);
    if (s.output_sync.phase_count < AP2_PHASE_HISTORY_SAMPLES)
      s.output_sync.phase_count++;
  }
}


/* Output volume on float PCM in place (1.0 = full scale), Q15 targets
 * (0..32768, 32768 = 0 dB). A changed target fades over one DMA block. Unity
 * gain leaves the samples untouched (bit-exact); no clipping here, the single
 * saturation is the float -> int32 step in output_stage(). Playout task only. */
static void apply_output_volume_f32(float *pcm, uint32_t frames,
                                    int32_t current_q15[2]) {
  if (!pcm || !current_q15 || frames == 0U) return;
  int32_t volume = __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
  if (volume < 0) volume = 0;
  if (volume > 32768) volume = 32768;
  const uint32_t muted = audio_receiver_get_output_mute_mask();
  const int32_t target[2] = {
      (muted & 1U) ? 0 : volume, (muted & 2U) ? 0 : volume};
  const int32_t start[2] = {current_q15[0], current_q15[1]};
  if (start[0] == target[0] && start[1] == target[1]) {
    if (target[0] == 32768 && target[1] == 32768) return;
    if (target[0] == 0 && target[1] == 0) {
      memset(pcm, 0, (size_t)frames * 2U * sizeof(*pcm));
      return;
    }
    const float g[2] = {(float)target[0] * (1.0f / 32768.0f),
                        (float)target[1] * (1.0f / 32768.0f)};
    for (uint32_t i = 0; i < frames * 2U; ++i) pcm[i] *= g[i & 1U];
    return;
  }
  /* Fade each changed output over one DMA block. An unchanged unity-gain
   * channel stays bit-exact, even while the other one fades. */
  const int64_t dg[2] = {
      (int64_t)target[0] - start[0], (int64_t)target[1] - start[1]};
  for (uint32_t f = 0; f < frames; ++f) {
    for (uint32_t ch = 0; ch < 2U; ++ch) {
      const uint32_t i = f * 2U + ch;
      const int32_t gain = start[ch] +
          (int32_t)((dg[ch] * (int64_t)(f + 1U)) / (int64_t)frames);
      if (gain == 32768) continue;
      pcm[i] *= (float)gain * (1.0f / 32768.0f);
    }
  }
  current_q15[0] = target[0];
  current_q15[1] = target[1];
}

/* Fade-in after a stop (audio_fade.h): the first AP2_FADE_IN_FRAMES frames
 * of every start that follows a stop (pause, FLUSH, track change, seek, lost
 * timing, the first start, any I2S stop) rise from 0 along an S-curve, so
 * a song resumed mid-waveform starts without a click. Only a stop arms it:
 * inside a running stream (gapless, AutoMix, a re-prime while playing) the
 * audio is never touched. After the fade the chain is bit-exact again.
 * The fade begins at the first frame that is not exact digital silence,
 * at most AP2_FADE_IN_WAIT_FRAMES after the start: the first AAC block of a
 * play sequence is muted to zeros and a start often falls inside it.
 * Playout task only. */
#define AP2_FADE_IN_FRAMES (8U * AUDIO_PLAYOUT_FRAMES) /* ~87 ms at 44.1 kHz */
#define AP2_FADE_IN_WAIT_FRAMES 2048U /* ~46 ms: one muted AAC block + margin */
static bool s_fade_in_armed = true;                 /* the first start fades in */
static uint32_t s_fade_in_pos = AP2_FADE_IN_FRAMES; /* == FRAMES: no fade */
static uint32_t s_fade_in_wait;                     /* zero frames still skipped */

static void fade_in_arm(void) {
  s_fade_in_armed = true;
  s_fade_in_pos = AP2_FADE_IN_FRAMES;
}

static void fade_in_apply(float *pcm, uint32_t frames) {
  if (s_fade_in_armed) { /* the first block of a start after a stop */
    s_fade_in_armed = false;
    s_fade_in_pos = 0U;
    s_fade_in_wait = AP2_FADE_IN_WAIT_FRAMES;
  }
  audio_fade_in_block(pcm, frames, &s_fade_in_pos, &s_fade_in_wait,
                      AP2_FADE_IN_FRAMES);
}

static uint32_t s_stereo_route = AUDIO_STEREO_ROUTE_STEREO;

void audio_receiver_set_stereo_route(audio_stereo_route_t route) {
  __atomic_store_n(&s_stereo_route, (uint32_t)route, __ATOMIC_RELEASE);
}

/* Surround speaker with a stereo source: keep only its side of it. */
static void apply_stereo_route(float *block, uint32_t frames) {
  const audio_stereo_route_t route = (audio_stereo_route_t)__atomic_load_n(
      &s_stereo_route, __ATOMIC_ACQUIRE);
  if (route == AUDIO_STEREO_ROUTE_STEREO) return;
  /* A multichannel stream is already this speaker's channel. */
  if (aac_decoder_get_mc_status(NULL, NULL, NULL, 0)) return;
  for (uint32_t i = 0; i < frames; ++i) {
    float *f = block + (size_t)i * 2U;
    const float v = route == AUDIO_STEREO_ROUTE_LEFT    ? f[0]
                    : route == AUDIO_STEREO_ROUTE_RIGHT ? f[1]
                    : route == AUDIO_STEREO_ROUTE_MONO  ? 0.5f * (f[0] + f[1])
                                                        : 0.0f;
    f[0] = v;
    f[1] = v;
  }
}

/* The end of the float chain: the speaker's stereo route, EQ, volume,
 * loudness, the fade-in after a stop, then the one and only saturation
 * float -> 32-bit slot, in place. Returns the same memory as the I2S block.
 * Only the playout task calls this, and only for blocks that are really
 * played, so the EQ filter history is exactly the audio that was heard: PCM
 * replaced in the FIFO (AutoMix re-send, FLUSH) never passed through it, and
 * EQ changes are heard within the DMA queue (~87 ms) instead of the whole
 * PCM window. */
static const audio_out_sample_t *output_stage(float *block, uint32_t frames,
                                              int sample_rate,
                                              int32_t current_q15[2]) {
  apply_stereo_route(block, frames);
  audio_eq_process(block, frames, 2, sample_rate > 0 ? sample_rate : 44100);
  apply_output_volume_f32(block, frames, current_q15);
  /* Loudness follows the volume just applied (current_q15 = its target). */
  audio_loudness_process_f32(block, frames, current_q15[0] > current_q15[1]
                                                ? current_q15[0] : current_q15[1]);
  fade_in_apply(block, frames);
  return audio_out_from_float_block_inplace(block, (size_t)frames * 2U);
}


#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
/* A failed I2S stop stays pending (i2s_flush_requested) and is retried as a
 * real stop, never answered with zeros. Playout task only. */
static bool s_stop_retry;
#endif

/* I2S lifecycle is single-owner on the playout task.  A failed flush must
 * never be treated as a successful STOPPED boundary: the driver may still be
 * RUNNING or may still contain preloaded DMA.  Keep the control request
 * pending so the next playout iteration retries before any new PRIME work is
 * allowed to touch the channel. */
static bool playout_flush_checked(const char *reason) {
  const bool was_running = audio_playout_is_enabled();
  const esp_err_t err = audio_playout_flush();
  if (err == ESP_OK) {
#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
    s_stop_retry = false;
#endif
    if (was_running) {
      aevt_post(AEVT_I2S_STOP, reason ? reason : "unknown",
                (int32_t)__atomic_load_n(&s.generation, __ATOMIC_RELAXED), 0, 0, 0);
    }
    /* I2S is stopped. Only now forget the EQ and loudness filters' memory of
     * the audio before the stop, so whatever plays next starts from silence. */
    audio_eq_reset_state();
    audio_loudness_reset();
    fade_in_arm();
    return true;
  }
#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
  s_stop_retry = true;
#endif
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
  pevt_post(PEVT_FLUSH_FAIL, (int32_t)err, 0, 0, 0, 0,
            reason ? reason : "unknown");
  return false;
}

/* Soft stop (pause, FLUSH, seek, track change, lost timing) with the I2S
 * clocks running: the audio queued in the DMA fades out (~55-75 ms) and the
 * caller keeps feeding zeros (*idle_zeros) until the next warm start, which
 * fades in. Returns false when that is not possible (option off, I2S not
 * running, a stop retry pending, descriptors not known yet): the caller then
 * stops I2S as before. */
#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
static bool playout_keep_running_stop(const char *reason, bool *idle_zeros) {
  uint32_t faded_frames = 0U;
  if (s_stop_retry || !audio_playout_is_enabled() ||
      audio_playout_fade_out_queued(&faded_frames) != ESP_OK) {
    return false;
  }
  /* What plays next starts from silence, as after a real stop. */
  audio_eq_reset_state();
  audio_loudness_reset();
  fade_in_arm();
  if (!*idle_zeros) {
    aevt_post(AEVT_I2S_IDLE, reason ? reason : "unknown",
              (int32_t)__atomic_load_n(&s.generation, __ATOMIC_RELAXED),
              (int32_t)(((uint64_t)faded_frames * 1000ULL) /
                        audio_playout_get_rate()), 0, 0);
    *idle_zeros = true;
  }
  return true;
}
#else
static inline bool playout_keep_running_stop(const char *reason,
                                             bool *idle_zeros) {
  (void)reason;
  (void)idle_zeros;
  return false;
}
#endif

/* One block of digital silence, shared read-only by the start-up phase probe,
 * the idle zeros and the latency measurement (zero-initialised, never
 * written). */
static audio_out_sample_t s_silence_block[AUDIO_PLAYOUT_FRAMES * 2U];

/* ---- wired latency measurement: the I2S side ----
 * Runs like the Wi-Fi scan. The web handler stops RTSP and releases
 * the audio engine (playout task stopped, codec memory freed), so the I2S
 * driver has no other owner and the ADC gets internal DMA memory. The caller's
 * task then plays silence + LATENCY_CAL_BURSTS chirps and records, from the
 * tagged DMA completions, when each chirp started at the same reference point
 * the sync servo uses. AirPlay is restored afterwards by the caller. */
#if CONFIG_AIRPLAY_LATENCY_CAL
#define CAL_GENERATION      0xCA1B0000U
#define CAL_LEAD_BLOCKS     16U   /* ~174 ms silence before the first chirp */
#define CAL_BURST_EVERY     20U   /* ~218 ms between chirps */
#define CAL_TAIL_BLOCKS     20U   /* covers up to ~218 ms of chain latency */
#define CAL_TOTAL_BLOCKS    (CAL_LEAD_BLOCKS + LATENCY_CAL_BURSTS * CAL_BURST_EVERY + CAL_TAIL_BLOCKS)
#define CAL_I2S_RATE_HZ     audio_playout_get_rate()

static struct {
  int64_t emit_us[LATENCY_CAL_BURSTS];
  int n_emit;
} s_cal;
static volatile bool s_cal_busy = false;

static bool cal_block_is_burst(uint32_t idx, uint32_t *burst) {
  if (idx < CAL_LEAD_BLOCKS) return false;
  const uint32_t rel = idx - CAL_LEAD_BLOCKS;
  if ((rel % CAL_BURST_EVERY) != 0U) return false;
  const uint32_t k = rel / CAL_BURST_EVERY;
  if (k >= LATENCY_CAL_BURSTS) return false;
  if (burst) *burst = k;
  return true;
}

static void cal_take_completion(const audio_playout_completion_t *d) {
  uint32_t k = 0;
  if (d->generation != CAL_GENERATION || !cal_block_is_burst(d->rtp, &k)) return;
  /* done_local_us marks the END of the block (the sync servo's reference);
   * the chirp starts at the first sample of the block. */
  s_cal.emit_us[k] = d->done_local_us -
      ((int64_t)d->frames * 1000000LL) / (int64_t)CAL_I2S_RATE_HZ;
  s_cal.n_emit++;
}

static esp_err_t playout_run_latency_calibration_blocks(
    int16_t *burst, audio_out_sample_t *burst_out) {
  const audio_out_sample_t *silence = s_silence_block;
  s_cal.n_emit = 0;
  for (int k = 0; k < LATENCY_CAL_BURSTS; ++k) s_cal.emit_us[k] = 0;

  if (!playout_flush_checked("latency-cal")) return ESP_FAIL;
  bool preload_ok = true;
  for (uint32_t k = 0; k < AUDIO_PLAYOUT_DMA_BLOCKS && preload_ok; ++k) {
    preload_ok = audio_playout_preload_tagged(silence, AUDIO_PLAYOUT_FRAMES, k,
                                              CAL_GENERATION) == ESP_OK;
  }
  if (!preload_ok || audio_playout_enable() != ESP_OK) {
    (void)playout_flush_checked("latency-cal-start");
    return ESP_FAIL;
  }
  audio_playout_completion_t d;
  for (uint32_t idx = AUDIO_PLAYOUT_DMA_BLOCKS; idx < CAL_TOTAL_BLOCKS; ++idx) {
    const bool is_burst = cal_block_is_burst(idx, NULL);
    if (is_burst) {
      latency_cal_fill_burst(burst, AUDIO_PLAYOUT_FRAMES, CAL_I2S_RATE_HZ);
      const uint32_t muted = audio_receiver_get_output_mute_mask();
      for (uint32_t f = 0; muted && f < AUDIO_PLAYOUT_FRAMES; ++f) {
        if (muted & 1U) burst[f * 2U] = 0;
        if (muted & 2U) burst[f * 2U + 1U] = 0;
      }
      /* Same chirp level in both slot widths (unity gain, bit-exact). */
      audio_out_from_pcm16_block(burst, burst_out,
                                 (size_t)AUDIO_PLAYOUT_FRAMES * 2U);
    }
    const audio_out_sample_t *blk = is_burst ? burst_out : silence;
    if (audio_playout_write_tagged(blk, AUDIO_PLAYOUT_FRAMES, idx,
                                   CAL_GENERATION) != ESP_OK ||
        audio_playout_has_fault()) {
      (void)playout_flush_checked("latency-cal-write");
      return ESP_FAIL;
    }
    while (audio_playout_poll_completion(&d)) cal_take_completion(&d);
  }
  /* drain the last descriptors */
  for (int i = 0; i < 10 && s_cal.n_emit < LATENCY_CAL_BURSTS; ++i) {
    if (audio_playout_wait_completion(&d, pdMS_TO_TICKS(20))) cal_take_completion(&d);
  }
  (void)playout_flush_checked("latency-cal-end");
  return s_cal.n_emit == LATENCY_CAL_BURSTS ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* The chirp buffers are only needed while a measurement runs: take them for
 * the measurement and give them back afterwards. */
static esp_err_t playout_run_latency_calibration(void) {
  int16_t *burst = malloc(AUDIO_PLAYOUT_FRAMES * 2U * sizeof(int16_t));
  audio_out_sample_t *burst_out =
      malloc(AUDIO_PLAYOUT_FRAMES * 2U * sizeof(audio_out_sample_t));
  esp_err_t err = ESP_ERR_NO_MEM;
  if (burst && burst_out) {
    err = playout_run_latency_calibration_blocks(burst, burst_out);
  }
  free(burst_out);
  free(burst);
  return err;
}

#endif /* CONFIG_AIRPLAY_LATENCY_CAL */

/* ---- room calibration: test chirps for the Gateway's microphone ----
 * Same lifecycle as the wired latency test: the engine is released (only
 * between streams), the caller's task owns I2S, plays silence and the chirps
 * and records from the tagged DMA completions when each chirp started at the
 * sync servo's reference point. Unlike the wired test the chirps go through
 * the EQ (a 2-way speaker's crossover protects the tweeter), at the level the
 * Gateway asks for. The engine is restored before returning. */
#define ROOMCAL_GENERATION  0xCA1C0000U
#define ROOMCAL_LEAD_BLOCKS 16U /* ~170 ms of silence before the first chirp */
#define ROOMCAL_TAIL_BLOCKS 20U /* the last chirp's echoes */

static volatile bool s_roomcal_busy = false;

/* A completed DMA block: the first block of a chirp gives its start. */
static void roomcal_take(const audio_playout_completion_t *d, uint32_t rate,
                         uint32_t period_blocks, int bursts, int64_t *emit_us,
                         int *n_emit) {
  if (d->generation != ROOMCAL_GENERATION || d->rtp < ROOMCAL_LEAD_BLOCKS) return;
  const uint32_t r = d->rtp - ROOMCAL_LEAD_BLOCKS;
  if (r % period_blocks != 0U || r / period_blocks >= (uint32_t)bursts) return;
  /* done_local_us marks the END of the block; the chirp starts with it. */
  emit_us[r / period_blocks] =
      d->done_local_us - ((int64_t)d->frames * 1000000LL) / (int64_t)rate;
  (*n_emit)++;
}

static esp_err_t roomcal_play(cal_chirp_kind_t kind, float gain, int bursts,
                              float *block, int64_t *emit_us, int *n_emit) {
  const uint32_t rate = audio_playout_get_rate();
  const uint32_t frames = AUDIO_PLAYOUT_FRAMES;
  const uint32_t period_blocks = (uint32_t)ceil(cal_chirp_period_s(kind) * rate / frames);
  const uint32_t chirp_blocks = (uint32_t)ceil(cal_chirp_length_s(kind) * rate / frames);
  const uint32_t total = ROOMCAL_LEAD_BLOCKS + (uint32_t)bursts * period_blocks +
                         ROOMCAL_TAIL_BLOCKS;
  *n_emit = 0;

  if (!playout_flush_checked("room-cal")) return ESP_FAIL;
  bool ok = true;
  for (uint32_t k = 0; k < AUDIO_PLAYOUT_DMA_BLOCKS && ok; ++k) {
    ok = audio_playout_preload_tagged(s_silence_block, frames, k,
                                      ROOMCAL_GENERATION) == ESP_OK;
  }
  if (!ok || audio_playout_enable() != ESP_OK) {
    (void)playout_flush_checked("room-cal-start");
    return ESP_FAIL;
  }
  audio_playout_completion_t d;
  for (uint32_t idx = AUDIO_PLAYOUT_DMA_BLOCKS; idx < total; ++idx) {
    const uint32_t rel = idx - ROOMCAL_LEAD_BLOCKS;
    const uint32_t burst = rel / period_blocks;
    const uint32_t off = rel % period_blocks;
    const bool chirp = idx >= ROOMCAL_LEAD_BLOCKS && burst < (uint32_t)bursts &&
                       off < chirp_blocks;
    for (uint32_t f = 0; f < frames; ++f) {
      const float v =
          chirp ? gain * cal_chirp_value(kind, (double)(off * frames + f) / rate) : 0.0f;
      block[2U * f] = v;
      block[2U * f + 1U] = v;
    }
    /* Silence too, so the filters ring out naturally after each chirp. */
    audio_eq_process(block, frames, 2, (int)rate);
    const uint32_t muted = audio_receiver_get_output_mute_mask();
    for (uint32_t f = 0; muted && f < frames; ++f) {
      if (muted & 1U) block[2U * f] = 0.0f;
      if (muted & 2U) block[2U * f + 1U] = 0.0f;
    }
    const audio_out_sample_t *out =
        audio_out_from_float_block_inplace(block, (size_t)frames * 2U);
    if (audio_playout_write_tagged(out, frames, idx, ROOMCAL_GENERATION) != ESP_OK ||
        audio_playout_has_fault()) {
      (void)playout_flush_checked("room-cal-write");
      return ESP_FAIL;
    }
    while (audio_playout_poll_completion(&d)) {
      roomcal_take(&d, rate, period_blocks, bursts, emit_us, n_emit);
    }
  }
  /* The tail is longer than the DMA queue, so every chirp has completed by
   * now; drain what is left. */
  for (int i = 0; i < 10 && *n_emit < bursts; ++i) {
    if (audio_playout_wait_completion(&d, pdMS_TO_TICKS(20))) {
      roomcal_take(&d, rate, period_blocks, bursts, emit_us, n_emit);
    }
  }
  (void)playout_flush_checked("room-cal-end");
  return *n_emit == bursts ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_receiver_play_cal_chirps(int kind, int level_db, int bursts,
                                         int64_t *emit_us, int *n_emit) {
  if (!emit_us || !n_emit || bursts < 1) return ESP_ERR_INVALID_ARG;
  *n_emit = 0;
  if (__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE) ||
      realtime_receiver_is_running()) {
    return ESP_ERR_INVALID_STATE; /* a stream is on */
  }
  if (__atomic_exchange_n(&s_roomcal_busy, true, __ATOMIC_ACQ_REL)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (level_db > 0) level_db = 0;
  if (level_db < -60) level_db = -60;
  float *block = malloc(AUDIO_PLAYOUT_FRAMES * 2U * sizeof(float));
  esp_err_t err = block ? audio_receiver_release_for_wifi_scan() : ESP_ERR_NO_MEM;
  if (err == ESP_OK) err = audio_playout_init();
  if (err == ESP_OK) {
    amp_control_session_connected();
    vTaskDelay(pdMS_TO_TICKS(400)); /* amplifier start-up / anti-pop time */
    err = roomcal_play(kind == CAL_CHIRP_LOW ? CAL_CHIRP_LOW : CAL_CHIRP_WIDE,
                       powf(10.0f, (float)level_db / 20.0f), bursts, block,
                       emit_us, n_emit);
    amp_control_session_disconnected();
  }
  free(block);
  /* Whatever happened, the engine is needed for the next stream. */
  const esp_err_t init_err = audio_receiver_init();
  __atomic_store_n(&s_roomcal_busy, false, __ATOMIC_RELEASE);
  AP_LOGI(TAG, "ROOM CAL %s chirps at %d dBFS: %s, %d emitted",
          kind == CAL_CHIRP_LOW ? "low" : "wide", level_db, esp_err_to_name(err),
          *n_emit);
  return err != ESP_OK ? err : init_err;
}

static esp_err_t playout_write(const audio_out_sample_t *blk, uint32_t frames,
                               uint32_t rtp, uint32_t generation) {
  return audio_playout_write_tagged(blk, frames, rtp, generation);
}

static void ap2_playout_task(void *arg) {
  (void)arg;
  /* One block: float PCM read from the FIFO, then volume and loudness in
   * place, then converted in place to the 32-bit I2S slots (float and int32
   * are both 4 bytes). i2s_channel_write() copies it into the DMA before
   * returning, so one block is enough. */
  _Static_assert(sizeof(float) == sizeof(audio_out_sample_t),
                 "in-place float -> int32 conversion needs equal sizes");
  float *block = heap_caps_malloc(AUDIO_PLAYOUT_FRAMES * 2U * sizeof(float),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!block) block = malloc(AUDIO_PLAYOUT_FRAMES * 2U * sizeof(float));
  if (!block) {
    ESP_LOGE(TAG, "playout block allocation failed");
    s.playout_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  typedef enum {
    PLAYOUT_STOPPED = 0,
    PLAYOUT_PRIMING = 1,
    PLAYOUT_RUNNING = 2,
  } playout_state_t;

  uint32_t cursor_rtp = 0; /* next block to submit after the preloaded blocks */
  uint32_t cursor_generation = 0;
  const int32_t initial_volume =
      __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
  const uint32_t initial_mute = audio_receiver_get_output_mute_mask();
  int32_t volume_current_q15[2] = {
      (initial_mute & 1U) ? 0 : initial_volume,
      (initial_mute & 2U) ? 0 : initial_volume};
  playout_state_t state = PLAYOUT_STOPPED;
  uint32_t zero_blocks = 0; /* consecutive whole blocks played as zeros */
  /* Why the next PRIMING flush happens, for the AUDIO EVT log. */
  const char *restart_reason = "prime-start";
  /* With CONFIG_AIRPLAY_I2S_KEEP_RUNNING, pause, FLUSH and track changes
   * fade out the queued audio, keep the I2S clocks running and play zeros;
   * a new start is then aligned on the running DMA ("warm" prime). I2S is
   * disabled only at hard boundaries (session/codec, DMA fault). Without it
   * every stop disables I2S; the warm prime is then used only for a re-prime
   * while playing. Every start after a stop fades in (fade_in_apply). */
  bool warm_prime = false;
  bool retune_pending = false;
  int32_t retune_ppm = 0;
  bool idle_zeros = false;   /* currently feeding zeros while not RUNNING */
  uint32_t warm_probe_seq = 0;
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
  int64_t pd_last_write_us = 0;
#endif
  int32_t servo_ppm = 0;
  int32_t servo_target_ppm = 0;
  int64_t pid_last_calc_us = 0;
  int64_t pid_last_tune_us = 0;
  uint32_t tune_fail_count = 0;
  int64_t resync_over_since_us = 0;
  uint32_t resync_count = 0;
  uint32_t start_logged_gen = 0;
  bool center_active = false;
  int32_t center_prev_sync_us = 0;
  bool center_prev_valid = false;
  int32_t center_jump_us = 0; /* sum of reference steps inside this window */
  int64_t center_out_since_us = 0;
  int64_t center_start_us = 0;
  /* Least-squares line through the 1 Hz sync samples of the window:
   * robust slope and a de-noised end-of-window sync estimate. */
  double center_n = 0.0, center_st = 0.0, center_sy = 0.0;
  double center_stt = 0.0, center_sty = 0.0;
  uint32_t servo_generation = 0;
  bool servo_seed_pending = true; /* start the next session from memory */
  double pid_integral_ms_s = 0.0;
  double pid_prev_error_ms = 0.0;
  double pid_d_filtered_ms_s = 0.0;
  bool pid_prev_valid = false;

  while (__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) {
    if (__atomic_exchange_n(&s.playout_servo_reset_requested, false,
                            __ATOMIC_ACQ_REL)) {
      /* A codec/session boundary is stronger than a normal timeline change.
       * Do not inherit the previous stream's learned crystal correction.
       * Ordinary buffered cursor/anchor changes deliberately keep the learned
       * physical clock correction; presentation changes are not new hardware
       * clock sessions. */
      if (!playout_flush_checked("servo-reset")) {
        __atomic_store_n(&s.playout_servo_reset_requested, true,
                         __ATOMIC_RELEASE);
        vTaskDelay(1);
        continue;
      }
      esp_err_t re = audio_playout_reset_tune();

      if (re != ESP_OK) {
        __atomic_store_n(&s.playout_servo_reset_requested, true,
                         __ATOMIC_RELEASE);
        /* A hard boundary is not complete until the physical clock is back
         * at nominal.  Retry instead of ACKing a half-reset session. */
        pevt_post(PEVT_SERVO_RESET_FAIL, (int32_t)re, 0, 0, 0, 0, NULL);
        vTaskDelay(1);
        continue;
      }

      servo_ppm = 0;
      servo_target_ppm = 0;
      servo_seed_pending = true;
      idle_zeros = false;
      servo_generation = 0;
      center_active = false;
      pid_integral_ms_s = 0.0;
      pid_prev_error_ms = 0.0;
      pid_d_filtered_ms_s = 0.0;
      pid_prev_valid = false;
      pid_last_calc_us = esp_timer_get_time();
      pid_last_tune_us = pid_last_calc_us;

      s.output_sync.valid = false;
      status_invalidate_sync();
      state = PLAYOUT_STOPPED;
    }

    {
      const uint32_t want_rate =
          __atomic_load_n(&s.i2s_rate_wanted, __ATOMIC_ACQUIRE);
      if (want_rate != 0U && want_rate != audio_playout_get_rate()) {
        if (!playout_flush_checked("I2S rate change")) {
          vTaskDelay(1);
          continue;
        }
        if (audio_playout_set_rate(want_rate) != ESP_OK) {
          vTaskDelay(pdMS_TO_TICKS(10));
          continue;
        }
        servo_ppm = 0;
        servo_target_ppm = 0;
        servo_seed_pending = true;
        idle_zeros = false;
        servo_generation = 0;
        center_active = false;
        pid_integral_ms_s = 0.0;
        pid_prev_error_ms = 0.0;
        pid_d_filtered_ms_s = 0.0;
        pid_prev_valid = false;
        pid_last_calc_us = esp_timer_get_time();
        pid_last_tune_us = pid_last_calc_us;
        s.output_sync.valid = false;
        status_invalidate_sync();
        state = PLAYOUT_STOPPED;
      }
    }

    const bool flush_requested =
        __atomic_exchange_n(&s.i2s_flush_requested, false, __ATOMIC_ACQ_REL);
    const bool dma_fault = audio_playout_has_fault();
    if (flush_requested || dma_fault) {
      /* Pause, FLUSH, track change, seek: silence the DMA and keep the
       * clocks running (CONFIG_AIRPLAY_I2S_KEEP_RUNNING), otherwise stop I2S
       * (clocks halted). A DMA fault always stops I2S: only a disable resets
       * the tag bookkeeping. */
      if (dma_fault ||
          !playout_keep_running_stop("pause/FLUSH/track change", &idle_zeros)) {
        if (!playout_flush_checked(dma_fault ? "dma-underrun-or-tag-fault"
                                             : "pause/FLUSH/track change")) {
          vTaskDelay(1);
          continue;
        }
        idle_zeros = false;
      }
      s.output_sync.valid = false;
      status_invalidate_sync();
      state = PLAYOUT_STOPPED;
    }

    /* Acknowledge a hard boundary only after both pending control requests
     * have been consumed and the local state machine is stopped. This keeps
     * all I2S driver ownership on this task while giving RTSP a deterministic
     * hand-off point before a new codec stream is started. */
    const uint32_t quiesce_req =
        __atomic_load_n(&s.playout_quiesce_req, __ATOMIC_ACQUIRE);
    if (quiesce_req != __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_RELAXED) &&
        state != PLAYOUT_RUNNING && audio_playout_is_enabled()) {
      /* New codec stream / session: the hand-off needs a stopped channel. */
      if (!playout_flush_checked("session/codec boundary")) {
        vTaskDelay(1);
        continue;
      }
      state = PLAYOUT_STOPPED;
      idle_zeros = false;
    }
    if (quiesce_req !=
            __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_RELAXED) &&
        state == PLAYOUT_STOPPED &&
        !audio_playout_is_enabled() &&
        !__atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&s.playout_quiesce_ack, quiesce_req, __ATOMIC_RELEASE);
    }

    timing_snapshot_t snap;
    refresh_timing_snapshot(&snap);
    if (snap.format.sample_rate > 0 &&
        (uint32_t)snap.format.sample_rate != audio_playout_get_rate()) {
      /* The rate switch at the top of the loop normally fixes this on the
       * next pass; never spin at this priority if it cannot. */
      vTaskDelay(1);
      continue;
    }
    process_i2s_completions(&snap);

    uint32_t desired_rtp = 0;
    bool timeline_ok = false;
    if (snap.playing && snap.anchor_valid && !snap.timeline_reset_pending &&
        timing_clock_ready(&snap)) {
      timeline_ok = wanted_rtp_now(&snap, &desired_rtp);
    }

    if (!timeline_ok) {
#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
      if (state == PLAYOUT_STOPPED && idle_zeros && audio_playout_is_enabled()) {
        /* Stopped with the clocks running (the DMA was silenced by the
         * stop): keep every descriptor fed with tagged zeros, an EOF without
         * a tag would be a fault. Sleep until a batch is free, then one
         * block per pass, as in the warm prime. */
        if (audio_playout_free_blocks() == 0U) {
          (void)audio_playout_wait_free(AUDIO_PLAYOUT_BATCH, pdMS_TO_TICKS(50));
        } else if (playout_write(s_silence_block, AUDIO_PLAYOUT_FRAMES, 0U, 0U) !=
                   ESP_OK) {
          (void)playout_flush_checked("idle-zeros-write-fail");
          idle_zeros = false;
          vTaskDelay(1);
        }
        continue;
      }
#endif
      /* Pause, FLUSH, track change, lost clock: silence the DMA and keep the
       * clocks running (CONFIG_AIRPLAY_I2S_KEEP_RUNNING), otherwise stop I2S
       * now (clocks halted). */
      if (audio_playout_is_enabled()) {
        const char *why = !snap.playing ? "not playing (pause/FLUSH/track change)"
            : !snap.anchor_valid ? "no anchor"
            : snap.timeline_reset_pending ? "timeline reset pending"
            : !timing_clock_ready(&snap) ? "PTP/clock map not ready"
            : "RTP position not computable";
        if (!playout_keep_running_stop(why, &idle_zeros)) {
          (void)playout_flush_checked(why);
          idle_zeros = false;
        }
      }
      state = PLAYOUT_STOPPED;
      (void)xSemaphoreTake(s.playout_wake, 1);
      continue;
    }

    /* A new buffered anchor (pause/resume, AutoMix) only changes timing, as
     * in Shairport: playback continues and the resync rules below decide.
     * Only a jump far outside the PCM window (a new RTP base) cannot be
     * followed by the playout cursor and needs a fresh PRIME. */
    if (state == PLAYOUT_RUNNING) {
      const int sr_now = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
      const int32_t ahead = rtp_delta(cursor_rtp, desired_rtp);
      if (ahead > 2 * sr_now || ahead < -2 * sr_now) {
        state = PLAYOUT_STOPPED;
        restart_reason = "re-prime: timeline jumped more than 2 s";
      }
    }

    /* Resync (Shairport resync_threshold semantics). SYNC < 0: late, drop
     * the late audio by skipping the cursor forward by the error (I2S keeps
     * running). SYNC > 0: early, re-prime and wait for the presentation
     * point. */
    if (state == PLAYOUT_RUNNING && s.output_sync.valid &&
        s.output_sync.generation == snap.generation) {
      const int32_t err_us = s.output_sync.us;
      const int32_t abs_err_us = err_us < 0 ? -err_us : err_us;
      const int64_t now_us = esp_timer_get_time();
      if (abs_err_us > AP2_RESYNC_THRESHOLD_US) {
        if (resync_over_since_us == 0) {
          resync_over_since_us = now_us;
        } else if (now_us - resync_over_since_us >= AP2_RESYNC_HOLD_US) {
          resync_count++;
          const int32_t held_ms =
              (int32_t)((now_us - resync_over_since_us) / 1000LL);
          resync_over_since_us = 0;
          if (err_us < 0) {
            const int sr_now = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
            const int32_t late_frames =
                (int32_t)((((int64_t)-err_us) * sr_now + 500000LL) / 1000000LL);
            cursor_rtp += (uint32_t)late_frames;
            pevt_post(PEVT_RESYNC, (int32_t)resync_count, err_us, held_ms,
                      servo_ppm, late_frames, NULL);
            s.output_sync.valid = false;
            status_invalidate_sync();
          } else {
            pevt_post(PEVT_RESYNC, (int32_t)resync_count, err_us, held_ms,
                      servo_ppm, 0, NULL);
            state = PLAYOUT_STOPPED; /* re-prime below; servo_ppm is kept */
            restart_reason = "re-prime: resync (early)";
          }
        }
      } else {
        resync_over_since_us = 0;
      }
    } else if (state != PLAYOUT_RUNNING) {
      resync_over_since_us = 0;
    }

    if (cursor_generation != snap.generation || state == PLAYOUT_STOPPED) {
      if (state == PLAYOUT_RUNNING)
        restart_reason = "re-prime: new generation while playing";
      const char *flush_reason = restart_reason;
      restart_reason = "prime-start";
      retune_pending = false;
      cursor_generation = snap.generation;
      servo_generation = snap.generation;
      pid_last_calc_us = esp_timer_get_time();
      pid_last_tune_us = pid_last_calc_us;
      /* Keep the learned I/frequency bias across ordinary track/anchor changes
       * inside the same session, but reset D. A hard codec/session boundary
       * is handled above and explicitly returns both hardware tune and PID to 0. */
      pid_prev_valid = false;
      pid_d_filtered_ms_s = 0.0;
      if (servo_seed_pending) {
        /* First timeline of a session: start from the clock learned for this
         * sender instead of nominal, so the first minutes are already in
         * sync rather than spent re-learning the crystal error. */
        servo_seed_pending = false;
        const int32_t seed = learned_ppm_lookup(snap.anchor_clock_id);
        if (seed != servo_ppm &&
            audio_playout_tune_ppm(seed, NULL, NULL) == ESP_OK) {
          aevt_post(AEVT_TUNE, NULL, servo_ppm, seed, 0, 0);
          servo_ppm = seed;
          const double i_lim = AP2_PID_I_TERM_LIMIT_PPM / AP2_PID_KI_PPM_PER_MS_S;
          pid_integral_ms_s = (double)seed / AP2_PID_KI_PPM_PER_MS_S;
          if (pid_integral_ms_s > i_lim) pid_integral_ms_s = i_lim;
          if (pid_integral_ms_s < -i_lim) pid_integral_ms_s = -i_lim;
        }
      }
      servo_target_ppm = servo_ppm;
      center_active = false; /* new timeline: restart slope measurement */
      center_prev_valid = false;
      s.output_sync.valid = false;
      status_invalidate_sync();
      state = PLAYOUT_PRIMING;
      warm_prime = audio_playout_is_enabled();
      if (warm_prime) {
        /* Keep the clocks: the running DMA plays zeros until the aligned
         * first block. */
        if (!idle_zeros) {
          aevt_post(AEVT_I2S_IDLE, flush_reason, (int32_t)snap.generation, 0, 0, 0);
          idle_zeros = true;
        }
      } else {
        if (!playout_flush_checked(flush_reason)) {
          state = PLAYOUT_STOPPED;
          vTaskDelay(1);
          continue;
        }
        idle_zeros = false;
      }
    }

    if (state == PLAYOUT_PRIMING && warm_prime) {
      /* WARM PRIME: I2S is running (zeros). Same alignment as the cold
       * prime, on the live DMA: queue a silent probe P and one more silent
       * block S; P's EOF timestamp is the exact start of S, so the block
       * written right after it starts one block later. */
      const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;
      uint32_t guard_frames = AP2_START_PRIME_GUARD_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      if (snap.stream_type == AUDIO_STREAM_REALTIME)
        guard_frames += (uint32_t)(((uint64_t)sr * AP2_REALTIME_PRIME_MS) / 1000ULL);
      const audio_out_sample_t *silence = s_silence_block;
      /* The first real block starts after the probe, i.e. after up to every
       * queued descriptor: check PCM from there. */
      if (!pcm_fifo_has_range(s.pcm_fifo,
                              desired_rtp + (AUDIO_PLAYOUT_DMA_BLOCKS + 1U) *
                                                AUDIO_PLAYOUT_FRAMES,
                              guard_frames, snap.pcm_generation)) {
        /* Not enough PCM yet: more zeros, one batch per wake. */
        if (audio_playout_free_blocks() == 0U) {
          (void)audio_playout_wait_free(AUDIO_PLAYOUT_BATCH, pdMS_TO_TICKS(200));
          continue;
        }
        if (playout_write(silence, AUDIO_PLAYOUT_FRAMES, 0U, 0U) != ESP_OK) {
          (void)playout_flush_checked("warm-zeros-write-fail");
          idle_zeros = false;
          state = PLAYOUT_STOPPED;
          vTaskDelay(1);
        }
        continue;
      }

      /* Probe tags: generation 0, RTP field = private counter (never a
       * generation's sync observation). */
      const uint32_t probe_id = 0x80000000U | (++warm_probe_seq & 0x7FFFFFFFU);
      if (playout_write(silence, AUDIO_PLAYOUT_FRAMES, probe_id, 0U) != ESP_OK ||
          playout_write(silence, AUDIO_PLAYOUT_FRAMES, 0U, 0U) != ESP_OK) {
        (void)playout_flush_checked("warm-probe-write-fail");
        idle_zeros = false;
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }
      const int64_t align_deadline = esp_timer_get_time() + AP2_START_ALIGN_TIMEOUT_US;
      audio_playout_completion_t probe_done;
      bool have_probe = false;
      while (esp_timer_get_time() < align_deadline) {
        if (audio_playout_wait_completion(&probe_done, 1U)) {
          if (probe_done.generation == 0U && probe_done.rtp == probe_id) {
            have_probe = true;
            break;
          }
        } else if (audio_playout_has_fault()) {
          break;
        }
      }
      if (!have_probe) {
        (void)playout_flush_checked("warm-probe-timeout");
        idle_zeros = false;
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      /* One DMA slot is free now and S is playing: the next write starts at
       * probe EOF + one block (at the current hardware clock). */
      int64_t rate_scale_ppm = 1000000LL + (int64_t)servo_ppm;
      if (rate_scale_ppm < 900000LL) rate_scale_ppm = 900000LL;
      const uint64_t block_den = (uint64_t)sr * (uint64_t)rate_scale_ppm;
      const uint64_t block_ns =
          ((uint64_t)AUDIO_PLAYOUT_FRAMES * 1000000000ULL * 1000000ULL +
           block_den / 2ULL) / block_den;
      const uint64_t boundary_ns =
          (uint64_t)completion_presentation_ns(&snap, &probe_done) + block_ns;
      const uint64_t half_sample_ns = 500000000ULL / (uint64_t)sr;

      timing_snapshot_t align_snap;
      snapshot_state(&align_snap);
      uint32_t real_start_rtp = 0;
      const bool still_valid =
          align_snap.playing && align_snap.anchor_valid &&
          !align_snap.timeline_reset_pending &&
          align_snap.generation == snap.generation &&
          (snap.stream_type != AUDIO_STREAM_BUFFERED ||
           align_snap.media_revision == snap.media_revision) &&
          !__atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) &&
          timing_clock_ready(&align_snap) &&
          (snap.stream_type != AUDIO_STREAM_REALTIME ||
           (align_snap.anchor_clock_id == snap.anchor_clock_id &&
            align_snap.anchor_ptp_ns == snap.anchor_ptp_ns &&
            align_snap.anchor_rtp == snap.anchor_rtp));
      const bool have_real =
          still_valid &&
          wanted_rtp_at_presentation_ns(&snap, boundary_ns + half_sample_ns,
                                        &real_start_rtp) &&
          pcm_fifo_read(s.pcm_fifo, real_start_rtp, AUDIO_PLAYOUT_FRAMES,
                        snap.pcm_generation, block) == AUDIO_PLAYOUT_FRAMES;
      if (!have_real) {
        /* Keep the free slot fed and try again on the next pass. */
        if (playout_write(silence, AUDIO_PLAYOUT_FRAMES, 0U, 0U) != ESP_OK) {
          (void)playout_flush_checked("warm-zeros-write-fail");
          idle_zeros = false;
          state = PLAYOUT_STOPPED;
          vTaskDelay(1);
        }
        continue;
      }

      const audio_out_sample_t *out_block =
          output_stage(block, AUDIO_PLAYOUT_FRAMES, snap.format.sample_rate,
                     volume_current_q15);
      if (playout_write(out_block, AUDIO_PLAYOUT_FRAMES,
                                     real_start_rtp, snap.generation) != ESP_OK) {
          (void)playout_flush_checked("warm-write-fail");
        idle_zeros = false;
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }
      cursor_rtp = real_start_rtp + AUDIO_PLAYOUT_FRAMES;
      state = PLAYOUT_RUNNING;
      warm_prime = false;
      idle_zeros = false;
      zero_blocks = 0;
      aevt_post(AEVT_PLAY_START, NULL, (int32_t)snap.generation,
                (int32_t)real_start_rtp, (int32_t)snap.stream_type, 1);
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
      pd_last_write_us = 0;
#endif
      if (snap.stream_type == AUDIO_STREAM_BUFFERED &&
          start_logged_gen != snap.generation &&
          s_anchor_commit_gen == snap.generation && s_anchor_commit_us != 0) {
        start_logged_gen = snap.generation;
        ptp_clock_snapshot_t ps = {0};
        ptp_clock_get_snapshot(&ps);
        pevt_post(PEVT_START, (int32_t)snap.generation,
                  (int32_t)((esp_timer_get_time() - s_anchor_commit_us) / 1000LL),
                  ps.locked ? 1 : 0, (int32_t)ps.mastership_age_ms,
                  (int32_t)ps.sample_count, NULL);
      }
      continue;
    }

    if (state == PLAYOUT_PRIMING) {
      const int sr = snap.format.sample_rate > 0 ? snap.format.sample_rate : 44100;

      /* Prime against the RTP that will actually become audible. The phase
       * probe starts AP2_START_SILENCE_FUTURE_BLOCKS blocks ahead of desired
       * RTP and consumes two silent DMA blocks before descriptor #3 can carry
       * real PCM, so checking from desired_rtp would spend almost the whole
       * short guard on the alignment procedure itself. Buffered AAC uses the
       * normal short guard from the predicted first-real boundary. Realtime
       * ALAC keeps its existing guard/recovery prime. The exact measured first
       * PCM block is still required after the silent EOF probe. */
      uint32_t guard_start = desired_rtp;
      uint32_t guard_frames =
          AP2_START_PRIME_GUARD_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      if (snap.stream_type == AUDIO_STREAM_REALTIME) {
        guard_frames += (uint32_t)(((uint64_t)sr * AP2_REALTIME_PRIME_MS) /
                                   1000ULL);
      } else {
        guard_start = desired_rtp +
                      AP2_START_REAL_BOUNDARY_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      }
      if (!pcm_fifo_has_range(s.pcm_fifo, guard_start, guard_frames,
                              snap.pcm_generation)) {
        (void)xSemaphoreTake(s.playout_wake, 1);
        continue;
      }

      const audio_out_sample_t *silence = s_silence_block;

      /* The silence tags use generation 0 on purpose: their EOFs are only a
       * phase probe and must not become the generation's public SYNC START.
       * The first real block carries the real generation and therefore becomes
       * the first normal sync observation. */
      const uint32_t silence_rtp =
          desired_rtp + AP2_START_SILENCE_FUTURE_BLOCKS * AUDIO_PLAYOUT_FRAMES;
      uint64_t silence_start_time_ns = 0;
      if (!rtp_to_presentation_ns(&snap, silence_rtp, &silence_start_time_ns)) {
        vTaskDelay(1);
        continue;
      }

      bool preload_ok = true;
      for (uint32_t k = 0; k < AUDIO_PLAYOUT_DMA_BLOCKS && preload_ok; ++k) {
        preload_ok = audio_playout_preload_tagged(
                         silence, AUDIO_PLAYOUT_FRAMES,
                         silence_rtp + k * AUDIO_PLAYOUT_FRAMES, 0U) == ESP_OK;
      }
      if (!preload_ok) {
        (void)playout_flush_checked("prime-preload-fail");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      wait_until_presentation_ns(&snap, silence_start_time_ns);

      timing_snapshot_t after_wait;
      snapshot_state(&after_wait);
      if (!after_wait.playing || !after_wait.anchor_valid ||
          after_wait.timeline_reset_pending ||
          after_wait.generation != snap.generation ||
          (snap.stream_type == AUDIO_STREAM_BUFFERED &&
           after_wait.media_revision != snap.media_revision) ||
          __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
          !timing_clock_ready(&after_wait) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (after_wait.anchor_clock_id != snap.anchor_clock_id ||
            after_wait.anchor_ptp_ns != snap.anchor_ptp_ns ||
            after_wait.anchor_rtp != snap.anchor_rtp))) {
        (void)playout_flush_checked("prime-revalidate-1");
        state = PLAYOUT_STOPPED;
        continue;
      }

      /* This is the only enable for the whole startup epoch. */
      if (audio_playout_enable() != ESP_OK) {
        (void)playout_flush_checked("prime-enable-fail");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      /* Wait for EOF of the first silent block. The other silent descriptors
       * are still queued, leaving several blocks to calculate the exact RTP
       * sample for the first real block and queue it without stopping I2S. */
      const int64_t align_deadline = esp_timer_get_time() + AP2_START_ALIGN_TIMEOUT_US;
      audio_playout_completion_t probe_done;
      bool have_probe = false;
      while (esp_timer_get_time() < align_deadline) {
        if (audio_playout_wait_completion(&probe_done, 1U)) {
          if (probe_done.generation == 0U && probe_done.rtp == silence_rtp) {
            have_probe = true;
            break;
          }
          /* No real-generation completion can exist yet. Ignore any
           * stale completion left over from a prior disabled epoch. */
        } else if (audio_playout_has_fault()) {
          break;
        }
      }
      if (!have_probe) {
        (void)playout_flush_checked("prime-probe-timeout");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      /* Convert the measured EOF edge into the stream presentation clock.
       * For realtime ALAC this stays as the ISR's ESP-local timestamp; AAC
       * keeps the original local+PTP-offset conversion. The first real block
       * starts AUDIO_PLAYOUT_DMA_BLOCKS - 1 blocks after the first silent EOF:
       * the remaining silent blocks are queued in front of it. */
      const int64_t probe_done_time_ns = completion_presentation_ns(&snap, &probe_done);
      int64_t rate_scale_ppm = 1000000LL + (int64_t)servo_ppm;
      if (rate_scale_ppm < 900000LL) rate_scale_ppm = 900000LL;
      const uint64_t block_den = (uint64_t)sr * (uint64_t)rate_scale_ppm;
      const uint64_t block_ns =
          ((uint64_t)AUDIO_PLAYOUT_FRAMES * 1000000000ULL * 1000000ULL +
           block_den / 2ULL) / block_den;
      uint64_t real_boundary_time_ns =
          probe_done_time_ns > 0
              ? (uint64_t)probe_done_time_ns +
                    (uint64_t)(AUDIO_PLAYOUT_DMA_BLOCKS - 1U) * block_ns
              : silence_start_time_ns +
                    (uint64_t)AUDIO_PLAYOUT_DMA_BLOCKS * block_ns;

      /* Round the measured physical I2S boundary to the nearest RTP sample.
       * This keeps startup alignment resolution at one sample (22.68 us at
       * 44.1 kHz) without a diagnostic/manual presentation offset. */
      const uint64_t half_sample_ns = 500000000ULL / (uint64_t)sr;
      uint32_t real_start_rtp = 0;
      if (!wanted_rtp_at_presentation_ns(&snap,
                                           real_boundary_time_ns + half_sample_ns,
                                           &real_start_rtp)) {
        (void)playout_flush_checked("prime-map-real");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      /* Revalidate the timeline after waiting for the probe EOF. */
      timing_snapshot_t align_snap;
      snapshot_state(&align_snap);
      if (!align_snap.playing || !align_snap.anchor_valid ||
          align_snap.timeline_reset_pending ||
          align_snap.generation != snap.generation ||
          (snap.stream_type == AUDIO_STREAM_BUFFERED &&
           align_snap.media_revision != snap.media_revision) ||
          __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
          !timing_clock_ready(&align_snap) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (align_snap.anchor_clock_id != snap.anchor_clock_id ||
            align_snap.anchor_ptp_ns != snap.anchor_ptp_ns ||
            align_snap.anchor_rtp != snap.anchor_rtp))) {
        (void)playout_flush_checked("prime-revalidate-2");
        state = PLAYOUT_STOPPED;
        continue;
      }

      bool ok = pcm_fifo_read(s.pcm_fifo, real_start_rtp, AUDIO_PLAYOUT_FRAMES,
                              snap.pcm_generation, block) == AUDIO_PLAYOUT_FRAMES;
      if (!ok) {
        /* Do not allow the already-running silent probe to leak into audible
         * timing indefinitely. A miss restarts the one-enable alignment epoch
         * after more PCM has arrived. */
        (void)playout_flush_checked("prime-pcm-miss");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }
      const audio_out_sample_t *out_block =
          output_stage(block, AUDIO_PLAYOUT_FRAMES, snap.format.sample_rate,
                     volume_current_q15);
      const esp_err_t first_err = playout_write(out_block, AUDIO_PLAYOUT_FRAMES,
                                                real_start_rtp, snap.generation);
      if (first_err != ESP_OK) {
        (void)playout_flush_checked("prime-write-fail");
        state = PLAYOUT_STOPPED;
        vTaskDelay(1);
        continue;
      }

      /* The first real write is blocking. Control/Core0 can publish a pause,
       * FLUSH or a newer buffered anchor while Core1 is inside the driver.
       * Revalidate once more before making this startup public as RUNNING.
       * The real block may have reached DMA, but a cancelled epoch is flushed
       * immediately and never becomes the accepted playout generation. */
      timing_snapshot_t commit_snap;
      snapshot_state(&commit_snap);
      if (!commit_snap.playing || !commit_snap.anchor_valid ||
          commit_snap.timeline_reset_pending ||
          commit_snap.generation != snap.generation ||
          (snap.stream_type == AUDIO_STREAM_BUFFERED &&
           commit_snap.media_revision != snap.media_revision) ||
          __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
          __atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE) ||
          !timing_clock_ready(&commit_snap) ||
          (snap.stream_type == AUDIO_STREAM_REALTIME &&
           (commit_snap.anchor_clock_id != snap.anchor_clock_id ||
            commit_snap.anchor_ptp_ns != snap.anchor_ptp_ns ||
            commit_snap.anchor_rtp != snap.anchor_rtp))) {
        (void)playout_flush_checked("prime-commit-cancel");
        s.output_sync.valid = false;
        state = PLAYOUT_STOPPED;
        continue;
      }

      cursor_rtp = real_start_rtp + AUDIO_PLAYOUT_FRAMES;
      state = PLAYOUT_RUNNING;
      idle_zeros = false;
      zero_blocks = 0;
      aevt_post(AEVT_PLAY_START, NULL, (int32_t)snap.generation,
                (int32_t)real_start_rtp, (int32_t)snap.stream_type, 0);
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
      pd_last_write_us = 0;
#endif
      if (snap.stream_type == AUDIO_STREAM_BUFFERED &&
          start_logged_gen != snap.generation &&
          s_anchor_commit_gen == snap.generation && s_anchor_commit_us != 0) {
        start_logged_gen = snap.generation;
        ptp_clock_snapshot_t ps = {0};
        ptp_clock_get_snapshot(&ps);
        pevt_post(PEVT_START, (int32_t)snap.generation,
                  (int32_t)((esp_timer_get_time() - s_anchor_commit_us) / 1000LL),
                  ps.locked ? 1 : 0, (int32_t)ps.mastership_age_ms,
                  (int32_t)ps.sample_count, NULL);
      }
      continue;
    }

    /* In RUNNING, cursor_rtp is the next future block to queue. The DMA
     * descriptors are paced by their EOF interrupts; the ISR completion tags
     * are the source of truth for sync. Sleep until AUDIO_PLAYOUT_BATCH
     * descriptors are free, then queue that many blocks without blocking:
     * Core1 wakes once per batch instead of once per descriptor. The loop top
     * (FLUSH, timeline, completions) runs again after every wake. */
    if (audio_playout_free_blocks() == 0U) {
      (void)audio_playout_wait_free(AUDIO_PLAYOUT_BATCH, pdMS_TO_TICKS(200));
      continue;
    }
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
    const int64_t pd_work_start_us = esp_timer_get_time();
#endif
    /* Plain copy from the PCM FIFO; frames it does not hold (a lost packet,
     * audio not decoded yet) come back as silence. */
    const uint32_t got = pcm_fifo_read(s.pcm_fifo, cursor_rtp,
                                       AUDIO_PLAYOUT_FRAMES,
                                       snap.pcm_generation, block);
    const bool have_pcm = got > 0U;
    if (have_pcm && got < AUDIO_PLAYOUT_FRAMES) {
      const uint32_t missing = AUDIO_PLAYOUT_FRAMES - got;
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
      PD_ADD(conceal_blocks, 1U);
      PD_ADD(conceal_frames, missing);
#endif
      aevt_post(AEVT_CONCEAL, NULL, (int32_t)cursor_rtp, (int32_t)missing,
                0, 0);
    }
    if (!have_pcm) {
      /* A stop (FLUSH, track change) may have cleared the PCM just now. Do
       * not send that gap as silence while I2S still runs: stop I2S first
       * (top of the loop). A real underrun while playing has no stop
       * request and is still played as silence. */
      if (__atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
          __atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE))
        continue;
      memset(block, 0, AUDIO_PLAYOUT_FRAMES * 2U * sizeof(float));
      PD_ADD(silent_blocks, 1U);
      if (zero_blocks++ == 0U)
        aevt_post(AEVT_SILENCE_BEGIN, NULL, (int32_t)cursor_rtp, 0, 0, 0);
    } else if (zero_blocks != 0U) {
      aevt_post(AEVT_SILENCE_END, NULL, (int32_t)cursor_rtp,
                (int32_t)zero_blocks, snap.format.sample_rate, 0);
      zero_blocks = 0;
    }
    timing_snapshot_t current;
    snapshot_state(&current);
    if (current.generation != snap.generation ||
        current.format_generation != snap.format_generation) continue;
    const audio_out_sample_t *out_block =
        output_stage(block, AUDIO_PLAYOUT_FRAMES, snap.format.sample_rate,
                     volume_current_q15);
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
    PD_MAX(max_work_us, esp_timer_get_time() - pd_work_start_us);
#endif

    const esp_err_t write_err = playout_write(
        out_block, AUDIO_PLAYOUT_FRAMES, cursor_rtp, snap.generation);
    /* A clock retune decided by the servo: one register write, I2S and the
     * DMA keep running. */
    if (write_err == ESP_OK && retune_pending) {
      retune_pending = false;
      audio_playout_tune_info_t ti = {0};
      uint32_t halt_us = 0;
      const esp_err_t te = audio_playout_tune_ppm(retune_ppm, &ti, &halt_us);
      if (te == ESP_OK) {
        aevt_post(AEVT_TUNE, NULL, servo_ppm, retune_ppm, (int32_t)halt_us, 0);
        servo_ppm = retune_ppm;
        tune_fail_count = 0;
      } else {
        tune_fail_count++;
        pevt_post(PEVT_TUNE_FAIL, retune_ppm, (int32_t)te,
                  (int32_t)tune_fail_count, 0, 0, NULL);
      }
    }
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
    {
      /* Time between consecutive running writes. Cleared when RUNNING
       * starts, so the first write after a (re)start is not a gap. */
      const int64_t pd_now = esp_timer_get_time();
      if (write_err == ESP_OK) {
        if (pd_last_write_us != 0)
          PD_MAX(max_gap_us, pd_now - pd_last_write_us);
        pd_last_write_us = pd_now;
      } else {
        pd_last_write_us = 0;
        PD_ADD(write_fails, 1U);
      }
    }
#endif
    process_i2s_completions(&snap);

    /* PID clock servo.
     *
     * Sign convention from tagged DMA EOF:
     *   SYNC < 0 : ESP is late  -> positive ppm (speed I2S up)
     *   SYNC > 0 : ESP is early -> negative pressure on ppm
     *
     * The PID math and the clock retune run every 1 s. A retune is a live
     * write of the fractional divider, dithered per block so every integer
     * ppm is exact on average; I2S and the DMA/tag chronology keep running.
     * +/-AP2_PID_DEADBAND_US is treated as GOOD: once there and phase
     * velocity is modest, the PID stops acting and the centring loop
     * (AP2_CENTER_*) steers towards 0 ms with small steps, based on a
     * least-squares slope of the measured sync.
     */
    const int64_t pid_now_us = esp_timer_get_time();
    if (write_err == ESP_OK && s.output_sync.valid &&
        s.output_sync.generation == snap.generation &&
        servo_generation == snap.generation &&
        pid_now_us - pid_last_calc_us >= AP2_PID_CALC_PERIOD_US) {
      const double dt_s = (double)(pid_now_us - pid_last_calc_us) / 1000000.0;
      pid_last_calc_us = pid_now_us;

      /* Controller error is opposite to SYNC: negative SYNC (late) must
       * request positive ppm. */
      const double sync_ms = (double)s.output_sync.us / 1000.0;
      const double error_ms = -sync_ms;

      double d_raw_ms_s = 0.0;
      if (pid_prev_valid && dt_s > 0.001) {
        d_raw_ms_s = (error_ms - pid_prev_error_ms) / dt_s;
        pid_d_filtered_ms_s += AP2_PID_D_ALPHA *
            (d_raw_ms_s - pid_d_filtered_ms_s);
      } else {
        pid_d_filtered_ms_s = 0.0;
      }
      pid_prev_error_ms = error_ms;
      pid_prev_valid = true;
      /* Keep log sign intuitive: positive d means SYNC is moving upward. */

      const int32_t abs_sync_us = s.output_sync.us < 0
          ? -s.output_sync.us : s.output_sync.us;
      const bool in_deadband = abs_sync_us <= AP2_PID_DEADBAND_US;

      /* In the soft band (deadband..softband) reduce proportional
       * aggression; outside it use full P. This prevents unnecessary hunting
       * once we are already close. */
      double p_error_ms = error_ms;
      if (abs_sync_us > AP2_PID_DEADBAND_US &&
          abs_sync_us <= AP2_PID_SOFTBAND_US) {
        p_error_ms *= 0.45;
      }

      const double p_term = AP2_PID_KP_PPM_PER_MS * p_error_ms;
      const double d_term = AP2_PID_KD_PPM_PER_MS_PER_S * pid_d_filtered_ms_s;

      /* Candidate integral update with anti-windup.  We integrate outside the
       * good zone.  If P+I+D is already saturated in the same direction
       * as the error, freeze I; if the error would pull us out of saturation,
       * allow it to unwind. */
      double i_term = AP2_PID_KI_PPM_PER_MS_S * pid_integral_ms_s;
      double unsat = p_term + i_term + d_term;
      bool allow_i = !in_deadband && abs_sync_us <= AP2_PID_SOFTBAND_US;
      if (allow_i) {
        if ((unsat >= AP2_PID_MAX_PPM && error_ms > 0.0) ||
            (unsat <= -AP2_PID_MAX_PPM && error_ms < 0.0)) {
          allow_i = false;
        }
        /* Already converging: error and its derivative have opposite signs. */
        if ((error_ms > 0.0) != (pid_d_filtered_ms_s > 0.0) &&
            (pid_d_filtered_ms_s > AP2_PID_CONVERGING_MS_S ||
             pid_d_filtered_ms_s < -AP2_PID_CONVERGING_MS_S)) {
          allow_i = false;
        }
      }
      if (allow_i) {
        pid_integral_ms_s += error_ms * dt_s;
        const double i_limit_state = AP2_PID_I_TERM_LIMIT_PPM /
                                     AP2_PID_KI_PPM_PER_MS_S;
        if (pid_integral_ms_s > i_limit_state) pid_integral_ms_s = i_limit_state;
        if (pid_integral_ms_s < -i_limit_state) pid_integral_ms_s = -i_limit_state;
        i_term = AP2_PID_KI_PPM_PER_MS_S * pid_integral_ms_s;
      }

      double command = p_term + i_term + d_term;
      if (!in_deadband) {
        /* Phase recovery (1 ppm = 1 us/s of sync slope). */
        double recover_us_s = -(double)s.output_sync.us / AP2_RECOVER_TIME_S;
        if (recover_us_s > AP2_RECOVER_MAX_SLOPE_US_S)
          recover_us_s = AP2_RECOVER_MAX_SLOPE_US_S;
        if (recover_us_s < -AP2_RECOVER_MAX_SLOPE_US_S)
          recover_us_s = -AP2_RECOVER_MAX_SLOPE_US_S;
        command = i_term + recover_us_s;
      }
      if (command > AP2_PID_MAX_PPM) command = AP2_PID_MAX_PPM;
      if (command < -AP2_PID_MAX_PPM) command = -AP2_PID_MAX_PPM;
      int32_t next_target = (int32_t)(command >= 0.0 ? command + 0.5 : command - 0.5);

      /* GOOD zone: if phase is not racing through it, keep the learned clock.
       * D still remains alive, so a clear passage through the band will be seen
       * on the next calculation rather than being hidden forever. */
      const double sync_slope_ms_s = -pid_d_filtered_ms_s;
      /* Hysteresis around the centring mode. */
      if (center_active) {
        if (abs_sync_us > AP2_CENTER_EXIT_HARD_US) {
          center_active = false;
        } else if (abs_sync_us > AP2_CENTER_EXIT_US) {
          if (center_out_since_us == 0) {
            center_out_since_us = pid_now_us;
          } else if (pid_now_us - center_out_since_us >= AP2_CENTER_EXIT_HOLD_US) {
            center_active = false;
          }
        } else {
          center_out_since_us = 0;
        }
      }
      const bool center_enter = !center_active && in_deadband &&
          sync_slope_ms_s > -0.080 && sync_slope_ms_s < 0.080;
      if (center_active || center_enter) {
        /* Hold the learned clock, then centre slowly. */
        bool restart_window = false;
        if (center_enter) {
          next_target = servo_ppm; /* entering the zone: drop pending PID step */
          center_active = true;
          center_out_since_us = 0;
          restart_window = true;
          /* PID frequency memory = the clock we are now holding. */
          pid_integral_ms_s = (double)servo_ppm / AP2_PID_KI_PPM_PER_MS_S;
          const double i_lim = AP2_PID_I_TERM_LIMIT_PPM / AP2_PID_KI_PPM_PER_MS_S;
          if (pid_integral_ms_s > i_lim) pid_integral_ms_s = i_lim;
          if (pid_integral_ms_s < -i_lim) pid_integral_ms_s = -i_lim;
        } else {
          next_target = servo_target_ppm;
          if (servo_target_ppm != servo_ppm) {
            /* A step is still waiting for the retune slot: measure the
             * slope only once the clock really runs at the new rate. */
            restart_window = true;
          } else {
            if (center_prev_valid &&
                (s.output_sync.us - center_prev_sync_us > AP2_CENTER_JUMP_US ||
                 center_prev_sync_us - s.output_sync.us > AP2_CENTER_JUMP_US)) {
              /* A reference step, not drift: keep it out of the slope. */
              center_jump_us += s.output_sync.us - center_prev_sync_us;
            }
            const double t_s = (double)(pid_now_us - center_start_us) / 1000000.0;
            const double y = (double)(s.output_sync.us - center_jump_us);
            center_n += 1.0; center_st += t_s; center_sy += y;
            center_stt += t_s * t_s; center_sty += t_s * y;
          }
          const double den = center_n * center_stt - center_st * center_st;
          const int64_t fit_span_us = pid_now_us - center_start_us;
          if (!restart_window && center_n >= 5.0 && den > 0.0 &&
              fit_span_us >= AP2_CENTER_MIN_FIT_US) {
            const double slope_us_s = (center_n * center_sty - center_st * center_sy) / den;
            const double icpt = (center_sy - slope_us_s * center_st) / center_n;
            const double t_end = (double)(pid_now_us - center_start_us) / 1000000.0;
            const int32_t sync_fit_us =
                (int32_t)(icpt + slope_us_s * t_end) + center_jump_us;
            const int32_t step = servo_center_step(sync_fit_us, slope_us_s);
            if (step != 0) {
              int32_t t = servo_ppm + step;
              if (t > AP2_PID_MAX_PPM) t = AP2_PID_MAX_PPM;
              if (t < -AP2_PID_MAX_PPM) t = -AP2_PID_MAX_PPM;
              next_target = t;
              /* Keep the PID's frequency memory consistent with the learned
               * clock, so leaving the zone does not jump back. */
              pid_integral_ms_s = (double)t / AP2_PID_KI_PPM_PER_MS_S;
              const double i_limit_state = AP2_PID_I_TERM_LIMIT_PPM /
                                           AP2_PID_KI_PPM_PER_MS_S;
              if (pid_integral_ms_s > i_limit_state) pid_integral_ms_s = i_limit_state;
              if (pid_integral_ms_s < -i_limit_state) pid_integral_ms_s = -i_limit_state;
              pevt_post(PEVT_CENTER, sync_fit_us,
                        (int32_t)(slope_us_s * 10.0), t, 0, 0, NULL);
              /* The slope changes with the step: fit it afresh. */
              restart_window = true;
            } else if (fit_span_us >= AP2_CENTER_WINDOW_US) {
              /* On target: start a fresh fit, so old samples do not hide a
               * slow change of the crystal. */
              restart_window = true;
            }
          }
        }
        if (restart_window) {
          center_start_us = pid_now_us;
          center_n = center_st = center_sy = center_stt = center_sty = 0.0;
          center_jump_us = 0;
        }
        center_prev_sync_us = s.output_sync.us;
        center_prev_valid = true;
      }
      servo_target_ppm = next_target;
      /* In the good zone with the clock held, the running ppm is the learned
       * correction for this sender. */
      if (center_active && in_deadband && servo_target_ppm == servo_ppm)
        learned_ppm_store(snap.anchor_clock_id, servo_ppm);
      status_publish_sync(s.output_sync.us, robust_phase_center_us(&s.output_sync),
                          servo_ppm, snap.generation);
    }


    if (write_err == ESP_OK && s.output_sync.valid &&
        s.output_sync.generation == snap.generation &&
        servo_generation == snap.generation &&
        pid_now_us - pid_last_tune_us >= AP2_PID_TUNE_PERIOD_US) {
      int32_t delta = servo_target_ppm - servo_ppm;
      int32_t abs_delta = delta < 0 ? -delta : delta;
      if (abs_delta >= AP2_PID_MIN_TUNE_PPM) {
        if (delta > AP2_PID_MAX_JUMP_PPM) delta = AP2_PID_MAX_JUMP_PPM;
        if (delta < -AP2_PID_MAX_JUMP_PPM) delta = -AP2_PID_MAX_JUMP_PPM;
        const int32_t next_ppm = servo_ppm + delta;

        /* A single live divider write, right after the next DMA write
         * (see above). */
        pid_last_tune_us = pid_now_us;
        retune_pending = true;
        retune_ppm = next_ppm;
      }
    }

    if (write_err == ESP_OK) {
      cursor_rtp += AUDIO_PLAYOUT_FRAMES;
    } else {
      /* A failed write breaks the exact tag<->descriptor FIFO relationship.
       * Flush and re-prime rather than pretending the software cursor moved. */
      (void)playout_flush_checked("running-write-fail");
      s.output_sync.valid = false;
      state = PLAYOUT_STOPPED;
      vTaskDelay(1);
    }
  }

  (void)playout_flush_checked("task-exit");
  free(block);
  s.playout_task = NULL;
  vTaskDelete(NULL);
}




esp_err_t audio_receiver_init(void) {
  ESP_RETURN_ON_ERROR(realtime_receiver_reserve_data_task_memory(), TAG,
                      "ALAC data task memory reservation failed");
  {
    int32_t lat = 0;
    if (settings_get_output_latency_us(&lat) == ESP_OK &&
        lat >= AP2_OUTPUT_LATENCY_MIN_US && lat <= AP2_OUTPUT_LATENCY_MAX_US) {
      __atomic_store_n(&s_output_latency_us, lat, __ATOMIC_RELAXED);
    }
  }
  if (!s.publish_mutex) s.publish_mutex = xSemaphoreCreateMutex();
  if (!s_buffered_control.mutex) s_buffered_control.mutex = xSemaphoreCreateMutex();
  if (!s.status_wake) s.status_wake = xSemaphoreCreateBinary();
  if (!s.playout_wake) s.playout_wake = xSemaphoreCreateBinary();
  if (!s.publish_mutex || !s_buffered_control.mutex || !s.status_wake ||
      !s.playout_wake) return ESP_ERR_NO_MEM;
  if (!s.playout_timer) {
    const esp_timer_create_args_t timer_args = {
        .callback = playout_timer_callback,
        .name = "audio_start",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s.playout_timer), TAG,
                        "playout timer create failed");
  }

  /* Allocate the one large codec backing store first while PSRAM is least
   * fragmented. Buffered AAC uses the full configured store as its contiguous
   * circular byte buffer.
   * Realtime ALAC reuses the beginning of the same bytes for its raw PCM ring
   * and DATA/RTX pools, but only after the buffered transport is fully idle. */
  if (!s.codec_workspace) {
    s.codec_workspace_size = AP2_BUFFERED_STORE_REQUEST_BYTES;
    s.codec_workspace = heap_caps_malloc(
        s.codec_workspace_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.codec_workspace) s.codec_workspace = malloc(s.codec_workspace_size);
    if (!s.codec_workspace) return ESP_ERR_NO_MEM;
  }

  if (!s.buffered_packet) {
    s.buffered_packet = heap_caps_malloc(AP2_PACKET_MAX,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.buffered_packet) s.buffered_packet = malloc(AP2_PACKET_MAX);
  }

  if (!s.decrypt_buf) {
    s.decrypt_buf = heap_caps_malloc(AP2_PACKET_MAX + AAC_DECODER_INPUT_HEADROOM,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.decrypt_buf) {
      s.decrypt_buf = malloc(AP2_PACKET_MAX + AAC_DECODER_INPUT_HEADROOM);
    }
  }
  if (!s.decode_pcm) {
    s.decode_pcm = heap_caps_malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.decode_pcm) {
      s.decode_pcm = malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(int16_t));
    }
  }
  /* Realtime ALAC16 staging retains its existing int16 allocation. */
  s.realtime_stage_pcm = s.decode_pcm;
  if (!s.pcm_f32) {
    /* One decoded block as float (8 KiB). PSRAM: EQ walks it through the
     * cache and internal RAM is the scarcer resource. */
    s.pcm_f32 = heap_caps_malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(float),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.pcm_f32) s.pcm_f32 = malloc(AP2_PCM_CAPACITY_FRAMES * 2U * sizeof(float));
  }

  if (!s.transport) {
    ap2_buffered_fifo_config_t tcfg = {
        .buffer_bytes = AP2_BUFFERED_STORE_REQUEST_BYTES,
        .task_core = AP2_AAC_RX_CORE,
        .task_priority = AP2_RX_PRIORITY,
        .task_stack = AP2_RX_STACK,
    };
    ESP_RETURN_ON_ERROR(ap2_buffered_fifo_create_with_storage(
                            &s.transport, &tcfg, s.codec_workspace,
                            s.codec_workspace_size),
                        TAG, "buffered FIFO create failed");
  }

  if (!s.pcm_fifo && pcm_fifo_create(&s.pcm_fifo) != ESP_OK) {
    return ESP_ERR_NO_MEM;
  }

  const size_t rt_stage_bytes = pcm_rtp_ring_storage_bytes();
  const size_t rt_pool_bytes = realtime_receiver_packet_workspace_size();
  if (rt_stage_bytes > s.codec_workspace_size ||
      rt_pool_bytes > s.codec_workspace_size - rt_stage_bytes) {
    ESP_LOGE(TAG,
             "shared codec workspace too small: have=%u KiB ALAC needs=%u KiB",
             (unsigned)(s.codec_workspace_size / 1024U),
             (unsigned)((rt_stage_bytes + rt_pool_bytes) / 1024U));
    return ESP_ERR_NO_MEM;
  }

  if (!s.realtime_stage_ring &&
      pcm_rtp_ring_create_with_storage(
          &s.realtime_stage_ring, s.codec_workspace, rt_stage_bytes) != ESP_OK) {
    return ESP_ERR_NO_MEM;
  }
  if (!s.realtime_workspace_bound) {
    ESP_RETURN_ON_ERROR(
        realtime_receiver_set_packet_workspace(
            s.codec_workspace + rt_stage_bytes,
            s.codec_workspace_size - rt_stage_bytes),
        TAG, "ALAC shared packet workspace bind failed");
    s.realtime_workspace_bound = true;
  }
  if (!s.buffered_packet || !s.decrypt_buf || !s.decode_pcm ||
      !s.realtime_stage_pcm || !s.pcm_f32 || !s.pcm_fifo ||
      !s.realtime_stage_ring) {
    return ESP_ERR_NO_MEM;
  }

  pcm_fifo_set_generation(s.pcm_fifo, s.buffered_pcm_generation);
  pcm_rtp_ring_set_generation(s.realtime_stage_ring, s.generation);

  ESP_RETURN_ON_ERROR(audio_playout_init(), TAG, "I2S playout init failed");
  __atomic_store_n(&s.engine_running, true, __ATOMIC_RELEASE);
  if (!s.status_task &&
      xTaskCreatePinnedToCoreWithCaps(
          audio_status_task, "audio_status", AP2_STATUS_STACK, NULL,
          AP2_STATUS_PRIORITY, &s.status_task, AP2_STATUS_CORE,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGW(TAG, "Task start failed: audio_status PSRAM stack=%u bytes",
             (unsigned)AP2_STATUS_STACK);
  }
  if (!s.playout_task) {
    if (xTaskCreatePinnedToCore(ap2_playout_task, "ap2_playout",
                                AP2_PLAYOUT_STACK, NULL, AP2_PLAYOUT_PRIORITY,
                                &s.playout_task, AP2_PLAYOUT_CORE) != pdPASS) {
      return ESP_FAIL;
    }
    AUDIO_DIAG_LIFECYCLE_TASK_STARTED(
        AUDIO_DIAG_TASK_PLAYOUT, AP2_PLAYOUT_CORE, AP2_PLAYOUT_PRIORITY,
        AUDIO_PLAYOUT_FRAMES);
  }
  if (!s.realtime_stage_task) {
    if (xTaskCreatePinnedToCore(realtime_stage_task, "alac_stage",
                                AP2_RT_STAGE_STACK, NULL, AP2_RT_STAGE_PRIORITY,
                                &s.realtime_stage_task, AP2_PLAYOUT_CORE) != pdPASS) {
      return ESP_FAIL;
    }
  }

  return ESP_OK;
}

bool audio_receiver_is_initialized(void) {
  return __atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE) && s.playout_task && s.realtime_stage_task &&
         s.codec_workspace && s.transport && s.pcm_fifo && s.realtime_stage_ring;
}

esp_err_t audio_receiver_release_for_wifi_scan(void) {
  if (!__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE) && !s.codec_workspace && !s.transport) return ESP_OK;

  /* First reach the same hard media boundary used for codec/session changes.
   * The RTSP server is stopped by the caller, so no new producer can appear
   * while the stores are being dismantled. */
  audio_receiver_stop();

  if (s.processor_task || !realtime_receiver_is_idle() ||
      (s.transport && !ap2_buffered_fifo_is_idle(s.transport))) {
    ESP_LOGE(TAG, "WiFi scan release refused: media producer still active");
    return ESP_ERR_TIMEOUT;
  }

  /* Stop the long-lived workers before freeing any object they can observe. */
  __atomic_store_n(&s.engine_running, false, __ATOMIC_RELEASE);
  __atomic_store_n(&s.realtime_stage_running, false, __ATOMIC_RELEASE);
  audio_status_notify();
  playout_wake();
  realtime_stage_kick();
  if (s.playout_timer) (void)esp_timer_stop(s.playout_timer);

  for (int i = 0; (s.playout_task || s.realtime_stage_task || s.status_task) &&
                  i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s.playout_task || s.realtime_stage_task || s.status_task) {
    ESP_LOGE(TAG,
             "WiFi scan release timed out: playout=%p stage=%p status=%p",
             (void *)s.playout_task, (void *)s.realtime_stage_task,
             (void *)s.status_task);
    /* No large allocation has been freed yet. Restore the normal engine
     * lifecycle rather than returning a half-stopped receiver to the web
     * server. */
    (void)audio_receiver_init();
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t err = realtime_receiver_clear_packet_workspace();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to detach realtime shared workspace: %s",
             esp_err_to_name(err));
    (void)audio_receiver_init();
    return err;
  }
  s.realtime_workspace_bound = false;

  if (s.transport) {
    err = ap2_buffered_fifo_destroy(s.transport);
    if (err != ESP_OK) {
      /* destroy() leaves the FIFO object intact when its reader still owns
       * the shared backing store. Rebind/restart the engine and, critically,
       * do not free codec_workspace underneath that reader. */
      ESP_LOGE(TAG, "WiFi scan release: FIFO destroy failed: %s",
               esp_err_to_name(err));
      (void)audio_receiver_init();
      return err;
    }
    s.transport = NULL;
  }
  if (s.realtime_stage_ring) {
    pcm_rtp_ring_destroy(s.realtime_stage_ring);
    s.realtime_stage_ring = NULL;
  }
  if (s.pcm_fifo) {
    pcm_fifo_destroy(s.pcm_fifo);
    s.pcm_fifo = NULL;
  }

  free(s.buffered_packet);
  s.buffered_packet = NULL;
  free(s.decrypt_buf);
  s.decrypt_buf = NULL;
  free(s.decode_pcm);
  s.decode_pcm = NULL;
  /* Alias of decode_pcm: one allocation and one free. */
  s.realtime_stage_pcm = NULL;
  free(s.pcm_f32);
  s.pcm_f32 = NULL;
  free(s.codec_workspace);
  s.codec_workspace = NULL;
  s.codec_workspace_size = 0;

  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  s.anchor_valid = false;
  s.anchor_local_valid = false;
  s.realtime_stage_cursor_valid = false;
  s.stream_type = AUDIO_STREAM_NONE;
  s.port = 0;
  taskEXIT_CRITICAL(&s.state_mux);

  AP_LOGI(TAG, "Audio memory released for WiFi scan");
  return ESP_OK;
}

static bool realtime_stage_stop_and_wait(void) {
  taskENTER_CRITICAL(&s.state_mux);
  __atomic_store_n(&s.realtime_stage_running, false, __ATOMIC_RELEASE);
  taskEXIT_CRITICAL(&s.state_mux);
  realtime_stage_kick();
  for (int i = 0; s.realtime_stage_task &&
                  !__atomic_load_n(&s.realtime_stage_idle, __ATOMIC_ACQUIRE) &&
                  i < 20; ++i) {
    vTaskDelay(1);
  }
  const bool idle = !s.realtime_stage_task ||
      __atomic_load_n(&s.realtime_stage_idle, __ATOMIC_ACQUIRE);
  if (!idle) AP_LOGW(TAG, "ALAC staging stop timed out; codec start refused");
  return idle;
}

bool audio_receiver_is_idle(void) {
  return !__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE) && !s.processor_task &&
         (!s.transport || ap2_buffered_fifo_is_idle(s.transport)) &&
         realtime_receiver_is_idle() &&
         (!s.realtime_stage_task ||
          __atomic_load_n(&s.realtime_stage_idle, __ATOMIC_ACQUIRE)) &&
         __atomic_load_n(&s.playout_quiesce_req, __ATOMIC_ACQUIRE) ==
             __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE);
}

void audio_receiver_set_format(const audio_format_t *f) {
  if (!f) {
    return;
  }
  if (f->sample_rate > 0) audio_eq_prepare(f->sample_rate);
  taskENTER_CRITICAL(&s.state_mux);
  const audio_format_t previous = s.format;
  s.format = *f;
  s.format_generation++;
  if (s.format_generation == 0) {
    s.format_generation = 1;
  }
  if (f->sample_rate == 44100 || f->sample_rate == 48000) {
    __atomic_store_n(&s.i2s_rate_wanted, (uint32_t)f->sample_rate,
                     __ATOMIC_RELEASE);
  }
  taskEXIT_CRITICAL(&s.state_mux);
  if (strcmp(previous.codec, f->codec) != 0 ||
      previous.sample_rate != f->sample_rate ||
      previous.bits_per_sample != f->bits_per_sample ||
      previous.channels != f->channels || previous.frame_size != f->frame_size) {
    char previous_pcm[16], next_pcm[16];
    snprintf(previous_pcm, sizeof(previous_pcm), "S%d", previous.bits_per_sample);
    snprintf(next_pcm, sizeof(next_pcm), "S%d", f->bits_per_sample);
    ESP_LOGI(TAG, "AUDIO PIPELINE %s/%d Hz/%s/%d ch/%d frames -> "
                  "%s/%d Hz/%s/%d ch/%d frames",
             previous.codec[0] ? previous.codec : "none", previous.sample_rate,
             strcmp(previous.codec, "AAC") == 0 ? "F24" : previous_pcm,
             previous.channels, previous.frame_size,
             f->codec, f->sample_rate,
             strcmp(f->codec, "AAC") == 0 ? "F24" : next_pcm, f->channels,
             f->frame_size);
  }
  media_control_wake();
  audio_status_notify();
}

uint32_t audio_receiver_get_sample_rate(void) {
  taskENTER_CRITICAL(&s.state_mux);
  const int rate = s.format.sample_rate;
  taskEXIT_CRITICAL(&s.state_mux);
  return rate > 0 ? (uint32_t)rate : AUDIO_PIPELINE_RATE_HZ;
}

void audio_receiver_set_encryption(const audio_encrypt_t *e) {
  audio_encrypt_t next = {0};
  if (e) next = *e;
  taskENTER_CRITICAL(&s.state_mux);
  s.encrypt = next;
  taskEXIT_CRITICAL(&s.state_mux);
  AP_LOGI(TAG, "ENCRYPT type=%d key_len=%u", (int)next.type,
           (unsigned)next.key_len);
}

void audio_receiver_set_stream_type(audio_stream_type_t t) {
  bool changed;
  taskENTER_CRITICAL(&s.state_mux);
  changed = s.stream_type != t;
  s.stream_type = t;
  taskEXIT_CRITICAL(&s.state_mux);
  if (changed) audio_status_notify();
}

esp_err_t audio_receiver_start_buffered(uint16_t port) {
  timing_snapshot_t fmt_snap;
  snapshot_state(&fmt_snap);
  const int codec_type = strcmp(fmt_snap.format.codec, "AAC") == 0 ? 4 :
      strcmp(fmt_snap.format.codec, "ALAC") == 0 ? 2 : -1;
  if (!airplay_audio_format_resolve(AUDIO_STREAM_BUFFERED, 0, codec_type,
                                    fmt_snap.format.sample_rate,
                                    fmt_snap.format.frame_size,
                                    fmt_snap.format.bits_per_sample,
                                    fmt_snap.format.channels)) {
    ESP_LOGE(TAG,
             "Buffered format rejected codec=%s sr=%d ch=%d bits=%d frame=%d",
             fmt_snap.format.codec, fmt_snap.format.sample_rate,
             fmt_snap.format.channels, fmt_snap.format.bits_per_sample,
             fmt_snap.format.frame_size);
    return ESP_ERR_NOT_SUPPORTED;
  }

  if (!realtime_stage_stop_and_wait() || !realtime_receiver_is_idle())
    return ESP_ERR_INVALID_STATE;
  if (!s.transport) return ESP_ERR_INVALID_STATE;
  if (__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE)) return ESP_OK;
  if (s.processor_task) {
    ESP_LOGE(TAG,
             "buffered processor from previous session is still active; refusing duplicate task");
    return ESP_ERR_INVALID_STATE;
  }

  /* New buffered codec session: compressed bytes, FLUSH state and decoded
   * media are all hard-reset. Live FLUSH commands never clear the byte FIFO. */
  buffered_control_reset();
  ap2_buffered_fifo_clear(s.transport);
  reset_buffered_pcm_store();

  uint16_t bound = port;
  ESP_RETURN_ON_ERROR(ap2_buffered_fifo_start(s.transport, port, &bound),
                      TAG, "buffered FIFO start failed");
  s.port = bound;
  __atomic_store_n(&s.rx_running, true, __ATOMIC_RELEASE);
  bool stack_psram = true;
  BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      ap2_buffered_processor_task, "ap2_buf_proc", AP2_PROCESS_STACK, NULL,
      AP2_DECODE_PRIORITY, &s.processor_task, AP2_BUFFERED_PROCESSOR_CORE,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (created != pdPASS) {
    stack_psram = false;
    created = xTaskCreatePinnedToCoreWithCaps(
        ap2_buffered_processor_task, "ap2_buf_proc", AP2_PROCESS_STACK, NULL,
        AP2_DECODE_PRIORITY, &s.processor_task, AP2_BUFFERED_PROCESSOR_CORE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (created != pdPASS) {
    __atomic_store_n(&s.rx_running, false, __ATOMIC_RELEASE);
    ap2_buffered_fifo_notify(s.transport);
    ap2_buffered_fifo_stop(s.transport);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "decoder task stack: %u KiB in %s (internal largest free "
                "block now %u KiB)",
           (unsigned)(AP2_PROCESS_STACK / 1024U),
           stack_psram ? "PSRAM" : "internal RAM (no PSRAM block free)",
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT) / 1024U));
  AP_LOGI(TAG, "AP2 buffered listener port=%u", (unsigned)s.port);
  return ESP_OK;
}

esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port) {
  const uint32_t quiesce_req =
      __atomic_load_n(&s.playout_quiesce_req, __ATOMIC_ACQUIRE);
  const uint32_t quiesce_ack =
      __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE);
  if (quiesce_req != quiesce_ack ||
      __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&s.playout_servo_reset_requested, __ATOMIC_ACQUIRE)) {
    ESP_LOGE(TAG,
             "audio start refused: previous playout boundary not quiesced "
             "req=%" PRIu32 " ack=%" PRIu32 " flush=%d servoReset=%d",
             quiesce_req, quiesce_ack,
             __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ? 1 : 0,
             __atomic_load_n(&s.playout_servo_reset_requested,
                             __ATOMIC_ACQUIRE) ? 1 : 0);
    return ESP_ERR_INVALID_STATE;
  }

  if (s.stream_type == AUDIO_STREAM_BUFFERED) {
    return audio_receiver_start_buffered(tcp_port);
  }

  if (s.stream_type == AUDIO_STREAM_REALTIME) {
    timing_snapshot_t fmt_snap;
    snapshot_state(&fmt_snap);
    if (strcmp(fmt_snap.format.codec, "ALAC") != 0 ||
        fmt_snap.format.sample_rate != 44100 || fmt_snap.format.channels != 2 ||
        fmt_snap.format.bits_per_sample != 16 || fmt_snap.format.frame_size != 352) {
      ESP_LOGE(TAG,
               "Realtime format rejected codec=%s sr=%d ch=%d bits=%d frame=%d",
               fmt_snap.format.codec, fmt_snap.format.sample_rate,
               fmt_snap.format.channels, fmt_snap.format.bits_per_sample,
               fmt_snap.format.frame_size);
      return ESP_ERR_NOT_SUPPORTED;
    }

    /* AAC compressed payload pages and ALAC large realtime buffers are the
     * same physical PSRAM. Never let ALAC reuse those bytes until the TCP
     * reader and buffered decoder have completely released their ownership. */
    if (s.transport && !ap2_buffered_fifo_is_idle(s.transport)) {
      ESP_LOGE(TAG,
               "realtime start refused: buffered transport still owns shared codec workspace");
      return ESP_ERR_INVALID_STATE;
    }
    if (s.transport) ap2_buffered_fifo_clear(s.transport);

    /* SETUP may be followed by RECORD for the same live stream. Preserve
     * its anchor, cursor and EQ history; only a stopped stream needs startup. */
    if (realtime_receiver_is_running()) {
      return !__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE) && !s.processor_task && s.realtime_stage_task &&
                     __atomic_load_n(&s.realtime_stage_running, __ATOMIC_ACQUIRE)
                 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (__atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE) || s.processor_task || !realtime_receiver_is_idle() ||
        !s.realtime_stage_task ||
        !realtime_stage_stop_and_wait()) return ESP_ERR_INVALID_STATE;
    uint32_t rt_gen = 0;
    taskENTER_CRITICAL(&s.state_mux);
    /* A codec/session boundary already marked a discontinuity. Publish the
     * fresh PCM generation now, before UDP starts, but deliberately leave the
     * timing anchor invalid. This lets early ALAC packets be retained by RTP
     * address while playout waits exclusively for a real D7/SETRATE anchor. */
    rt_gen = commit_anchor_epoch_locked();
    s.anchor_valid = false;
    s.rt_media_rebase_valid = false;
    s.rt_media_rebase_clock_id = 0;
    s.rt_media_rebase_epoch = 0;
    s.rt_media_rebase_bias_ns = 0;
    s.realtime_stage_cursor_valid = false;
    s.realtime_stage_generation = rt_gen;
    taskEXIT_CRITICAL(&s.state_mux);
    __atomic_store_n(&s.realtime_stage_running, true, __ATOMIC_RELEASE);
    realtime_stage_kick();
    AUDIO_DIAG_LIFECYCLE_REALTIME_EPOCH(rt_gen);

    audio_encrypt_t session_encrypt;
    taskENTER_CRITICAL(&s.state_mux);
    session_encrypt = s.encrypt;
    taskEXIT_CRITICAL(&s.state_mux);
    realtime_receiver_config_t cfg = {
        .format = fmt_snap.format,
        .encrypt = session_encrypt,
        .pcm_sink = realtime_pcm_sink,
        .pcm_sink_ctx = NULL,
        .deadline_cb = realtime_playout_deadline,
        .deadline_ctx = NULL,
    };
    esp_err_t err = realtime_receiver_start(data_port, control_port, &cfg);
    if (err != ESP_OK) {
      __atomic_store_n(&s.realtime_stage_running, false, __ATOMIC_RELEASE);
      realtime_stage_kick();
    }
    return err;
  }

  return ESP_ERR_NOT_SUPPORTED;
}

static bool wait_playout_quiesced(uint32_t request, uint32_t timeout_ms) {
  if (!s.playout_task || !__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) return true;
  const int64_t deadline_us =
      esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
  while (esp_timer_get_time() < deadline_us) {
    if (__atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE) == request) {
      return true;
    }
    vTaskDelay(1);
  }
  return __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE) == request;
}

void audio_receiver_stop(void) {
  /* A codec switch is a hard session boundary. Stop both possible producers
   * first; no compressed, decoded or timing state is allowed to bleed into
   * the next AAC/ALAC SETUP. */
  (void)realtime_stage_stop_and_wait();
  realtime_receiver_stop();
  taskENTER_CRITICAL(&s.state_mux);
  s.realtime_stage_cursor_valid = false;
  taskEXIT_CRITICAL(&s.state_mux);
  __atomic_store_n(&s.rx_running, false, __ATOMIC_RELEASE);
  ap2_buffered_fifo_notify(s.transport);

  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  status_invalidate_sync();
  mark_timeline_discontinuity();
  audio_status_notify();
  __atomic_store_n(&s.playout_servo_reset_requested, true, __ATOMIC_RELEASE);
  const uint32_t quiesce_request =
      __atomic_add_fetch(&s.playout_quiesce_req, 1U, __ATOMIC_ACQ_REL);

  /* This is a hard boundary in execution too: do not let the next RTSP SETUP
   * overtake the Core1 I2S flush/reset. The wait is normally only a few milliseconds and is bounded
   * so a genuine I2S stall is surfaced rather than hanging the control task. */
  if (!wait_playout_quiesced(quiesce_request, 250U)) {
    ESP_LOGE(TAG,
             "PLAYOUT quiesce timeout req=%" PRIu32 " ack=%" PRIu32
             " flush=%d servoReset=%d",
             quiesce_request,
             __atomic_load_n(&s.playout_quiesce_ack, __ATOMIC_ACQUIRE),
             __atomic_load_n(&s.i2s_flush_requested, __ATOMIC_ACQUIRE) ? 1 : 0,
             __atomic_load_n(&s.playout_servo_reset_requested,
                             __ATOMIC_ACQUIRE) ? 1 : 0);
  }

  if (s.transport) {
    ap2_buffered_fifo_stop(s.transport);
  }

  /* The buffered processor exits when rx_running becomes false. Wait only on
   * the control path so its decoder cannot still publish PCM while the next
   * codec session is being configured. */
  for (int i = 0; s.processor_task && i < 100; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s.processor_task) {
    ESP_LOGE(TAG,
             "buffered processor did not stop within shutdown window; next buffered start will be rejected");
  }

  if (s.transport) {
    ap2_buffered_fifo_clear(s.transport);
  }
  buffered_control_reset();

  /* Do not reset the stateful EQ asynchronously from the control core. AAC
   * and the ALAC staging task each reset it on their next generation. */
  s.port = 0;
}

uint16_t audio_receiver_get_buffered_port(void) { return s.port; }

void audio_receiver_set_volume_q15(int32_t volume_q15) {
  if (volume_q15 < 0) volume_q15 = 0;
  if (volume_q15 > 32768) volume_q15 = 32768;
  __atomic_store_n(&s_volume_target_q15, volume_q15, __ATOMIC_RELEASE);
}

int32_t audio_receiver_get_volume_q15(void) {
  return __atomic_load_n(&s_volume_target_q15, __ATOMIC_ACQUIRE);
}

uint32_t audio_receiver_get_output_mute_mask(void) {
  return __atomic_load_n(&s_output_mute_mask, __ATOMIC_ACQUIRE);
}

esp_err_t audio_receiver_set_output_muted(uint32_t channel, bool muted) {
  if (channel > 1U) return ESP_ERR_INVALID_ARG;
  const uint32_t bit = 1U << channel;
  if (muted) __atomic_fetch_or(&s_output_mute_mask, bit, __ATOMIC_ACQ_REL);
  else __atomic_fetch_and(&s_output_mute_mask, ~bit, __ATOMIC_ACQ_REL);
  return ESP_OK;
}

void audio_receiver_seek_flush(void) {
  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type == AUDIO_STREAM_BUFFERED) {
    /* Plain buffered FLUSH supplies no 23-bit endpoint. Use the same single
     * no-boundary path as FLUSHBUFFERED until seq 0, including immediate PCM
     * invalidation; it must not retain old PCM through the next anchor. */
    audio_receiver_set_immediate_flush(0U, 0U);
  } else {
    mark_timeline_discontinuity();
  }
}

void audio_receiver_realtime_flush_to_rtp(uint32_t flush_rtp) {
  timing_snapshot_t snap;
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME) {
    audio_receiver_seek_flush();
    return;
  }

  /* RTP-Info names the first timestamp that may remain after FLUSH. Preserve
   * the validated sender timing map and current generation, invalidate only
   * older audio resident in the ALAC stage ring and PCM FIFO, and move the
   * chronological staging floor forward to the same RTP boundary. */
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  snapshot_state(&snap);
  if (snap.stream_type != AUDIO_STREAM_REALTIME) {
    xSemaphoreGive(s.publish_mutex);
    return;
  }
  if (s.realtime_stage_ring) {
    pcm_rtp_ring_invalidate_before(s.realtime_stage_ring, flush_rtp,
                                   snap.generation);
  }
  if (s.pcm_fifo) {
    pcm_fifo_invalidate_before(s.pcm_fifo, flush_rtp, snap.generation);
  }

  uint32_t old_cursor = 0;
  bool old_cursor_valid = false;
  taskENTER_CRITICAL(&s.state_mux);
  if (s.stream_type == AUDIO_STREAM_REALTIME &&
      s.generation == snap.generation && !s.timeline_reset_pending) {
    old_cursor_valid = s.realtime_stage_cursor_valid &&
                       s.realtime_stage_generation == snap.generation;
    if (old_cursor_valid) old_cursor = s.realtime_stage_cursor_rtp;
    if (!old_cursor_valid || rtp_delta(flush_rtp, old_cursor) > 0) {
      s.realtime_stage_cursor_rtp = flush_rtp;
      s.realtime_stage_generation = snap.generation;
      s.realtime_stage_cursor_valid = true;
    }
  }
  taskEXIT_CRITICAL(&s.state_mux);
  xSemaphoreGive(s.publish_mutex);
  realtime_stage_kick();

  /* An explicit FLUSH may cancel already queued DMA, but it must not change
   * the validated RTP<->presentation map. The normal playout task re-primes
   * against the same sender timeline and waits until desired_rtp reaches
   * flush_rtp. */
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  media_control_wake();
  if (old_cursor_valid) {
    AP_LOGI(TAG,
             "REALTIME FLUSH preserve timing rtp=%" PRIu32
             " gen=%" PRIu32 " anchor=%d oldCursor=%" PRIu32,
             flush_rtp, snap.generation, snap.anchor_valid ? 1 : 0, old_cursor);
  } else {
    AP_LOGI(TAG,
             "REALTIME FLUSH preserve timing rtp=%" PRIu32
             " gen=%" PRIu32 " anchor=%d oldCursor=--",
             flush_rtp, snap.generation, snap.anchor_valid ? 1 : 0);
  }
}

void audio_receiver_realtime_flush_wait_sender_anchor(void) {
  mark_timeline_discontinuity();
  AP_LOGI(TAG,
           "REALTIME FLUSH no RTP boundary: local timing invalidated; "
           "waiting for D7/SETRATE (no arrival-time fallback)");
}

esp_err_t audio_receiver_set_deferred_flush_range(
    uint32_t from_seq, uint32_t from_ts, uint32_t until_seq, uint32_t until_ts,
    bool timestamps_valid) {
  if (!s_buffered_control.mutex || !s.publish_mutex) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  timing_snapshot_t snap;
  snapshot_state(&snap);
  esp_err_t err = snap.stream_type == AUDIO_STREAM_BUFFERED
      ? buffered_control_add_deferred_locked(from_seq, from_ts, until_seq,
                                              until_ts, timestamps_valid)
      : ESP_ERR_INVALID_STATE;
  /* As in Shairport Sync, a deferred FLUSH acts only on the compressed
   * packets as the decoder reaches flushFromSeq; PCM already decoded (at
   * most the 0.85 s decode window) is not touched. */
  xSemaphoreGive(s.publish_mutex);
  xSemaphoreGive(s_buffered_control.mutex);
  if (err == ESP_OK) media_control_wake();
  return err;
}

void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts) {
  if (!s_buffered_control.mutex || !s.publish_mutex) return;

  /* Shairport 5.5.2 handle_flushbuffered() immediate path:
   *   1. arm the sequential flush endpoint;
   *   2. disable playout;
   *   3. invalidate the presentation anchor;
   *   4. keep consuming compressed packets until untilSeq is reached or
   *      overshot. The endpoint packet itself is retained.
   * untilTS is stored with the request for protocol parity but does not drive
   * the sequential discard decision. */
  /* Shairport updates the immediate request, play flag and anchor while its
   * flush_mutex excludes the packet classifier. Keep that same atomic control
   * boundary so the processor cannot observe an armed FLUSH with the old play
   * state/anchor still published. */
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  const uint32_t masked_until_seq = until_seq & 0x007fffffU;
  if (++s_buffered_control.revision == 0U) ++s_buffered_control.revision;
  s_buffered_control.immediate_until_seq = masked_until_seq;
  s_buffered_control.immediate_until_rtp = until_ts;

  if (masked_until_seq == 0U) {
    /* Shairport development: "A flush to Block 0 looks like a bug in HomePod
     * OS 27." The flush is finished at once: no packet is discarded, and, as
     * when any immediate flush finishes, all deferred requests are cancelled.
     * Play stop, anchor reset and PCM reset below still apply. */
    s_buffered_control.immediate_active = false;
    memset(s_buffered_control.deferred, 0, sizeof(s_buffered_control.deferred));
  } else {
    s_buffered_control.immediate_active = true;
  }
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  status_invalidate_sync();
  /* Stop I2S first: request the stop before the PCM is invalidated, so the
   * playout task sees it before it can find the emptied store. */
  __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
  /* Must happen here, not on a sampled playing->stopped transition. Rapid
   * FLUSH/anchor/resume can otherwise bypass the consumer's old PCM reset. */
  (void)reset_buffered_pcm_store();
  mark_timeline_discontinuity();
  xSemaphoreGive(s.publish_mutex);
  xSemaphoreGive(s_buffered_control.mutex);

  if (masked_until_seq == 0U) {
    AP_LOGI(TAG, "AAC FLUSH to block 0: finished at once (untilTS=%" PRIu32 ")",
             until_ts);
  }

  media_control_wake();
  audio_status_notify();
}

void audio_receiver_pause(void) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s.state_mux);
  /* Shairport handle_setrateanchori(rate 0): reset anchor, play disabled */
  status_invalidate_sync();
  pause_presentation_timing();
  audio_status_notify();
}

void audio_receiver_set_playout_latency_samples(uint32_t v) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playout_latency_samples = v;
  taskEXIT_CRITICAL(&s.state_mux);
}
int32_t audio_receiver_get_output_latency_us(void) {
  return __atomic_load_n(&s_output_latency_us, __ATOMIC_RELAXED);
}

esp_err_t audio_receiver_set_output_latency_us(int32_t us, bool persist) {
  if (us < AP2_OUTPUT_LATENCY_MIN_US || us > AP2_OUTPUT_LATENCY_MAX_US)
    return ESP_ERR_INVALID_ARG;
  const int32_t old = __atomic_exchange_n(&s_output_latency_us, us, __ATOMIC_ACQ_REL);
  const int32_t delta = us > old ? us - old : old - us;
  if (delta >= 500 && __atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) {
    /* A larger shift is applied like a resync: stop and re-prime at the new
     * presentation point (brief silence). Small shifts go through the servo. */
    __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
    media_control_wake();
  }
  AP_LOGI(TAG, "OUTPUT LATENCY %+ld us (was %+ld us)%s", (long)us, (long)old,
           persist ? ", saved" : "");
  return persist ? settings_set_output_latency_us(us) : ESP_OK;
}

void audio_receiver_get_playback_info(audio_receiver_playback_info_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  timing_snapshot_t snap;
  snapshot_state(&snap);
  out->stream_type = snap.stream_type;
  out->format = snap.format;
  out->playing = snap.playing;
  out->active = __atomic_load_n(&s.rx_running, __ATOMIC_ACQUIRE) ||
                realtime_receiver_is_running();
  out->sync_valid =
      __atomic_load_n(&s.status_sync_valid, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&s.status_sync_generation, __ATOMIC_RELAXED) ==
          snap.generation;
  out->sync_us = __atomic_load_n(&s.status_sync_us, __ATOMIC_RELAXED);
  out->servo_ppm = __atomic_load_n(&s.status_servo_ppm, __ATOMIC_RELAXED);
  if (snap.stream_type == AUDIO_STREAM_BUFFERED && s.transport) {
    ap2_buffered_fifo_usage_t usage = {0};
    ap2_buffered_fifo_get_usage(s.transport, &usage);
    out->fifo_used_kib = (uint32_t)(usage.used_bytes / 1024U);
    out->fifo_capacity_kib = (uint32_t)(usage.capacity_bytes / 1024U);
  }
}

int32_t audio_receiver_get_extra_delay_us(void) {
  return __atomic_load_n(&s_extra_delay_us, __ATOMIC_RELAXED);
}

esp_err_t audio_receiver_set_extra_delay_us(int32_t us) {
  if (us < 0 || us > AP2_EXTRA_DELAY_MAX_US) return ESP_ERR_INVALID_ARG;
  const int32_t old = __atomic_exchange_n(&s_extra_delay_us, us, __ATOMIC_ACQ_REL);
  const int32_t delta = us > old ? us - old : old - us;
  if (delta >= 500 && __atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE)) {
    /* As for the output latency: a larger shift re-primes at the new point. */
    __atomic_store_n(&s.i2s_flush_requested, true, __ATOMIC_RELEASE);
    media_control_wake();
  }
  if (us != old) {
    AP_LOGI(TAG, "EXTRA DELAY %ld us (was %ld us)", (long)us, (long)old);
  }
  return ESP_OK;
}

esp_err_t audio_receiver_measure_output_latency(latency_cal_result_t *res,
                                                bool audible) {
  if (!res) return ESP_ERR_INVALID_ARG;
  memset(res, 0, sizeof(*res));
  (void)audible;
#if !CONFIG_AIRPLAY_LATENCY_CAL
  res->error = "measurement not built in (CONFIG_AIRPLAY_LATENCY_CAL)";
  return ESP_ERR_NOT_SUPPORTED;
#else
  /* The caller must have released the engine (like the Wi-Fi scan): no
   * playout task may own I2S while the test plays. */
  if (__atomic_load_n(&s.engine_running, __ATOMIC_ACQUIRE) || s.playout_task) {
    res->error = "AirPlay engine still running (release it first)";
    return ESP_ERR_INVALID_STATE;
  }
  /* A browser may silently re-send the pending request after a reboot;
   * never start a measurement in the first seconds. */
  if (esp_timer_get_time() < 20000000LL) {
    res->error = "device just started - try again in a few seconds";
    return ESP_ERR_INVALID_STATE;
  }
  if (__atomic_exchange_n(&s_cal_busy, true, __ATOMIC_ACQ_REL)) {
    res->error = "a measurement is already running";
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t err = audio_playout_init(); /* no-op when already initialised */
  if (err != ESP_OK) {
    res->error = "I2S not available";
    __atomic_store_n(&s_cal_busy, false, __ATOMIC_RELEASE);
    return err;
  }
  /* The amplifier is normally OFF without an AirPlay session. "audible"
   * switches it on for the test (the usual 300 s off-delay applies after).
   * A line-level measurement before the amplifier is not affected. */
  if (audible) {
    amp_control_session_connected();
    vTaskDelay(pdMS_TO_TICKS(400)); /* amplifier start-up / anti-pop time */
  }
  err = latency_cal_capture_start();
  if (err != ESP_OK) {
    res->error = err == ESP_ERR_NO_MEM ? "not enough memory for the ADC capture"
                                       : "ADC capture could not start (pin)";
  } else {
    vTaskDelay(pdMS_TO_TICKS(30)); /* ADC settles; lead silence follows */
    const esp_err_t play = playout_run_latency_calibration();
    vTaskDelay(pdMS_TO_TICKS(20));
    if (play == ESP_OK) {
      latency_cal_capture_finish(s_cal.emit_us, s_cal.n_emit, res);
      err = res->ok ? ESP_OK : ESP_FAIL;
    } else {
      latency_cal_result_t dummy;
      latency_cal_capture_finish(s_cal.emit_us, 0, &dummy);
      res->error = play == ESP_ERR_NO_MEM
                       ? "not enough memory for the test signal"
                       : "I2S test playback failed";
      err = play;
    }
  }
  if (audible) amp_control_session_disconnected();
  __atomic_store_n(&s_cal_busy, false, __ATOMIC_RELEASE);
  if (res->ok) {
    AP_LOGI(TAG, "LATENCY MEASURED %ld us (spread %ld us, %u/%u bursts, corr %.2f%s)",
             (long)res->latency_us, (long)res->spread_us, res->valid_bursts,
             (unsigned)LATENCY_CAL_BURSTS, (double)res->corr,
             res->inverted ? ", polarity inverted" : "");
  } else {
    AP_LOGW(TAG, "LATENCY MEASUREMENT FAILED: %s", res->error ? res->error : "?");
  }
  return err;
#endif
}

void audio_receiver_set_playing(bool p) {
  taskENTER_CRITICAL(&s.state_mux);
  s.playing = p;
  taskEXIT_CRITICAL(&s.state_mux);
  media_control_wake();
  if (!p) {
    status_invalidate_sync();
    mark_timeline_discontinuity();
  }
  audio_status_notify();
}

void audio_receiver_set_client_control(uint32_t ip, uint16_t port) {
  realtime_receiver_set_client_control(ip, port);
}

void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t ptp_ns,
                                    uint32_t rtp) {
  if (clock_id) {
    ptp_clock_set_master_clock_id(clock_id);
  }
  ptp_clock_snapshot_t ps = {0};
  ptp_clock_get_snapshot(&ps);
  uint64_t local_ns = 0;
  const bool local_valid = buffered_ptp_qualified(&ps, clock_id, true) &&
      ptp_clock_engine_remote_to_local(ptp_ns, ps.filtered_offset_ns, &local_ns) &&
      local_ns != 0;
  const uint64_t local_update_us = local_valid ? (uint64_t)esp_timer_get_time() : 0;
  if (!s_buffered_control.mutex || !s.publish_mutex) return;
  xSemaphoreTake(s_buffered_control.mutex, portMAX_DELAY);
  xSemaphoreTake(s.publish_mutex, portMAX_DELAY);
  uint32_t gen;
  bool committed;
  taskENTER_CRITICAL(&s.state_mux);
  if (s.stream_type == AUDIO_STREAM_BUFFERED && s.anchor_valid &&
      !s.timeline_reset_pending && s.buffered_sender_anchor_valid &&
      s.buffered_sender_anchor_clock_id == clock_id &&
      s.buffered_sender_anchor_ptp_ns == ptp_ns &&
      s.buffered_sender_anchor_rtp == rtp) {
    taskEXIT_CRITICAL(&s.state_mux);
    xSemaphoreGive(s.publish_mutex);
    xSemaphoreGive(s_buffered_control.mutex);
    return;
  }
  committed = s.timeline_reset_pending;
  gen = commit_anchor_epoch_locked();
  s.media_revision = next_generation(s.media_revision);
  s.anchor_clock_id = clock_id;
  s.anchor_ptp_ns = ptp_ns;
  /* Publish both sides of the map together. No transient clock loss for a
   * qualified same-GM refresh, including while AAC is publishing an AU. */
  s.anchor_local_ns = local_valid ? local_ns : 0;
  s.anchor_local_update_us = local_update_us;
  s.anchor_local_valid = local_valid;
  s.anchor_rtp = rtp;
  if (s.stream_type == AUDIO_STREAM_BUFFERED) {
    s.buffered_sender_anchor_valid = true;
    s.buffered_sender_anchor_clock_id = clock_id;
    s.buffered_sender_anchor_ptp_ns = ptp_ns;
    s.buffered_sender_anchor_rtp = rtp;
  }
  s.rt_media_rebase_valid = false;
  s.rt_media_rebase_clock_id = 0;
  s.rt_media_rebase_epoch = 0;
  s.rt_media_rebase_bias_ns = 0;
  s.anchor_valid = true;
  taskEXIT_CRITICAL(&s.state_mux);
  if (++s_buffered_control.revision == 0U) ++s_buffered_control.revision;
  xSemaphoreGive(s.publish_mutex);
  xSemaphoreGive(s_buffered_control.mutex);
  if (committed) {
    s_anchor_commit_us = esp_timer_get_time();
    s_anchor_commit_gen = gen;
  }
  /* Shairport-style separation: SETRATEANCHORTIME updates presentation timing
   * only. The compressed FIFO itself has no RTP cursor to search or rebind. */
  if (s.transport) ap2_buffered_fifo_notify(s.transport);
  playout_wake();
  realtime_stage_kick();

  timing_snapshot_t anchor_snap;
  snapshot_state(&anchor_snap);
  AP_LOGI(TAG, "ANCHOR clock=%016" PRIx64 " ptp=%" PRIu64
                " rtp=%" PRIu32 " gen=%" PRIu32 " pcm_gen=%" PRIu32 "%s",
           clock_id, ptp_ns, rtp, gen, anchor_snap.pcm_generation,
           committed ? " (new timeline committed; waiting for PTP lock if needed)"
                     : " (anchor update; waiting for PTP lock if needed)");
}


bool audio_receiver_set_realtime_anchor_local(
    uint64_t clock_id, uint32_t gm_epoch, uint32_t mastership_age_ms,
    uint64_t remote_ptp_ns, uint64_t candidate_local_ns, uint32_t rtp,
    audio_realtime_anchor_result_t *result) {
  audio_realtime_anchor_result_t local_result = {0};
  if (!result) result = &local_result;
  memset(result, 0, sizeof(*result));
  if (candidate_local_ns == 0) return false;

  uint32_t gen = 0;
  bool committed = false;
  bool accepted = false;
  bool log_rebase = false;
  uint64_t effective_local_ns = candidate_local_ns;
  int64_t rebase_step_ns = 0;
  int64_t rebase_bias_ns = 0;

  taskENTER_CRITICAL(&s.state_mux);
  if (s.stream_type == AUDIO_STREAM_REALTIME) {
    const bool have_running_local_map =
        s.anchor_valid && !s.timeline_reset_pending && s.anchor_local_ns != 0U;
    const bool epoch_changed =
        have_running_local_map &&
        (!s.rt_media_rebase_valid || s.rt_media_rebase_epoch != gm_epoch ||
         s.rt_media_rebase_clock_id != clock_id);

    if (epoch_changed && mastership_age_ms < AP2_RT_GM_REBASE_SETTLE_MS) {
      result->deferred = true;
      taskEXIT_CRITICAL(&s.state_mux);
      return false;
    }

    if (!have_running_local_map) {
      /* Initial stream startup: no phase exists to preserve yet, so use the
       * sender/GM conversion directly and establish this epoch with zero
       * media bias. The existing ~400 ms PTP readiness remains unchanged. */
      s.rt_media_rebase_valid = true;
      s.rt_media_rebase_clock_id = clock_id;
      s.rt_media_rebase_epoch = gm_epoch;
      s.rt_media_rebase_bias_ns = 0;
      effective_local_ns = candidate_local_ns;
    } else if (epoch_changed) {
      /* New GM/mastership epoch while audio is already running. Preserve the
       * exact existing RTP<->local phase and calculate the initial media
       * bias needed to express the new GM's D7 observations in that same
       * local timeline. The bias then converges to zero at 50 ppm;
       * PTP estimation and the hardware PID remain independent. */
      const int sr = s.format.sample_rate > 0 ? s.format.sample_rate : 44100;
      const int32_t drtp = rtp_delta(rtp, s.anchor_rtp);
      const int64_t predicted_local_ns =
          (int64_t)s.anchor_local_ns +
          ((int64_t)drtp * 1000000000LL) / (int64_t)sr;
      rebase_step_ns = (int64_t)candidate_local_ns - predicted_local_ns;
      rebase_bias_ns = -rebase_step_ns;
      const int64_t rebased = (int64_t)candidate_local_ns + rebase_bias_ns;
      if (rebased <= 0) {
        taskEXIT_CRITICAL(&s.state_mux);
        return false;
      }
      effective_local_ns = (uint64_t)rebased;
      s.rt_media_rebase_valid = true;
      s.rt_media_rebase_clock_id = clock_id;
      s.rt_media_rebase_epoch = gm_epoch;
      s.rt_media_rebase_bias_ns = rebase_bias_ns;
      s.rt_media_rebase_start_us = esp_timer_get_time();
      result->rebased = true;
      log_rebase = true;
    } else if (s.rt_media_rebase_valid &&
               s.rt_media_rebase_epoch == gm_epoch &&
               s.rt_media_rebase_clock_id == clock_id) {
      /* Same GM: follow the current filtered PTP offset and gradually retire
       * the temporary handover bias. Ordinary D7 updates do not restart decay. */
      rebase_bias_ns = realtime_remaining_bias(
          s.rt_media_rebase_bias_ns, s.rt_media_rebase_start_us,
          esp_timer_get_time());
      const int64_t adjusted = (int64_t)candidate_local_ns + rebase_bias_ns;
      if (adjusted <= 0) {
        taskEXIT_CRITICAL(&s.state_mux);
        return false;
      }
      effective_local_ns = (uint64_t)adjusted;
    }

    committed = s.timeline_reset_pending;
    gen = commit_anchor_epoch_locked();
    s.anchor_clock_id = clock_id;
    s.anchor_ptp_ns = remote_ptp_ns; /* retained remote anchor for live PTP conversion */
    s.anchor_local_ns = effective_local_ns;
    s.anchor_rtp = rtp;
    s.anchor_valid = true;
    accepted = true;

    result->effective_local_ns = effective_local_ns;
    result->rebase_step_ns = rebase_step_ns;
    result->rebase_bias_ns = realtime_remaining_bias(
        s.rt_media_rebase_bias_ns, s.rt_media_rebase_start_us,
        esp_timer_get_time());
  }
  taskEXIT_CRITICAL(&s.state_mux);

  if (accepted) {
    playout_wake();
    realtime_stage_kick();
  }

  if (log_rebase) {
    AP_LOGI(TAG,
             "RT GM MEDIA REBASE clock=%016" PRIx64 " epoch=%" PRIu32
             " age=%lums step=%+.3fms bias=%+.3fms rtp=%" PRIu32,
             clock_id, gm_epoch, (unsigned long)mastership_age_ms,
             (double)rebase_step_ns / 1000000.0,
             (double)rebase_bias_ns / 1000000.0, rtp);
  }

  /* Continuous D7 refreshes are normal steady-state operation.
   * Keep INFO logging for phase-significant events only. */
  if (accepted && (committed || log_rebase)) {
    AP_LOGI(TAG,
             "RT ANCHOR clock=%016" PRIx64 " epoch=%" PRIu32
             " remotePTP=%" PRIu64 " rawLocal=%" PRIu64
             " local=%" PRIu64 " bias=%+.3fms rtp=%" PRIu32
             " gen=%" PRIu32 "%s",
             clock_id, gm_epoch, remote_ptp_ns, candidate_local_ns,
             effective_local_ns,
             (double)result->rebase_bias_ns / 1000000.0, rtp, gen,
             committed ? " (new timeline committed)" : " (GM rebase)");
  }
  return accepted;
}
