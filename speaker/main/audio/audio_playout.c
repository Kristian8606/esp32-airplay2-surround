#include "audio_playout.h"
#include "sdkconfig.h"

#include <stddef.h>
#include <inttypes.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "audio_diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led.h"
#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
#include "audio_fade.h"
#include "audio_receiver.h" /* AUDIO_PIPELINE_RATE_HZ */
#include "soc/soc_caps.h"
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#include "esp_cache.h"
#endif
#endif

#define TAG "audio_playout"

/* Live clock tuning: within +/-160 ppm the integer part of the
 * MCLK divider never changes, so a retune only rewrites the fractional part
 * (x, y, z, yn1) in ONE 32-bit register store. No disable/enable, no DMA
 * reset, no intermediate divider 2: the I2S clocks and the samples keep
 * running. */
#include "hal/i2s_ll.h"
#define LIVE_SCLK_HZ 160000000U /* PLL_F160M, tx_clk_sel == 2 */
static uint32_t s_live_div_int; /* 0: live tuning not available (no tuning) */
static void live_tune_init(void);
static esp_err_t live_tune_write(int32_t ppm, uint32_t *mclk_hz_out);
static void live_dither_step(void);

#define I2S_DMA_DESC_NUM  AUDIO_PLAYOUT_DMA_BLOCKS
#define I2S_DMA_FRAME_NUM AUDIO_PLAYOUT_FRAMES
/* Initial rate; the playout task switches it to the active stream's rate. */
#define I2S_RATE_HZ       AUDIO_PIPELINE_RATE_HZ
#define I2S_MIN_RATE_HZ   44100U
/* Every descriptor holds one tagged block, plus the batch being written. */
#define TX_TAG_Q_CAP      16U
#define TX_DONE_Q_CAP     16U
_Static_assert(TX_TAG_Q_CAP > I2S_DMA_DESC_NUM + AUDIO_PLAYOUT_BATCH,
               "tag queue must cover every descriptor plus one batch");
_Static_assert(I2S_DMA_FRAME_NUM * 8U <= 4092U,
               "one DMA descriptor holds at most 4092 bytes");
/* Bounded DMA write. Normally a write never waits (playout writes only when
 * a descriptor is free); at most it waits for one descriptor, i.e. one block.
 * The bound only matters if the driver stops completing descriptors: then the
 * write fails, the caller flushes and re-primes instead of hanging forever.
 * i2s_channel_write() takes milliseconds (it converts to ticks itself): pass
 * this value as it is, never through pdMS_TO_TICKS(). */
#define I2S_WRITE_TIMEOUT_MS 200U
_Static_assert(I2S_WRITE_TIMEOUT_MS * I2S_MIN_RATE_HZ >
                   2U * 1000U * I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM,
               "write timeout must exceed twice the whole DMA queue");

/* 32-bit data in 32-bit slots (audio_out_sample.h): 64 BCLK per stereo
 * frame, MCLK 256 x fs. */
#define I2S_SLOT_DATA_WIDTH I2S_DATA_BIT_WIDTH_32BIT
#define I2S_FRAME_BYTES ((size_t)2U * sizeof(audio_out_sample_t))

/* Allocate the I2S/GDMA channel from Core1 so its external interrupt is
 * serviced on the same core as the playout task, away from Core0 network/PTP
 * traffic. This helper task exists only during boot-time initialisation. */
#define AUDIO_PLAYOUT_INIT_CORE  1
#define AUDIO_PLAYOUT_INIT_STACK 4096U
#define AUDIO_PLAYOUT_INIT_PRIO  5U

typedef struct {
  uint32_t rtp;
  uint32_t generation;
  uint32_t frames;
} tx_tag_t;

static i2s_chan_handle_t s_tx;
static bool s_enabled;
/* True when READY-state DMA descriptors contain data loaded by preload but the
 * channel has not yet been enabled. IDF concatenates repeated preloads until
 * the DMA cache is full, so an aborted startup must explicitly consume/reset
 * this state before retrying. */
static bool s_preload_pending;
static uint32_t s_nominal_mclk_hz;
static int32_t s_tune_ppm;
static uint32_t s_rate_hz = I2S_RATE_HZ;

/* SPSC queues: playout task -> TX ISR for tags; TX ISR -> playout task for
 * completions. Capacity is intentionally larger than the two DMA descriptors
 * so the producer can publish the next tag before a blocking write wakes on
 * the previous descriptor's EOF. */
static DRAM_ATTR tx_tag_t s_tag_q[TX_TAG_Q_CAP];
static DRAM_ATTR uint32_t s_tag_head;
static DRAM_ATTR uint32_t s_tag_tail;
static DRAM_ATTR audio_playout_completion_t s_done_q[TX_DONE_Q_CAP];
static DRAM_ATTR uint32_t s_done_head;
static DRAM_ATTR uint32_t s_done_tail;

