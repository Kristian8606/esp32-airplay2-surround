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
 * twice: the speaker's audio engine (speaker/main/audio/sound_engine.c) and
 * the Gateway's proxy that forwards each call to the speakers (gw_proxy.c). */
typedef struct {
  char codec[32];
  int sample_rate;
  int channels;
  int bits_per_sample;
  int frame_size;
} sound_format_t;

/* Initial I2S rate; the playout task switches to the active stream's rate. */
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#define SOUND_PIPELINE_RATE_HZ 48000U
#else
#define SOUND_PIPELINE_RATE_HZ 44100U
#endif

typedef enum {
  SOUND_CIPHER_NONE = 0,
  SOUND_CIPHER_CHACHA20_POLY1305
} sound_cipher_kind_t;

typedef struct {
  sound_cipher_kind_t type;
  uint8_t key[32];
  uint8_t iv[16];
  size_t key_len;
} sound_cipher_t;

/* The key for ap2_audio_decrypt_*(): NULL for a stream that is not
 * encrypted. */
static inline const uint8_t *sound_cipher_key(const sound_cipher_t *e) {
  return e->type == SOUND_CIPHER_CHACHA20_POLY1305 ? e->key : NULL;
}

typedef enum {
  SOUND_STREAM_NONE = 0,
  SOUND_STREAM_REALTIME = 96,  /* AP2 realtime UDP ALAC */
  SOUND_STREAM_BUFFERED = 103  /* AP2 buffered TCP AAC */
} sound_stream_t;

esp_err_t sound_engine_init(void);
bool sound_engine_is_initialized(void);
/* True only at a completed media stop boundary; persistent workers may exist. */
bool sound_engine_is_idle(void);
/* Wi-Fi scans need temporary heap headroom. This is a full audio-engine
 * memory release (except for the already-created I2S driver itself): current
 * media is stopped, audio worker tasks exit, all large codec/PCM stores are
 * freed, and sound_engine_init() may be called afterwards to restore the
 * engine. Call only when no stream can start: on the speaker after
 * spk_link_suspend(), on the Gateway after the RTSP server stopped. */
esp_err_t sound_engine_release_for_wifi_scan(void);
void sound_engine_set_format(const sound_format_t *format);
uint32_t sound_engine_get_sample_rate(void);
void sound_engine_set_encryption(const sound_cipher_t *encrypt);
void sound_engine_set_stream_type(sound_stream_t type);

esp_err_t sound_engine_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port);
esp_err_t sound_engine_start_buffered(uint16_t tcp_port);
void sound_engine_stop(void);
uint16_t sound_engine_get_buffered_port(void);

/* Software output volume. Q15: 0=mute, 32768=0 dB/full scale. */
void sound_engine_set_volume_q15(int32_t volume_q15);
int32_t sound_engine_get_volume_q15(void);
/* Physical outputs: channel 0 = left, 1 = right. Applied after the PCM FIFO,
 * independently of EQ/source mode and RTSP volume. Runtime only, no NVS. */
uint32_t sound_engine_get_output_mute_mask(void);
esp_err_t sound_engine_set_output_muted(uint32_t channel, bool muted);

void sound_engine_seek_flush(void);
/* AP2 realtime FLUSH with RTP-Info: discard audio older than the sender's
 * RTP boundary while preserving the validated D7/SETRATE RTP<->PTP map. */
void sound_engine_realtime_flush_to_rtp(uint32_t flush_rtp);
void sound_engine_realtime_flush_wait_sender_anchor(void);
/* timestamps_valid is key presence, not numeric nonzero: RTP zero is valid.
 * A usable timestamp interval additionally invalidates the matching PCM range. */
esp_err_t sound_engine_set_deferred_flush_range(uint32_t from_seq, uint32_t from_ts,
                                                   uint32_t until_seq, uint32_t until_ts,
                                                   bool timestamps_valid);
void sound_engine_set_immediate_flush(uint32_t until_seq, uint32_t until_ts);
void sound_engine_pause(void);
void sound_engine_set_playing(bool playing);

void sound_engine_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time);
typedef struct {
  uint64_t effective_local_ns;
  int64_t rebase_step_ns;
  int64_t rebase_bias_ns;
  bool rebased;
  bool deferred;
} sound_realtime_anchor_result_t;

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
bool sound_engine_set_realtime_anchor_local(
    uint64_t clock_id, uint32_t gm_epoch, uint32_t mastership_age_ms,
    uint64_t remote_ptp_ns, uint64_t candidate_local_ns, uint32_t rtp_time,
    sound_realtime_anchor_result_t *result);
void sound_engine_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port);

/* Realtime (type 96): sender latencyMin in samples, added to the anchor.
 * Buffered (type 103) plays at the anchor itself and uses 0. */
void sound_engine_set_playout_latency_samples(uint32_t latency_samples);

/* Output latency after the ESP (DAC/DSP), microseconds. Positive =
 * the chain delays the sound, so the ESP plays that much earlier. Applied
 * live; persist=true also stores it in NVS. Range -100000..150000. */
#include "latency_cal.h"
int32_t sound_engine_get_output_latency_us(void);
esp_err_t sound_engine_set_output_latency_us(int32_t us, bool persist);
/* Surround speaker: play this much later than the anchor says (distance to
 * the listener), on top of the output latency. Not saved here. */
esp_err_t sound_engine_set_extra_delay_us(int32_t us);
/* Surround speaker: which part of a stereo source it plays, from its role
 * (spk_link.c). A 5.1/7.1 stream is not affected: it already arrives as the
 * speaker's own channel. Applied before the EQ, so the EQ's own source
 * selection still works on top. SOUND_STEREO_ROUTE_STEREO until the link
 * sets a route. Runtime only, no NVS. */
typedef enum {
  SOUND_STEREO_ROUTE_STEREO = 0, /* left and right unchanged */
  SOUND_STEREO_ROUTE_LEFT,       /* left channel on both outputs */
  SOUND_STEREO_ROUTE_RIGHT,
  SOUND_STEREO_ROUTE_MONO,       /* (L+R)/2 on both outputs */
  SOUND_STEREO_ROUTE_MUTE,
} sound_stereo_route_t;
void sound_engine_set_stereo_route(sound_stereo_route_t route);
/* Surround subwoofer: crossover low-pass on everything it plays (a stereo
 * source's mix and the LFE channel alike), Linkwitz-Riley 24 dB/octave,
 * before the EQ. 0 = off, the default; spk_link.c sets it from the role.
 * Heard within the DMA queue (~87 ms). Runtime only, no NVS. */
void sound_engine_set_lfe_lowpass_hz(uint32_t hz);

/* What the receiver is playing, for status pages (Surround Gateway list). */
typedef struct {
  bool active;  /* a stream is started */
  bool playing;
  sound_stream_t stream_type;
  sound_format_t format;
  bool sync_valid; /* sync_us/servo_ppm belong to the running output */
  int32_t sync_us;    /* output phase error, + = early */
  int32_t servo_ppm;  /* I2S clock correction */
  uint32_t fifo_used_kib; /* buffered only */
  uint32_t fifo_capacity_kib;
} sound_engine_playback_info_t;
void sound_engine_get_playback_info(sound_engine_playback_info_t *out);

/* Speaker: wired ADC loopback measurement. The caller must first suspend the
 * Gateway link and call sound_engine_release_for_wifi_scan() (exactly like
 * the Wi-Fi scan) and resume afterwards. Blocks ~2-3 s. Does NOT apply the
 * result. */
esp_err_t sound_engine_measure_output_latency(latency_cal_result_t *res,
                                                bool audible);
