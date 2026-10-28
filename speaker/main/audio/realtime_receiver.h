#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_receiver.h"
#include "esp_err.h"

typedef bool (*realtime_pcm_sink_t)(uint32_t rtp, int16_t *pcm,
                                    size_t frames, int channels, void *ctx);

/* Return the remaining time until the first sample at `rtp` reaches the
 * physical playout deadline. False means that no valid PTP/RTP timeline is
 * available yet (for example before the first realtime local anchor). */
typedef bool (*realtime_deadline_cb_t)(uint32_t rtp,
                                       int64_t *time_to_play_us, void *ctx);

/* Shairport-style recovery ends only when ordered staging must commit the
 * frame. Keep the final loss decision and the staging silence deadline on the
 * same boundary so an in-flight RTX remains useful after the last NACK. */
#define REALTIME_RECOVERY_FINAL_MARGIN_US 50000LL


typedef struct {
  uint32_t work_queue_depth;
  uint32_t work_queue_capacity;
  uint32_t dropped_data;
  uint32_t dropped_rtx;
  uint32_t missing_backpressure;
  uint32_t received_backpressure;
} realtime_receiver_usage_t;

typedef struct {
  audio_format_t format;
  audio_encrypt_t encrypt;
  realtime_pcm_sink_t pcm_sink;
  void *pcm_sink_ctx;
  realtime_deadline_cb_t deadline_cb;
  void *deadline_ctx;
} realtime_receiver_config_t;

/* Large ingress packet pools may live in caller-owned shared PSRAM. This is
 * intentionally limited to DATA/RTX slots; queues, decoder scratch and loss
 * tracking keep their existing allocation and task logic. Call only before
 * the first realtime start while the receiver is idle. */
size_t realtime_receiver_packet_workspace_size(void);
esp_err_t realtime_receiver_set_packet_workspace(void *workspace,
                                                  size_t workspace_bytes);
/* Detach caller-owned DATA/RTX packet storage before that shared workspace is
 * freed. The realtime receiver must already be fully stopped. */
esp_err_t realtime_receiver_clear_packet_workspace(void);

/* Reserve the DATA task's internal stack/TCB before sessions fragment RAM.
 * The reservation survives stop/start and audio release for Wi-Fi scanning. */
esp_err_t realtime_receiver_reserve_data_task_memory(void);

esp_err_t realtime_receiver_start(uint16_t data_port, uint16_t control_port,
                                  const realtime_receiver_config_t *config);
void realtime_receiver_stop(void);
bool realtime_receiver_is_running(void);
void realtime_receiver_get_usage(realtime_receiver_usage_t *out);
void realtime_receiver_set_client_control(uint32_t client_ip, uint16_t client_control_port);

/* True only after every old producer has exited. */
bool realtime_receiver_is_idle(void);