static DRAM_ATTR uint32_t s_fault;
/* Only the single playout owner may wait; ISR never dereferences this handle
 * after the task exits (the persistent playout task owns I2S for its lifetime). */
static DRAM_ATTR TaskHandle_t s_completion_waiter;
/* 0: wake the waiter on every EOF. N: only once N descriptors are free. */
static DRAM_ATTR uint32_t s_wake_free_blocks;

#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
/* Keep-running stop (audio_playout_fade_out_queued): the audio already queued
 * in the DMA fades out in place. The TX ISR learns the descriptor buffers, in
 * ring order, from its EOF events (the channel is created once and never
 * reconfigured, so they never move) and remembers which one finished last
 * and when (audio_fade.h). Every EOF re-checks the ring order; one mismatch
 * turns the fade off for good (the stop then cuts to zeros). Written by the
 * TX ISR under s_tag_mux. */
static DRAM_ATTR audio_dma_ring_t s_ring = {.last = AUDIO_DMA_IDX_UNKNOWN};
/* Keeps the TX ISR's tag pop out while the playout task retags the queue. */
static DRAM_ATTR portMUX_TYPE s_tag_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

bool audio_playout_has_fault(void) {
  return __atomic_load_n(&s_fault, __ATOMIC_ACQUIRE) != 0U;
}

static inline uint32_t IRAM_ATTR queued_tags(void) {
  const uint32_t head = __atomic_load_n(&s_tag_head, __ATOMIC_ACQUIRE);
  const uint32_t tail = __atomic_load_n(&s_tag_tail, __ATOMIC_ACQUIRE);
  return (head + TX_TAG_Q_CAP - tail) % TX_TAG_Q_CAP;
}

static inline uint32_t IRAM_ATTR free_blocks(void) {
  const uint32_t queued = queued_tags();
  return queued >= I2S_DMA_DESC_NUM ? 0U : I2S_DMA_DESC_NUM - queued;
}

static bool IRAM_ATTR wake_completion_waiter(void) {
  BaseType_t woken = pdFALSE;
  TaskHandle_t waiter = __atomic_load_n(&s_completion_waiter, __ATOMIC_ACQUIRE);
  const uint32_t want = __atomic_load_n(&s_wake_free_blocks, __ATOMIC_ACQUIRE);
  if (waiter && (want == 0U || free_blocks() >= want ||
                 __atomic_load_n(&s_fault, __ATOMIC_ACQUIRE) != 0U)) {
    vTaskNotifyGiveFromISR(waiter, &woken);
  }
  return woken == pdTRUE;
}

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
/* PLAYOUT diagnostics: DMA ran out of queued audio (underrun), and EOF tag
 * bookkeeping faults. Read and cleared by the audio status line. */
static DRAM_ATTR uint32_t s_diag_underruns;
static DRAM_ATTR uint32_t s_diag_tag_faults;
#define DIAG_ISR_COUNT(v) __atomic_fetch_add(&(v), 1U, __ATOMIC_RELAXED)
void audio_playout_diag_take(uint32_t *underruns, uint32_t *tag_faults) {
  if (underruns) *underruns = __atomic_exchange_n(&s_diag_underruns, 0U, __ATOMIC_RELAXED);
  if (tag_faults) *tag_faults = __atomic_exchange_n(&s_diag_tag_faults, 0U, __ATOMIC_RELAXED);
}
#else
#define DIAG_ISR_COUNT(v) do {} while (0)
#endif

static bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t handle,
                                    i2s_event_data_t *event, void *user_ctx) {
  (void)handle; (void)event; (void)user_ctx;
  DIAG_ISR_COUNT(s_diag_underruns);
  __atomic_store_n(&s_fault, 1U, __ATOMIC_RELEASE);
  return wake_completion_waiter();
}

static inline void queues_reset(void) {
  /* Called only with DMA stopped, so no ISR can republish an old fault. */
  __atomic_store_n(&s_fault, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_tag_head, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_tag_tail, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_done_head, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_done_tail, 0U, __ATOMIC_RELEASE);
#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
  /* The next start may begin at any descriptor. */
  audio_dma_ring_restart(&s_ring);
#endif
}

static bool tag_push(uint32_t rtp, uint32_t generation, uint32_t frames) {
  const uint32_t head = __atomic_load_n(&s_tag_head, __ATOMIC_RELAXED);
  const uint32_t tail = __atomic_load_n(&s_tag_tail, __ATOMIC_ACQUIRE);
  const uint32_t next = (head + 1U) % TX_TAG_Q_CAP;
  if (next == tail) {
    return false;
  }
  s_tag_q[head].rtp = rtp;
  s_tag_q[head].generation = generation;
  s_tag_q[head].frames = frames;
  __atomic_store_n(&s_tag_head, next, __ATOMIC_RELEASE);
  return true;
}

