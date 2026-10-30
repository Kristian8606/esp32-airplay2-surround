/* Gateway build: the audio_receiver.h API without a decoder or I2S output.
 *
 * RTSP (rtsp_handlers.c, rtsp_server.c, rtsp_conn.c) is built unchanged and
 * calls these functions exactly as on a speaker. Each call keeps the state
 * the Gateway must answer RTSP from by itself (port, idle, sample rate,
 * volume), is logged with a "FWD" prefix and is forwarded with the same
 * arguments to the speakers (gw_speakers.c), which make the same call.
 * The buffered audio stream is accepted by gw_relay.c, the realtime UDP
 * stream by gw_rt_relay.c; both copy it unchanged to every speaker. */

#include "audio_receiver.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "airplay_audio_formats.h"
#include "airplay_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gw_relay.h"
#include "gw_rt_relay.h"
#include "gw_speakers.h"
#include "ptp_clock.h"
#include "ptp_clock_engine.h"

static const char *TAG = "gw_proxy";

/* The key is copied between these two structs byte for byte. */
static_assert(sizeof(((audio_encrypt_t *)0)->key) ==
                  sizeof(((surround_encryption_t *)0)->key),
              "key size");
static_assert(sizeof(((audio_encrypt_t *)0)->iv) ==
                  sizeof(((surround_encryption_t *)0)->iv),
              "iv size");

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
  bool initialized;
  bool started; /* a stream was started and not stopped yet */
  bool session; /* the speakers hold stream state (format or start) */
  bool playing;
  audio_stream_type_t stream_type;
  audio_format_t format;
  audio_encrypt_t encrypt;
  int32_t volume_q15;
  uint16_t buffered_port;
} s = {.volume_q15 = 32768};

static const char *stream_type_name(audio_stream_type_t t) {
  switch (t) {
    case AUDIO_STREAM_BUFFERED: return "buffered";
    case AUDIO_STREAM_REALTIME: return "realtime";
    default: return "none";
  }
}

esp_err_t audio_receiver_init(void) {
  taskENTER_CRITICAL(&s_mux);
  s.initialized = true;
  taskEXIT_CRITICAL(&s_mux);
  ESP_LOGI(TAG, "Gateway proxy ready: no decoder, no I2S; audio_receiver "
                "calls are logged as FWD");
  return ESP_OK;
}

bool audio_receiver_is_initialized(void) { return s.initialized; }

bool audio_receiver_is_idle(void) {
  return !s.started && gw_relay_is_idle() && gw_rt_relay_is_idle();
}

esp_err_t audio_receiver_release_for_wifi_scan(void) {
  audio_receiver_stop();
  taskENTER_CRITICAL(&s_mux);
  s.initialized = false;
  taskEXIT_CRITICAL(&s_mux);
  return gw_relay_is_idle() && gw_rt_relay_is_idle() ? ESP_OK : ESP_ERR_TIMEOUT;
}

void audio_receiver_set_format(const audio_format_t *format) {
  if (!format) return;
  taskENTER_CRITICAL(&s_mux);
  s.format = *format;
  s.session = true;
  taskEXIT_CRITICAL(&s_mux);
  AP_LOGI(TAG, "FWD set_format %s %d Hz %d ch %d bit %d frames",
          format->codec, format->sample_rate, format->channels,
          format->bits_per_sample, format->frame_size);
  surround_format_t m = {
      .sample_rate = format->sample_rate,
      .channels = format->channels,
      .bits_per_sample = format->bits_per_sample,
      .frame_size = format->frame_size,
  };
  strlcpy(m.codec, format->codec, sizeof(m.codec));
  gw_speakers_send(SURROUND_MSG_SET_FORMAT, &m, sizeof(m));
}

uint32_t audio_receiver_get_sample_rate(void) {
  const int rate = s.format.sample_rate;
  return rate > 0 ? (uint32_t)rate : AUDIO_PIPELINE_RATE_HZ;
}

