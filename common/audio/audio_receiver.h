#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/* Raw AirPlay-2 buffered TCP FIFO. The receiver stores bytes exactly in TCP
 * order; framing and FLUSH interpretation belong to the sequential consumer. */
#define AP2_BUFFERED_AUDIO_BUFFER_REQUEST_BYTES (6U * 1024U * 1024U)

/* AirPlay type-103 capacity advertised to the sender. Keep this equal to the
 * physical 6 MiB raw FIFO so normal TCP backpressure bounds sender preload. */
#define AP2_BUFFERED_AUDIO_ADVERTISED_BYTES (6U * 1024U * 1024U)

/* AirPlay 2 audio receiver: buffered AAC/ALAC plus realtime ALAC. Implemented
 * twice: the speaker's audio engine (speaker/main/audio/audio_receiver.c) and
 * the Gateway's proxy that forwards each call to the speakers (gw_proxy.c). */
typedef struct {
  char codec[32];
  int sample_rate;
  int channels;
  int bits_per_sample;
  int frame_size;
} audio_format_t;

/* Initial I2S rate; the playout task switches to the active stream's rate. */
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#define AUDIO_PIPELINE_RATE_HZ 48000U
#else
#define AUDIO_PIPELINE_RATE_HZ 44100U
#endif

typedef enum {
  AUDIO_ENCRYPT_NONE = 0,
  AUDIO_ENCRYPT_CHACHA20_POLY1305
} audio_encrypt_type_t;

typedef struct {
  audio_encrypt_type_t type;
  uint8_t key[32];
  uint8_t iv[16];
  size_t key_len;
} audio_encrypt_t;

typedef enum {
  AUDIO_STREAM_NONE = 0,
  AUDIO_STREAM_REALTIME = 96,  /* AP2 realtime UDP ALAC */
  AUDIO_STREAM_BUFFERED = 103  /* AP2 buffered TCP AAC */
} audio_stream_type_t;

esp_err_t audio_receiver_init(void);
bool audio_receiver_is_initialized(void);
/* True only at a completed media stop boundary; persistent workers may exist. */
bool audio_receiver_is_idle(void);
/* Wi-Fi scans need temporary heap headroom. This is a full audio-engine
 * memory release (except for the already-created I2S driver itself): current
 * media is stopped, audio worker tasks exit, all large codec/PCM stores are
 * freed, and audio_receiver_init() may be called afterwards to restore the
 * engine. Call only when no stream can start: on the speaker after
 * spk_link_suspend(), on the Gateway after the RTSP server stopped. */
esp_err_t audio_receiver_release_for_wifi_scan(void);
void audio_receiver_set_format(const audio_format_t *format);
uint32_t audio_receiver_get_sample_rate(void);
void audio_receiver_set_encryption(const audio_encrypt_t *encrypt);
void audio_receiver_set_stream_type(audio_stream_type_t type);

esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port);
esp_err_t audio_receiver_start_buffered(uint16_t tcp_port);
void audio_receiver_stop(void);
uint16_t audio_receiver_get_buffered_port(void);

/* Software output volume. Q15: 0=mute, 32768=0 dB/full scale. */
void audio_receiver_set_volume_q15(int32_t volume_q15);
int32_t audio_receiver_get_volume_q15(void);
/* Physical outputs: channel 0 = left, 1 = right. Applied after the PCM FIFO,
 * independently of EQ/source mode and RTSP volume. Runtime only, no NVS. */
uint32_t audio_receiver_get_output_mute_mask(void);
esp_err_t audio_receiver_set_output_muted(uint32_t channel, bool muted);

void audio_receiver_seek_flush(void);
/* AP2 realtime FLUSH with RTP-Info: discard audio older than the sender's
 * RTP boundary while preserving the validated D7/SETRATE RTP<->PTP map. */
void audio_receiver_realtime_flush_to_rtp(uint32_t flush_rtp);
void audio_receiver_realtime_flush_wait_sender_anchor(void);
/* timestamps_valid is key presence, not numeric nonzero: RTP zero is valid.
 * A usable timestamp interval additionally invalidates the matching PCM range. */