static bool IRAM_ATTR tag_pop_isr(tx_tag_t *out) {
  const uint32_t tail = __atomic_load_n(&s_tag_tail, __ATOMIC_RELAXED);
  const uint32_t head = __atomic_load_n(&s_tag_head, __ATOMIC_ACQUIRE);
  if (tail == head) {
    return false;
  }
  /* Keep this copy explicit: an IRAM ISR must not acquire an out-of-line
   * memcpy helper from flash. */
  out->rtp = s_tag_q[tail].rtp;
  out->generation = s_tag_q[tail].generation;
  out->frames = s_tag_q[tail].frames;
  __atomic_store_n(&s_tag_tail, (tail + 1U) % TX_TAG_Q_CAP,
                   __ATOMIC_RELEASE);
  return true;
}

static bool IRAM_ATTR done_push_isr(const tx_tag_t *tag, int64_t local_us) {
  const uint32_t head = __atomic_load_n(&s_done_head, __ATOMIC_RELAXED);
  const uint32_t tail = __atomic_load_n(&s_done_tail, __ATOMIC_ACQUIRE);
  const uint32_t next = (head + 1U) % TX_DONE_Q_CAP;
  if (next == tail) {
    return false;
  }
  s_done_q[head].rtp = tag->rtp;
  s_done_q[head].generation = tag->generation;
  s_done_q[head].frames = tag->frames;
  s_done_q[head].done_local_us = local_us;
  __atomic_store_n(&s_done_head, next, __ATOMIC_RELEASE);
  return true;
}

static bool IRAM_ATTR on_sent(i2s_chan_handle_t handle,
                              i2s_event_data_t *event,
                              void *user_ctx) {
  (void)handle;
  (void)event;
  (void)user_ctx;

  tx_tag_t tag;
#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
  const int64_t eof_us = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&s_tag_mux);
  audio_dma_ring_track(&s_ring, I2S_DMA_DESC_NUM, event ? event->dma_buf : NULL,
                       event ? (uint32_t)event->size : 0U, eof_us);
  const bool have_tag = tag_pop_isr(&tag);
  portEXIT_CRITICAL_ISR(&s_tag_mux);
#else
  const bool have_tag = tag_pop_isr(&tag);
#endif
  if (!have_tag) {
    DIAG_ISR_COUNT(s_diag_tag_faults);
    __atomic_store_n(&s_fault, 1U, __ATOMIC_RELEASE);
    return wake_completion_waiter();
  }

  /* esp_timer_get_time() is lock-free and documented for ISR use. Timestamp
   * the DMA EOF itself; conversion to PTP is done later in task context. */
  const int64_t done_local_us = esp_timer_get_time();
  if (!done_push_isr(&tag, done_local_us)) {
    DIAG_ISR_COUNT(s_diag_tag_faults);
    __atomic_store_n(&s_fault, 1U, __ATOMIC_RELEASE);
  }
  return wake_completion_waiter();
}


static esp_err_t audio_playout_init_current_core(void) {
  if (s_tx) {
    return ESP_OK;
  }

  i2s_chan_handle_t tx = NULL;
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
  chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
  chan_cfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chan_cfg, &tx, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
    goto fail;
  }

  i2s_std_config_t cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_RATE_HZ),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_SLOT_DATA_WIDTH,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = CONFIG_I2S_SCK_IO,
          .bclk = CONFIG_I2S_BCK_IO,
          .ws = CONFIG_I2S_WS_IO,
          .dout = CONFIG_I2S_DO_IO,
          .din = I2S_GPIO_UNUSED,
      },
  };

  err = i2s_channel_init_std_mode(tx, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
    goto fail;
  }

  const i2s_event_callbacks_t callbacks = {
      .on_recv = NULL,
      .on_recv_q_ovf = NULL,
      .on_sent = on_sent,
      .on_send_q_ovf = on_send_q_ovf,
  };
  err = i2s_channel_register_event_callback(tx, &callbacks, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s callback registration failed: %s", esp_err_to_name(err));
    goto fail;
  }

  queues_reset();
  s_enabled = false;
  s_preload_pending = false;
  s_tune_ppm = 0;

  /* The channel is READY here, which is the state required by
   * i2s_channel_tune_rate(). Capture the actual nominal MCLK so ppm
   * corrections are relative to the hardware clock chosen by IDF. */
  i2s_tuning_info_t initial_tune = {0};
  err = i2s_channel_tune_rate(tx, NULL, &initial_tune);
  if (err == ESP_OK) {
    s_nominal_mclk_hz = initial_tune.curr_mclk_hz;
  } else {
    s_nominal_mclk_hz = s_rate_hz * 256U;
    ESP_LOGW(TAG, "initial tune query failed: %s; assuming MCLK=%" PRIu32,
             esp_err_to_name(err), s_nominal_mclk_hz);
  }

  live_tune_init();

  /* Keep the channel READY/disabled. Every DMA descriptor is preloaded before
   * the exact PTP start edge, then the channel is enabled. This removes the
   * zero-descriptor ambiguity that would make EOF counting unreliable. */
  AUDIO_DIAG_PLAYOUT_I2S(
      s_nominal_mclk_hz, (uint32_t)I2S_DMA_DESC_NUM,
      (uint32_t)I2S_DMA_FRAME_NUM,
#if CONFIG_I2S_ISR_IRAM_SAFE
      1U
#else
      0U
#endif
  );
  s_tx = tx; /* Publish only a fully initialised channel. */
  ESP_LOGI(TAG, "I2S TX 32-bit data in 32-bit slots, %u Hz, MCLK=%" PRIu32 " Hz",
           (unsigned)s_rate_hz, s_nominal_mclk_hz);
  return ESP_OK;