void audio_receiver_set_encryption(const audio_encrypt_t *encrypt) {
  audio_encrypt_t next = {0};
  if (encrypt) next = *encrypt;
  taskENTER_CRITICAL(&s_mux);
  s.encrypt = next;
  taskEXIT_CRITICAL(&s_mux);
  /* The key itself is never logged. */
  AP_LOGI(TAG, "FWD set_encryption type=%d key_len=%u", (int)next.type,
          (unsigned)next.key_len);
  surround_encryption_t m = {
      .type = (uint8_t)next.type,
      .key_len = (uint8_t)(next.key_len <= sizeof(m.key) ? next.key_len : 0U),
  };
  memcpy(m.key, next.key, sizeof(m.key));
  memcpy(m.iv, next.iv, sizeof(m.iv));
  gw_speakers_send(SURROUND_MSG_SET_ENCRYPTION, &m, sizeof(m));
}

void audio_receiver_set_stream_type(audio_stream_type_t type) {
  taskENTER_CRITICAL(&s_mux);
  s.stream_type = type;
  taskEXIT_CRITICAL(&s_mux);
  AP_LOGI(TAG, "FWD set_stream_type %s (%d)", stream_type_name(type), (int)type);
  const surround_stream_type_t m = {.stream_type = (uint8_t)type};
  gw_speakers_send(SURROUND_MSG_SET_STREAM_TYPE, &m, sizeof(m));
}

esp_err_t audio_receiver_start_buffered(uint16_t tcp_port) {
  const audio_format_t f = s.format;
  /* Accept and refuse the same formats as the speaker's receiver. */
  const int codec_type = strcmp(f.codec, "AAC") == 0 ? 4 :
      strcmp(f.codec, "ALAC") == 0 ? 2 : -1;
  if (!airplay_audio_format_resolve(AUDIO_STREAM_BUFFERED, 0, codec_type,
                                    f.sample_rate, f.frame_size,
                                    f.bits_per_sample, f.channels)) {
    ESP_LOGE(TAG,
             "Buffered format rejected codec=%s sr=%d ch=%d bits=%d frame=%d",
             f.codec, f.sample_rate, f.channels, f.bits_per_sample,
             f.frame_size);
    return ESP_ERR_NOT_SUPPORTED;
  }
  uint16_t bound = tcp_port;
  esp_err_t err = gw_relay_start(tcp_port, &bound);
  if (err != ESP_OK) return err;
  taskENTER_CRITICAL(&s_mux);
  const bool was_started = s.started;
  s.started = true;
  s.session = true;
  s.buffered_port = bound;
  taskEXIT_CRITICAL(&s_mux);
  if (!was_started) {
    AP_LOGI(TAG, "FWD start_buffered (Gateway listens on port %u)",
            (unsigned)bound);
    gw_speakers_send(SURROUND_MSG_START_BUFFERED, NULL, 0);
  }
  return ESP_OK;
}

esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port) {
  if (s.stream_type == AUDIO_STREAM_BUFFERED) {
    return audio_receiver_start_buffered(tcp_port);
  }
  if (s.stream_type == AUDIO_STREAM_REALTIME) {
    /* The speaker's realtime receiver takes exactly this format. */
    const audio_format_t f = s.format;
    if (strcmp(f.codec, "ALAC") != 0 || f.sample_rate != 44100 ||
        f.channels != 2 || f.bits_per_sample != 16 || f.frame_size != 352) {
      ESP_LOGE(TAG,
               "Realtime format rejected codec=%s sr=%d ch=%d bits=%d frame=%d",
               f.codec, f.sample_rate, f.channels, f.bits_per_sample,
               f.frame_size);
      return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = gw_rt_relay_start(data_port, control_port);
    if (err != ESP_OK) return err;
    taskENTER_CRITICAL(&s_mux);
    const bool was_started = s.started;
    s.started = true;
    s.session = true;
    taskEXIT_CRITICAL(&s_mux);
    if (!was_started) {
      AP_LOGI(TAG, "FWD start_stream realtime (Gateway data %u control %u)",
              (unsigned)data_port, (unsigned)control_port);
      gw_speakers_send(SURROUND_MSG_START_STREAM, NULL, 0);
    }
    return ESP_OK;
  }
  return ESP_ERR_NOT_SUPPORTED;
}

void audio_receiver_stop(void) {
  gw_relay_stop();
  gw_rt_relay_stop();
  taskENTER_CRITICAL(&s_mux);
  /* Also after a SETUP whose stream never started: the speakers got its
   * format, and a speaker joining later must not wait for a FLUSH of a
   * stream that does not exist. Repeated stops are not forwarded. */
  const bool had_session = s.session;
  s.started = false;
  s.session = false;
  s.playing = false;
  s.buffered_port = 0;
  taskEXIT_CRITICAL(&s_mux);
  gw_relay_set_playing(false);
  if (had_session) {
    AP_LOGI(TAG, "FWD stop");
    gw_speakers_send(SURROUND_MSG_STOP, NULL, 0);
  }
}

uint16_t audio_receiver_get_buffered_port(void) { return s.buffered_port; }

/* RTSP calls this inside a critical section (rtsp_conn.c volume_lock), where
 * a speaker only stores an atomic. So does the Gateway: no log, lock or
 * socket here; the speakers task forwards it (gw_speakers_post_volume). */
void audio_receiver_set_volume_q15(int32_t volume_q15) {
  if (volume_q15 < 0) volume_q15 = 0;
  if (volume_q15 > 32768) volume_q15 = 32768;
  __atomic_store_n(&s.volume_q15, volume_q15, __ATOMIC_RELEASE);
  gw_speakers_post_volume(volume_q15);
}

int32_t audio_receiver_get_volume_q15(void) {
  return __atomic_load_n(&s.volume_q15, __ATOMIC_ACQUIRE);
}

void audio_receiver_seek_flush(void) {
  AP_LOGI(TAG, "FWD seek_flush");
  gw_speakers_send(SURROUND_MSG_SEEK_FLUSH, NULL, 0);
}

void audio_receiver_realtime_flush_to_rtp(uint32_t flush_rtp) {
  AP_LOGI(TAG, "FWD realtime_flush_to_rtp rtp=%" PRIu32, flush_rtp);
  const surround_realtime_flush_t m = {.rtp = flush_rtp};
  gw_speakers_send(SURROUND_MSG_REALTIME_FLUSH_TO_RTP, &m, sizeof(m));
}

void audio_receiver_realtime_flush_wait_sender_anchor(void) {
  AP_LOGI(TAG, "FWD realtime_flush_wait_sender_anchor");
  gw_speakers_send(SURROUND_MSG_REALTIME_FLUSH_WAIT_ANCHOR, NULL, 0);
}

esp_err_t audio_receiver_set_deferred_flush_range(uint32_t from_seq,
                                                   uint32_t from_ts,
                                                   uint32_t until_seq,
                                                   uint32_t until_ts,
                                                   bool timestamps_valid) {
  AP_LOGI(TAG,
          "FWD set_deferred_flush_range from %" PRIu32 "/%" PRIu32
          " until %" PRIu32 "/%" PRIu32 " ts_valid=%d",
          from_seq, from_ts, until_seq, until_ts, timestamps_valid ? 1 : 0);
  const surround_deferred_flush_t m = {
      .from_seq = from_seq,
      .from_ts = from_ts,
      .until_seq = until_seq,
      .until_ts = until_ts,
      .timestamps_valid = timestamps_valid ? 1U : 0U,
  };
  gw_speakers_send(SURROUND_MSG_DEFERRED_FLUSH, &m, sizeof(m));
  return ESP_OK;
}

void audio_receiver_set_immediate_flush(uint32_t until_seq, uint32_t until_ts) {
  AP_LOGI(TAG, "FWD set_immediate_flush until %" PRIu32 "/%" PRIu32, until_seq,
          until_ts);
  const surround_immediate_flush_t m = {.until_seq = until_seq,
                                        .until_ts = until_ts};
  gw_speakers_send(SURROUND_MSG_IMMEDIATE_FLUSH, &m, sizeof(m));
}

void audio_receiver_pause(void) {
  taskENTER_CRITICAL(&s_mux);
  s.playing = false;
  taskEXIT_CRITICAL(&s_mux);
  gw_relay_set_playing(false);
  AP_LOGI(TAG, "FWD pause");
  gw_speakers_send(SURROUND_MSG_PAUSE, NULL, 0);
}

void audio_receiver_set_playing(bool playing) {
  taskENTER_CRITICAL(&s_mux);
  s.playing = playing;
  taskEXIT_CRITICAL(&s_mux);
  gw_relay_set_playing(playing);
  AP_LOGI(TAG, "FWD set_playing %d", playing ? 1 : 0);
  const surround_playing_t m = {.playing = playing ? 1U : 0U};
  gw_speakers_send(SURROUND_MSG_SET_PLAYING, &m, sizeof(m));
}

void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time) {
  ptp_clock_snapshot_t ps = {0};
  ptp_clock_get_snapshot(&ps);
  uint64_t local_ns = 0;
  if (ps.valid &&
      ptp_clock_engine_remote_to_local(network_time_ns, ps.filtered_offset_ns,
                                       &local_ns)) {
    const int64_t in_ms =
        ((int64_t)local_ns - (int64_t)esp_timer_get_time() * 1000LL) / 1000000LL;
    AP_LOGI(TAG,
            "FWD set_anchor_time clock=%016" PRIx64 " ptp=%" PRIu64
            " rtp=%" PRIu32 " (Gateway PTP locked=%d: anchor %+" PRId64
            " ms from now)",
            clock_id, network_time_ns, rtp_time, ps.locked ? 1 : 0, in_ms);
  } else {
    AP_LOGI(TAG,
            "FWD set_anchor_time clock=%016" PRIx64 " ptp=%" PRIu64
            " rtp=%" PRIu32 " (Gateway PTP not valid yet)",
            clock_id, network_time_ns, rtp_time);
  }
  const surround_anchor_t m = {.clock_id = clock_id,
                               .network_time_ns = network_time_ns,
                               .rtp_time = rtp_time};
  gw_speakers_send(SURROUND_MSG_SET_ANCHOR, &m, sizeof(m));
}