esp_err_t audio_receiver_set_deferred_flush_range(uint32_t from_seq, uint32_t from_ts,
                                                   uint32_t until_seq, uint32_t until_ts,
                                                   bool timestamps_valid);
void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts);
void audio_receiver_pause(void);
void audio_receiver_set_playing(bool playing);

void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time);
typedef struct {
  uint64_t effective_local_ns;
  int64_t rebase_step_ns;
  int64_t rebase_bias_ns;
  bool rebased;
  bool deferred;
} audio_realtime_anchor_result_t;

/* Realtime-only D7/SETRATE anchor already converted into ESP monotonic time.
 *
 * Initial startup accepts the current ready GM immediately. While audio is
 * already running, a new PTP mastership epoch is intentionally kept out of
 * the media phase until it has been stable for the handover settle period.
 * The first accepted anchor from that epoch is then rebased onto the existing
 * RTP<->ESP-local timeline, and the resulting constant media-domain bias is
 * applied to later anchors from the same epoch. PTP itself is never biased or
 * slewed, and buffered/AAC timing is untouched.
 *
 * Returns true when the anchor was published. False means it was deliberately
 * deferred (typically because a new GM is still settling) or not applicable.
 */
bool audio_receiver_set_realtime_anchor_local(
    uint64_t clock_id, uint32_t gm_epoch, uint32_t mastership_age_ms,
    uint64_t remote_ptp_ns, uint64_t candidate_local_ns, uint32_t rtp_time,
    audio_realtime_anchor_result_t *result);
void audio_receiver_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port);

/* Realtime (type 96): sender latencyMin in samples, added to the anchor.
 * Buffered (type 103) plays at the anchor itself and uses 0. */
void audio_receiver_set_playout_latency_samples(uint32_t latency_samples);

/* Output latency after the ESP (DAC/DSP), microseconds. Positive =
 * the chain delays the sound, so the ESP plays that much earlier. Applied
 * live; persist=true also stores it in NVS. Range -100000..150000. */
#include "latency_cal.h"
int32_t audio_receiver_get_output_latency_us(void);
esp_err_t audio_receiver_set_output_latency_us(int32_t us, bool persist);
/* Surround speaker: play this much later than the anchor says (distance to
 * the listener), on top of the output latency. Not saved here. */
esp_err_t audio_receiver_set_extra_delay_us(int32_t us);
/* Surround speaker: which part of a stereo source it plays, from its role
 * (spk_link.c). A 5.1/7.1 stream is not affected: it already arrives as the
 * speaker's own channel. Applied before the EQ, so the EQ's own source
 * selection still works on top. AUDIO_STEREO_ROUTE_STEREO until the link
 * sets a route. Runtime only, no NVS. */
typedef enum {
  AUDIO_STEREO_ROUTE_STEREO = 0, /* left and right unchanged */
  AUDIO_STEREO_ROUTE_LEFT,       /* left channel on both outputs */
  AUDIO_STEREO_ROUTE_RIGHT,
  AUDIO_STEREO_ROUTE_MONO,       /* (L+R)/2 on both outputs */
  AUDIO_STEREO_ROUTE_MUTE,
} audio_stereo_route_t;
void audio_receiver_set_stereo_route(audio_stereo_route_t route);

/* What the receiver is playing, for status pages (Surround Gateway list). */
typedef struct {
  bool active;  /* a stream is started */
  bool playing;
  audio_stream_type_t stream_type;
  audio_format_t format;
  bool sync_valid; /* sync_us/servo_ppm belong to the running output */
  int32_t sync_us;    /* output phase error, + = early */
  int32_t servo_ppm;  /* I2S clock correction */
  uint32_t fifo_used_kib; /* buffered only */
  uint32_t fifo_capacity_kib;
} audio_receiver_playback_info_t;
void audio_receiver_get_playback_info(audio_receiver_playback_info_t *out);

/* Speaker: wired ADC loopback measurement. The caller must first suspend the
 * Gateway link and call audio_receiver_release_for_wifi_scan() (exactly like
 * the Wi-Fi scan) and resume afterwards. Blocks ~2-3 s. Does NOT apply the
 * result. */
esp_err_t audio_receiver_measure_output_latency(latency_cal_result_t *res,
                                                bool audible);