fail:
  if (tx) (void)i2s_del_channel(tx);
  return err;
}

typedef struct {
  SemaphoreHandle_t done;
  esp_err_t result;
} audio_playout_init_ctx_t;

static void audio_playout_init_task(void *arg) {
  audio_playout_init_ctx_t *ctx = (audio_playout_init_ctx_t *)arg;
  ctx->result = audio_playout_init_current_core();
  xSemaphoreGive(ctx->done);
  vTaskDelete(NULL);
}

esp_err_t audio_playout_init(void) {
  if (s_tx) {
    return ESP_OK;
  }

#if CONFIG_FREERTOS_UNICORE
  return audio_playout_init_current_core();
#else
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) {
    return ESP_ERR_NO_MEM;
  }

  audio_playout_init_ctx_t ctx = {
      .done = done,
      .result = ESP_FAIL,
  };

  BaseType_t created = xTaskCreatePinnedToCore(
      audio_playout_init_task, "i2s_init", AUDIO_PLAYOUT_INIT_STACK, &ctx,
      AUDIO_PLAYOUT_INIT_PRIO, NULL, AUDIO_PLAYOUT_INIT_CORE);
  if (created != pdPASS) {
    vSemaphoreDelete(done);
    return ESP_ERR_NO_MEM;
  }

  (void)xSemaphoreTake(done, portMAX_DELAY);
  const esp_err_t result = ctx.result;
  vSemaphoreDelete(done);
  return result;
#endif
}

esp_err_t audio_playout_flush(void) {
  if (!s_tx) {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_enabled) {
    esp_err_t de = i2s_channel_disable(s_tx);
    if (de == ESP_OK || de == ESP_ERR_INVALID_STATE) {
      s_enabled = false;
      s_preload_pending = false;
    } else {
      return de;
    }
  } else if (s_preload_pending) {
    /* There is no public "discard preload" API. A failed realtime timeline
     * revalidation can happen after both silent descriptors were preloaded but
     * before normal enable. Leaving them there makes every later preload append
     * into an already-full DMA cache and PRIME can then persist until reboot.
     * Cycle READY -> RUNNING -> READY with silence only; this consumes/resets
     * the driver's preloaded DMA state without exposing stale program audio. */
    esp_err_t ee = i2s_channel_enable(s_tx);
    if (ee == ESP_OK) {
      s_enabled = true;
      esp_err_t de = i2s_channel_disable(s_tx);
      if (de == ESP_OK || de == ESP_ERR_INVALID_STATE) {
        s_enabled = false;
        s_preload_pending = false;
      } else {
        return de;
      }
    } else {
      /* If enable says the driver is not READY, try to converge it to READY.
       * This is exceptional; converge the channel back to READY. */
      esp_err_t de = i2s_channel_disable(s_tx);
      if (de == ESP_OK || de == ESP_ERR_INVALID_STATE) {
        s_enabled = false;
        s_preload_pending = false;
      } else {
        return de;
      }
    }
  }

  queues_reset();
  return ESP_OK;
}

esp_err_t audio_playout_preload_tagged(const audio_out_sample_t *stereo,
                                       uint32_t frames,
                                       uint32_t rtp, uint32_t generation) {
  if (!s_tx || !stereo || frames == 0U || s_enabled) {
    return ESP_ERR_INVALID_STATE;
  }
  size_t loaded = 0;
  const size_t bytes = (size_t)frames * I2S_FRAME_BYTES;
  esp_err_t err = i2s_channel_preload_data(s_tx, stereo, bytes, &loaded);
  if (loaded > 0U) s_preload_pending = true;
  if (err != ESP_OK || loaded != bytes) {
    return err == ESP_OK ? ESP_ERR_NO_MEM : err;
  }
  if (!tag_push(rtp, generation, frames)) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

esp_err_t audio_playout_enable(void) {
  if (!s_tx) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_enabled) {
    return ESP_OK;
  }
  esp_err_t err = i2s_channel_enable(s_tx);
  if (err == ESP_OK) {
    s_enabled = true;
    s_preload_pending = false;
  }
  return err;
}