bool audio_receiver_set_realtime_anchor_local(
    uint64_t clock_id, uint32_t gm_epoch, uint32_t mastership_age_ms,
    uint64_t remote_ptp_ns, uint64_t candidate_local_ns, uint32_t rtp_time,
    audio_realtime_anchor_result_t *result) {
  /* RTSP converted the anchor with the Gateway's clock; each speaker
   * converts the sender time again with its own. */
  AP_LOGI(TAG,
          "FWD set_realtime_anchor_local clock=%016" PRIx64 " epoch=%" PRIu32
          " age=%" PRIu32 " ms ptp=%" PRIu64 " rtp=%" PRIu32,
          clock_id, gm_epoch, mastership_age_ms, remote_ptp_ns, rtp_time);
  const surround_realtime_anchor_t m = {.clock_id = clock_id,
                                        .remote_ptp_ns = remote_ptp_ns,
                                        .rtp_time = rtp_time};
  gw_speakers_send(SURROUND_MSG_REALTIME_ANCHOR, &m, sizeof(m));
  if (result) {
    memset(result, 0, sizeof(*result));
    result->effective_local_ns = candidate_local_ns;
  }
  return true;
}

void audio_receiver_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port) {
  /* The speakers send their NACKs to the Gateway, which passes them on. */
  gw_rt_relay_set_sender_control(client_ip, client_control_port);
  const surround_client_control_t m = {.port = gw_rt_relay_control_port()};
  AP_LOGI(TAG,
          "FWD set_client_control sender %u.%u.%u.%u:%u, speakers -> Gateway "
          "port %u",
          (unsigned)(client_ip & 0xffU), (unsigned)((client_ip >> 8) & 0xffU),
          (unsigned)((client_ip >> 16) & 0xffU),
          (unsigned)((client_ip >> 24) & 0xffU), (unsigned)client_control_port,
          (unsigned)m.port);
  if (m.port) gw_speakers_send(SURROUND_MSG_SET_CLIENT_CONTROL, &m, sizeof(m));
}

void audio_receiver_set_playout_latency_samples(uint32_t latency_samples) {
  AP_LOGI(TAG, "FWD set_playout_latency_samples %" PRIu32, latency_samples);
  const surround_playout_latency_t m = {.latency_samples = latency_samples};
  gw_speakers_send(SURROUND_MSG_SET_PLAYOUT_LATENCY, &m, sizeof(m));
}
