#include "audio_diag.h"

#if defined(CONFIG_AIRPLAY_DIAG_ACTIVE) && CONFIG_AIRPLAY_DIAG_ACTIVE

#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AUDIO_DIAG_STACK       3072U
#define AUDIO_DIAG_PRIORITY       1U
#define AUDIO_DIAG_CORE           0U
#define AUDIO_DIAG_EVENT_DEPTH   24U

static const char *TAG = "audio_diag";
static TaskHandle_t s_diag_task;

/* Audio and control code only post numbers; the low-priority diagnostic task
 * formats and prints them, so a log line never blocks an audio path. */
typedef enum {
  DIAG_EVENT_NONE = 0,
  DIAG_EVENT_TASK_STARTED,
  DIAG_EVENT_REALTIME_STOPPED,
  DIAG_EVENT_REALTIME_EPOCH,
  DIAG_EVENT_I2S,
} diag_event_kind_t;

typedef struct {
  uint16_t kind;
  uint16_t id;
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
} diag_event_t;

static StaticQueue_t s_event_queue_struct;
static uint8_t s_event_queue_storage[AUDIO_DIAG_EVENT_DEPTH * sizeof(diag_event_t)];
static QueueHandle_t s_event_queue;

static void diag_emit(diag_event_kind_t kind, uint16_t id,
                      uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  QueueHandle_t q = __atomic_load_n(&s_event_queue, __ATOMIC_ACQUIRE);
  if (!q) return;
  const diag_event_t ev = {
      .kind = (uint16_t)kind, .id = id, .a = a, .b = b, .c = c, .d = d,
  };
  /* Diagnostics are never allowed to backpressure audio/control code. */
  (void)xQueueSend(q, &ev, 0);
}

#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
void audio_diag_lifecycle_task_started(audio_diag_task_id_t task_id, int core,
                                       uint32_t priority, uint32_t extra) {
  diag_emit(DIAG_EVENT_TASK_STARTED, (uint16_t)task_id,
            (uint32_t)core, priority, extra, 0U);
}
void audio_diag_lifecycle_realtime_stopped(void) {
  diag_emit(DIAG_EVENT_REALTIME_STOPPED, 0U, 0U, 0U, 0U, 0U);
}
void audio_diag_lifecycle_realtime_epoch(uint32_t generation) {
  diag_emit(DIAG_EVENT_REALTIME_EPOCH, 0U, generation, 0U, 0U, 0U);
}

static const char *task_name(uint16_t id) {
  switch ((audio_diag_task_id_t)id) {
    case AUDIO_DIAG_TASK_AAC_PROCESSOR: return "AAC_PROCESSOR";
    case AUDIO_DIAG_TASK_ALAC_STAGE: return "ALAC_STAGE";
    case AUDIO_DIAG_TASK_PLAYOUT: return "PLAYOUT";
    case AUDIO_DIAG_TASK_RT_DATA: return "RT_DATA";
    case AUDIO_DIAG_TASK_RT_CTRL: return "RT_CTRL";
    case AUDIO_DIAG_TASK_RT_WORK: return "RT_WORK";
    case AUDIO_DIAG_TASK_RT_RESEND: return "RT_RESEND";
    case AUDIO_DIAG_TASK_TCP_READER: return "TCP_READER";
    default: return "UNKNOWN";
  }
}
#endif

#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
void audio_diag_playout_i2s(uint32_t mclk_hz, uint32_t dma_desc,
                            uint32_t dma_frames, uint32_t isr_iram_safe) {
  diag_emit(DIAG_EVENT_I2S, 0U, mclk_hz, dma_desc, dma_frames, isr_iram_safe);
}
#endif

static void log_event(const diag_event_t *ev) {
  switch ((diag_event_kind_t)ev->kind) {
#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
    case DIAG_EVENT_TASK_STARTED:
      ESP_LOGI(TAG, "LIFECYCLE task=%s core=%lu prio=%lu extra=%lu",
               task_name(ev->id), (unsigned long)ev->a,
               (unsigned long)ev->b, (unsigned long)ev->c);
      break;
    case DIAG_EVENT_REALTIME_STOPPED:
      ESP_LOGI(TAG, "LIFECYCLE realtime stopped");
      break;
    case DIAG_EVENT_REALTIME_EPOCH:
      ESP_LOGI(TAG, "LIFECYCLE realtime epoch gen=%lu awaiting sender anchor",
               (unsigned long)ev->a);
      break;
#endif
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
    case DIAG_EVENT_I2S:
      ESP_LOGI(TAG, "PLAYOUT I2S mclk=%luHz dma=%lux%lu ISR_IRAM=%lu",
               (unsigned long)ev->a, (unsigned long)ev->b,
               (unsigned long)ev->c, (unsigned long)ev->d);
      break;
#endif
    default:
      break;
  }
}

static void audio_diag_task(void *arg) {
  (void)arg;
  ESP_LOGI(TAG, "ENABLED:"
#if defined(CONFIG_AIRPLAY_DIAG_PLAYOUT) && CONFIG_AIRPLAY_DIAG_PLAYOUT
                " PLAYOUT"
#endif
#if defined(CONFIG_AIRPLAY_DIAG_LIFECYCLE) && CONFIG_AIRPLAY_DIAG_LIFECYCLE
                " LIFECYCLE"
#endif
  );
  for (;;) {
    diag_event_t ev;
    if (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) == pdTRUE)
      log_event(&ev);
  }
}

esp_err_t audio_diag_init(void) {
  if (__atomic_load_n(&s_diag_task, __ATOMIC_ACQUIRE) != NULL) return ESP_OK;

  if (!s_event_queue) {
    QueueHandle_t q = xQueueCreateStatic(AUDIO_DIAG_EVENT_DEPTH,
                                         sizeof(diag_event_t),
                                         s_event_queue_storage,
                                         &s_event_queue_struct);
    if (!q) return ESP_ERR_NO_MEM;
    __atomic_store_n(&s_event_queue, q, __ATOMIC_RELEASE);
  }

  TaskHandle_t task = NULL;
  if (xTaskCreatePinnedToCore(audio_diag_task, "audio_diag", AUDIO_DIAG_STACK,
                              NULL, AUDIO_DIAG_PRIORITY, &task,
                              AUDIO_DIAG_CORE) != pdPASS) {
    ESP_LOGE(TAG, "diagnostic task create failed");
    return ESP_ERR_NO_MEM;
  }
  __atomic_store_n(&s_diag_task, task, __ATOMIC_RELEASE);
  return ESP_OK;
}

#endif /* CONFIG_AIRPLAY_DIAG_ACTIVE */