esp_err_t audio_playout_write_tagged(const audio_out_sample_t *stereo,
                                     uint32_t frames,
                                     uint32_t rtp, uint32_t generation) {
  if (!s_tx || !s_enabled || !stereo || frames == 0U || audio_playout_has_fault()) {
    return ESP_ERR_INVALID_STATE;
  }

  /* Publish the tag before entering the write. While running there are
   * already outstanding tagged blocks, so an EOF that frees a descriptor for
   * this write always consumes the oldest earlier tag, never this one. */
  if (!tag_push(rtp, generation, frames)) {
    return ESP_ERR_NO_MEM;
  }

  size_t written = 0;
  const size_t bytes = (size_t)frames * I2S_FRAME_BYTES;
  esp_err_t err = i2s_channel_write(s_tx, stereo, bytes, &written,
                                    I2S_WRITE_TIMEOUT_MS);


  if (err == ESP_OK && written == bytes && !audio_playout_has_fault()) {
    /* RGB/VU is fed from the final I2S submission path. When disabled
     * in menuconfig this compiles to a no-op. */
    led_audio_feed(stereo, frames);
    live_dither_step();
    return ESP_OK;
  }

  /* A failed write invalidates the FIFO relationship between application tags
   * and DMA descriptors. Caller will flush/re-prime the channel. */
  return err == ESP_OK ? ESP_FAIL : err;
}

bool audio_playout_poll_completion(audio_playout_completion_t *out) {
  if (!out || audio_playout_has_fault()) {
    return false;
  }
  const uint32_t tail = __atomic_load_n(&s_done_tail, __ATOMIC_RELAXED);
  const uint32_t head = __atomic_load_n(&s_done_head, __ATOMIC_ACQUIRE);
  if (tail == head) {
    return false;
  }
  *out = s_done_q[tail];
  __atomic_store_n(&s_done_tail, (tail + 1U) % TX_DONE_Q_CAP,
                   __ATOMIC_RELEASE);
  return true;
}

bool audio_playout_wait_completion(audio_playout_completion_t *out,
                                    uint32_t timeout_ticks) {
  /* Arm before checking the queue: an EOF between checking and blocking must
   * leave a notification. Notifications are only a wake hint; the queue owns
   * the completion data, including when several EOFs coalesce. */
  __atomic_store_n(&s_completion_waiter, xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
  bool ready = audio_playout_poll_completion(out);
  if (!ready && !audio_playout_has_fault()) {
    (void)ulTaskNotifyTake(pdTRUE, (TickType_t)timeout_ticks);
    ready = audio_playout_poll_completion(out);
  }
  __atomic_store_n(&s_completion_waiter, NULL, __ATOMIC_RELEASE);
  return ready;
}

bool audio_playout_is_enabled(void) {
  return s_enabled;
}

uint32_t audio_playout_free_blocks(void) {
  return free_blocks();
}

bool audio_playout_wait_free(uint32_t blocks, uint32_t timeout_ticks) {
  if (blocks > I2S_DMA_DESC_NUM) blocks = I2S_DMA_DESC_NUM;
  __atomic_store_n(&s_wake_free_blocks, blocks, __ATOMIC_RELEASE);
  __atomic_store_n(&s_completion_waiter, xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
  bool ready = free_blocks() >= blocks;
  if (!ready && !audio_playout_has_fault()) {
    (void)ulTaskNotifyTake(pdTRUE, (TickType_t)timeout_ticks);
    ready = free_blocks() >= blocks;
  }
  __atomic_store_n(&s_completion_waiter, NULL, __ATOMIC_RELEASE);
  __atomic_store_n(&s_wake_free_blocks, 0U, __ATOMIC_RELEASE);
  return ready;
}

#if CONFIG_AIRPLAY_I2S_KEEP_RUNNING
/* Do not touch a descriptor the DMA starts reading within this time. */
#define FADE_GUARD_US 2000LL
#define DMA_BLOCK_US \
  ((int64_t)I2S_DMA_FRAME_NUM * 1000000LL / (int64_t)s_rate_hz)

static inline void sync_dma_buf(void *buf) {
#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
  (void)esp_cache_msync(buf, s_ring.size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#else
  (void)buf;
#endif
}

esp_err_t audio_playout_fade_out_queued(uint32_t *faded_frames) {
  if (faded_frames) *faded_frames = 0U;
  if (!s_tx || !s_enabled || audio_playout_has_fault()) {
    return ESP_ERR_INVALID_STATE;
  }
  /* All descriptors are known after the first DMA round (~87 ms of the first
   * start); before that the caller stops I2S instead. */
  if (__atomic_load_n(&s_ring.count, __ATOMIC_ACQUIRE) < I2S_DMA_DESC_NUM) {
    return ESP_ERR_NOT_FOUND;
  }

  /* Everything below runs as one critical section on this core (~0.4 ms
   * for a full fade: one multiply-add chain per frame, no division). The
   * plan picks descriptors the DMA will not start within FADE_GUARD_US; with
   * interrupts masked nothing can delay this task between the plan and the
   * last modified frame (no preemption, and a flash operation on the other
   * core has to wait until we are done), so the DMA can never read a
   * descriptor while it is being faded. The TX ISR is held off for that
   * time; the DMA itself keeps playing and the ISR has a whole block
   * (~11 ms) of slack. */
  portENTER_CRITICAL(&s_tag_mux);

  /* 1. One consistent picture: which descriptor finished last and when, and
   *    the queued tags. The queued tags now describe silence: generation 0,
   *    RTP 0, like the idle zeros (never a generation's sync observation,
   *    never a warm-prime probe id). */
  const uint32_t last = s_ring.last;
  const int64_t last_us = s_ring.last_us;
  const bool order_ok = s_ring.bad == 0U;
  const uint32_t head = __atomic_load_n(&s_tag_head, __ATOMIC_RELAXED);
  const uint32_t tail = __atomic_load_n(&s_tag_tail, __ATOMIC_RELAXED);
  const uint32_t queued = (head + TX_TAG_Q_CAP - tail) % TX_TAG_Q_CAP;
  for (uint32_t i = tail; i != head; i = (i + 1U) % TX_TAG_Q_CAP) {
    s_tag_q[i].rtp = 0U;
    s_tag_q[i].generation = 0U;
  }

  /* 2. Drop the completions reported before step 1 (consumer side, so this
   *    is safe against the producing ISR). */
  __atomic_store_n(&s_done_tail, __atomic_load_n(&s_done_head, __ATOMIC_ACQUIRE),
                   __ATOMIC_RELEASE);

  /* 3. In flight, in ring order: last+1 (playing now, its tag is the tail)
   *    up to last+queued (the newest). The playing one, and any the DMA
   *    starts within FADE_GUARD_US, play as they are; last+first ..
   *    last+queued fade to exactly zero (audio_fade_out_plan). Free
   *    descriptors are zero already (auto_clear); only this task writes
   *    descriptors, the ISR only zeroes finished ones. */
  uint32_t first = 0U;
  uint32_t fade_blocks = 0U;
  if (order_ok && last < I2S_DMA_DESC_NUM && queued <= I2S_DMA_DESC_NUM &&
      s_ring.size == I2S_DMA_FRAME_NUM * I2S_FRAME_BYTES) {
    fade_blocks = audio_fade_out_plan(queued, esp_timer_get_time() - last_us,
                                      DMA_BLOCK_US, FADE_GUARD_US, &first);
  }

  uint32_t j = 0U;
  if (fade_blocks == 0U) {
    /* Nothing that can still be faded safely: cut to zeros at once, the one
     * playing included, as with a disable. */
    for (uint32_t i = 0; i < I2S_DMA_DESC_NUM; ++i)
      memset(s_ring.buf[i], 0, s_ring.size);
  } else {
    /* Nearest descriptor first, as one continuous fade. */
    j = audio_fade_out_ring(s_ring.buf, I2S_DMA_DESC_NUM, last, first,
                            fade_blocks, I2S_DMA_FRAME_NUM);
  }
  portEXIT_CRITICAL(&s_tag_mux);

  /* Cache write-back (targets with cached internal RAM only; none on S3). */
  for (uint32_t i = 0; i < I2S_DMA_DESC_NUM; ++i) sync_dma_buf(s_ring.buf[i]);
  if (faded_frames) *faded_frames = j;
  return ESP_OK;
}
#endif


static void live_tune_init(void) {
  i2s_dev_t *hw = I2S_LL_GET_HW(I2S_NUM_0);
  s_live_div_int = 0;
  const uint32_t sel = hw->tx_clkm_conf.tx_clk_sel;
  const uint32_t n = hw->tx_clkm_conf.tx_clkm_div_num;
  if (sel != 2U || s_nominal_mclk_hz == 0U) {
    ESP_LOGE(TAG, "I2S clock tuning unavailable: clock source %u is not PLL_F160M",
             (unsigned)sel);
    return;
  }
  /* The whole +/-200 ppm range must keep the same integer divider. */
  const uint64_t lo = (uint64_t)s_nominal_mclk_hz * (1000000U - 200U);
  const uint64_t hi = (uint64_t)s_nominal_mclk_hz * (1000000U + 200U);
  const uint64_t sclk_u = (uint64_t)LIVE_SCLK_HZ * 1000000ULL;
  if (sclk_u / lo != n || sclk_u / hi != n) {
    ESP_LOGE(TAG, "I2S clock tuning unavailable: divider %u not constant over +/-200 ppm",
             (unsigned)n);
    return;
  }
  s_live_div_int = n;
  if (live_tune_write(0, NULL) != ESP_OK) {
    s_live_div_int = 0;
    ESP_LOGE(TAG, "I2S clock tuning unavailable: no divider levels around nominal");
    return;
  }
  ESP_LOGI(TAG, "I2S clock tuning: LIVE, one register write, I2S never stops"
                " (MCLK = 160 MHz / %u.xxx)", (unsigned)n);
}

/* Fractional divider levels and dithering.
 *
 * MCLK = 160 MHz / (n + b/a) with a < 512. Around 44.1 kHz x 256 (n = 14,
 * fraction ~0.172) the reachable frequencies are up to ~6 ppm apart, and the
 * nearest level can miss a requested ppm by ~3 ppm. The servo then hunts
 * between levels and parks hundreds of microseconds off. Instead, every
 * requested ppm is bracketed by the two nearest levels and the playout task
 * alternates them per DMA block (first-order sigma-delta), so the AVERAGE
 * clock is exactly the requested one. Adjacent levels differ by a few ppm:
 * per block that is a phase wobble of tens of nanoseconds. */
typedef struct {
  uint32_t reg;    /* tx_clkm_div_conf value (x, y, z, yn1; reserved kept) */
  int64_t ppb;     /* exact MCLK offset of this level vs nominal */
} live_level_t;

static live_level_t s_dith_lo, s_dith_hi; /* lo.ppb <= target <= hi.ppb */
static int64_t s_dith_num;  /* target - lo */
static int64_t s_dith_den;  /* hi - lo (0: a single exact level) */
static int64_t s_dith_acc;
static uint32_t s_dith_reg; /* level currently in the register */

static uint32_t live_level_reg(uint64_t a, uint64_t b) {
  uint32_t x = 0, y = 0, z = 0, yn1 = 0;
  if (b) {
    yn1 = b * 2U > a;
    z = (uint32_t)(yn1 ? a - b : b);
    x = (uint32_t)(a / z) - 1U;
    y = (uint32_t)(a % z);
  }
  i2s_dev_t *hw = I2S_LL_GET_HW(I2S_NUM_0);
  return (hw->tx_clkm_div_conf.val & 0xF0000000U) | (z & 0x1FFU) |
         ((y & 0x1FFU) << 9) | ((x & 0x1FFU) << 18) | ((yn1 & 1U) << 27);
}

/* Exact offset of MCLK = sclk / (n + b/a) from nominal, in ppb. */
static int64_t live_level_ppb(uint64_t n, uint64_t a, uint64_t b) {
  const int64_t num = (int64_t)((uint64_t)LIVE_SCLK_HZ * a);
  const int64_t den = (int64_t)((uint64_t)s_nominal_mclk_hz * (n * a + b));
  return (num - den) * 1000000000LL / den;
}

/* The two divider levels around target_ppb. Integer math, a < 512. */
static esp_err_t live_bracket(int32_t target_ppm, live_level_t *lo,
                              live_level_t *hi) {
  if (s_live_div_int == 0U) return ESP_ERR_INVALID_STATE;
  /* M = target MCLK in micro-Hz; LIVE_SCLK_HZ * 1e6 = n * M + r. */
  const uint64_t M = (uint64_t)s_nominal_mclk_hz * (uint64_t)(1000000 + target_ppm);
  const uint64_t sclk_u = (uint64_t)LIVE_SCLK_HZ * 1000000ULL;
  const uint64_t n = sclk_u / M;
  if (n != s_live_div_int) return ESP_ERR_INVALID_ARG;
  const uint64_t r = sclk_u - n * M; /* target fraction = r / M */
  /* fl = largest b/a <= r/M (higher MCLK), ce = smallest b/a >= r/M. */
  uint64_t fl_a = 1, fl_b = 0, ce_a = 1, ce_b = 1;
  for (uint64_t a = 2; a < I2S_LL_CLK_FRAC_DIV_AB_MAX; ++a) {
    const uint64_t ra = r * a;
    const uint64_t bf = ra / M;
    const uint64_t bc = bf + (ra % M != 0U ? 1U : 0U);
    if (bf >= 1U && bf < a && bf * fl_a > fl_b * a) { fl_a = a; fl_b = bf; }
    if (bc >= 1U && bc < a && bc * ce_a < ce_b * a) { ce_a = a; ce_b = bc; }
  }
  if (fl_b == 0U || ce_b >= ce_a) return ESP_ERR_INVALID_ARG;
  hi->reg = live_level_reg(fl_a, fl_b);
  hi->ppb = live_level_ppb(n, fl_a, fl_b);
  lo->reg = live_level_reg(ce_a, ce_b);
  lo->ppb = live_level_ppb(n, ce_a, ce_b);
  return ESP_OK;
}

/* Set the average TX MCLK to nominal * (1 + ppm / 1e6). Applies the nearer
 * level at once; live_dither_step() then alternates per block. */
static esp_err_t live_tune_write(int32_t ppm, uint32_t *mclk_hz_out) {
  live_level_t lo, hi;
  const esp_err_t err = live_bracket(ppm, &lo, &hi);
  if (err != ESP_OK) return err;
  const int64_t target_ppb = (int64_t)ppm * 1000LL;
  s_dith_lo = lo;
  s_dith_hi = hi;
  s_dith_num = target_ppb - lo.ppb;
  s_dith_den = hi.ppb - lo.ppb;
  if (s_dith_num < 0) s_dith_num = 0;
  if (s_dith_num > s_dith_den) s_dith_num = s_dith_den;
  s_dith_acc = 0;
  s_dith_reg = (s_dith_den == 0 || s_dith_num * 2 < s_dith_den) ? lo.reg : hi.reg;
  I2S_LL_GET_HW(I2S_NUM_0)->tx_clkm_div_conf.val = s_dith_reg; /* one store */
  if (mclk_hz_out) {
    *mclk_hz_out = (uint32_t)((int64_t)s_nominal_mclk_hz +
                              ((int64_t)s_nominal_mclk_hz * ppm) / 1000000LL);
  }
  return ESP_OK;
}

/* One sigma-delta step per written DMA block (playout task only). */
static void live_dither_step(void) {
  if (s_live_div_int == 0U || s_dith_den == 0) return;
  s_dith_acc += s_dith_num;
  uint32_t reg = s_dith_lo.reg;
  if (s_dith_acc >= s_dith_den) {
    s_dith_acc -= s_dith_den;
    reg = s_dith_hi.reg;
  }
  if (reg != s_dith_reg) {
    s_dith_reg = reg;
    I2S_LL_GET_HW(I2S_NUM_0)->tx_clkm_div_conf.val = reg;
  }
}

uint32_t audio_playout_get_rate(void) { return s_rate_hz; }

esp_err_t audio_playout_set_rate(uint32_t rate_hz) {
  if (!s_tx || rate_hz == 0U) {
    return ESP_ERR_INVALID_STATE;
  }
  if (rate_hz == s_rate_hz) {
    return ESP_OK;
  }
  const uint32_t previous_rate = s_rate_hz;
  /* The clock can only be reprogrammed in READY: stop and drop the queue. */
  esp_err_t err = audio_playout_flush();
  if (err != ESP_OK) {
    return err;
  }
  const i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz);
  err = i2s_channel_reconfig_std_clock(s_tx, &clk);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S rate %u Hz rejected: %s", (unsigned)rate_hz,
             esp_err_to_name(err));
    return err;
  }
  s_rate_hz = rate_hz;
  /* The servo's ppm corrections are relative to the nominal MCLK of the new
   * rate: measure it again and rebuild the live divider levels. */
  s_tune_ppm = 0;
  i2s_tuning_info_t tune = {0};
  if (i2s_channel_tune_rate(s_tx, NULL, &tune) == ESP_OK) {
    s_nominal_mclk_hz = tune.curr_mclk_hz;
  } else {
    s_nominal_mclk_hz = rate_hz * 256U;
  }
  live_tune_init();
  ESP_LOGI(TAG, "I2S RATE %u -> %u Hz, MCLK=%" PRIu32 " Hz",
           (unsigned)previous_rate, (unsigned)rate_hz, s_nominal_mclk_hz);
  return ESP_OK;
}

bool audio_playout_tune_is_live(void) {
  return s_live_div_int != 0U;
}

esp_err_t audio_playout_reset_tune(void) {
  if (!s_tx || s_nominal_mclk_hz == 0U) {
    return ESP_ERR_INVALID_STATE;
  }
  if (s_tune_ppm == 0) {
    return ESP_OK;
  }
  const esp_err_t le = live_tune_write(0, NULL);
  if (le == ESP_OK) s_tune_ppm = 0;
  return le;
}

esp_err_t audio_playout_tune_ppm(int32_t target_ppm,
                                 audio_playout_tune_info_t *out,
                                 uint32_t *halt_us) {
  if (halt_us) *halt_us = 0;
  if (!s_tx || s_nominal_mclk_hz == 0U) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!audio_playout_tune_is_live()) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (target_ppm > 160) target_ppm = 160;
  if (target_ppm < -160) target_ppm = -160;
  if (target_ppm == s_tune_ppm) {
    if (out) {
      out->requested_ppm = s_tune_ppm;
      out->curr_mclk_hz = (uint32_t)((int64_t)s_nominal_mclk_hz +
          ((int64_t)s_nominal_mclk_hz * s_tune_ppm) / 1000000LL);
      out->actual_delta_mclk_hz = (int32_t)out->curr_mclk_hz -
                                   (int32_t)s_nominal_mclk_hz;
    }
    return ESP_OK;
  }

  /* One register store: nothing stops, the DMA is not touched.
   * halt_us = time spent here. */
  const int64_t lt0 = esp_timer_get_time();
  uint32_t mclk_hz = 0;
  const esp_err_t le = live_tune_write(target_ppm, &mclk_hz);
  if (halt_us) *halt_us = (uint32_t)(esp_timer_get_time() - lt0);
  if (le == ESP_OK) {
    s_tune_ppm = target_ppm;
    if (out) {
      out->requested_ppm = target_ppm;
      out->curr_mclk_hz = mclk_hz;
      out->actual_delta_mclk_hz = (int32_t)mclk_hz - (int32_t)s_nominal_mclk_hz;
    }
  }
  return le;
}
