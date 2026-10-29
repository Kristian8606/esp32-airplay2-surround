#include "spk_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aac_decoder.h"
#include "airplay_log.h"
#include "amp_control.h"
#include "audio_receiver.h"
#include "cal_chirp.h"
#include "cal_playback.h"
#include "firmware_id.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "ptp_clock.h"
#include "rtsp_events.h"
#include "sdkconfig.h"
#include "settings.h"
#include "socket_utils.h"
#include "spk_time.h"
#include "surround_proto.h"

static const char *TAG = "spk_link";

/* Core 1 next to the buffered TCP reader and I2S playout; core 0 stays with
 * the FDK decoder. Above the reader (4), below playout (8). */
#define SPK_LINK_CORE        1
#define SPK_LINK_PRIORITY    7
#define SPK_LINK_STACK       6144U
#define SPK_LINK_RETRY_MS    2000U
#define SPK_LINK_RX_POLL_MS  500
#define SPK_MDNS_TIMEOUT_MS  3000U
/* A send to the Gateway that cannot complete in this time ends the link
 * (the Gateway stopped reading), instead of blocking the link task. */
#define SPK_LINK_TX_TIMEOUT_MS 2000

/* The key is copied between these two structs byte for byte. */
static_assert(sizeof(((audio_encrypt_t *)0)->key) ==
                  sizeof(((surround_encryption_t *)0)->key),
              "key size");
static_assert(sizeof(((audio_encrypt_t *)0)->iv) ==
                  sizeof(((surround_encryption_t *)0)->iv),
              "iv size");

static bool s_stream_started; /* START_* done, no STOP yet */
static uint32_t s_gateway_ip;  /* of the current link, network byte order */
/* Web page requests (other tasks): drop the link and stay away, or report
 * changed settings to the Gateway. */
static volatile bool s_suspend;
static volatile bool s_idle = true; /* no link and no stream */
static volatile bool s_config_dirty;
static volatile int s_link_fd = -1;
/* Tuning from the Gateway web page (settings_get_surround_tuning). The trim
 * scales the sender's volume here; audio and loudness see the product. */
static int32_t s_trim_db10;
static int32_t s_delay_us;
static int32_t s_sender_volume_q15 = 32768;

static bool send_all(int fd, const uint8_t *data, size_t len) {
  while (len > 0) {
    const int n = send(fd, data, len, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    data += n;
    len -= (size_t)n;
  }
  return true;
}

static bool send_msg(int fd, uint8_t type, const void *payload, size_t len) {
  uint8_t buf[3U + SURROUND_MAX_PAYLOAD];
  if (len > SURROUND_MAX_PAYLOAD) return false;
  const size_t body = 1U + len;
  buf[0] = (uint8_t)(body >> 8);
  buf[1] = (uint8_t)body;
  buf[2] = type;
  if (len) memcpy(buf + 3, payload, len);
  return send_all(fd, buf, 2U + body);
}

static bool first_ipv4(const mdns_ip_addr_t *a, uint32_t *ip) {
  for (; a; a = a->next) {
    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
      *ip = a->addr.u_addr.ip4.addr;
      return true;
    }
  }
  return false;
}

static bool find_gateway(struct sockaddr_in *out) {
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  const char *host = CONFIG_AIRPLAY_SURROUND_GATEWAY_HOST;
  if (host[0]) {
    if (inet_pton(AF_INET, host, &out->sin_addr) != 1) {
      ESP_LOGE(TAG, "Gateway address '%s' is not an IPv4 address", host);
      return false;
    }
    out->sin_port = htons(SURROUND_CONTROL_PORT);
    return true;
  }

  mdns_result_t *results = NULL;
  if (mdns_query_ptr(SURROUND_MDNS_SERVICE, SURROUND_MDNS_PROTO,
                     SPK_MDNS_TIMEOUT_MS, 4, &results) != ESP_OK) {
    return false;
  }
  bool found = false;
  for (mdns_result_t *r = results; r && !found; r = r->next) {
    uint32_t ip = 0;
    bool have_ip = first_ipv4(r->addr, &ip);
    if (!have_ip && r->hostname) {
      /* The A record did not come with the PTP answer: ask for it. */
      esp_ip4_addr_t a = {0};
      if (mdns_query_a(r->hostname, SPK_MDNS_TIMEOUT_MS, &a) == ESP_OK) {
        ip = a.addr;
        have_ip = true;
      }
    }
    if (have_ip && r->port) {
      out->sin_addr.s_addr = ip;
      out->sin_port = htons(r->port);
      found = true;
    }
  }
  mdns_query_results_free(results);
  return found;
}

static void send_hello(int fd) {
  surround_hello_t h = {
      .magic = SURROUND_HELLO_MAGIC,
      .version = SURROUND_PROTO_VERSION,
      .channel = (int8_t)aac_decoder_get_speaker(),
  };
  esp_read_mac(h.mac, ESP_MAC_WIFI_STA);
  char name[65] = {0};
  if (settings_get_device_name(name, sizeof(name)) == ESP_OK) {
    strlcpy(h.name, name, sizeof(h.name));
  }
  firmware_id(h.firmware, sizeof(h.firmware));
  (void)send_msg(fd, SURROUND_MSG_HELLO, &h, sizeof(h));
}

/* Which part of a stereo source this speaker plays, as an AV receiver does
 * it: the front pair plays left and right, the subwoofer the mix of both
 * (its own crossover keeps the bass), centre and surrounds are silent. A
 * "mono mix" speaker plays the mix. */
static void apply_stereo_route(void) {
  const int role = aac_decoder_get_speaker();
  audio_stereo_route_t route;
  switch (role) {
    case AAC_SPEAKER_L:
      route = AUDIO_STEREO_ROUTE_LEFT;
      break;
    case AAC_SPEAKER_R:
      route = AUDIO_STEREO_ROUTE_RIGHT;
      break;
    case AAC_SPEAKER_C:
    case AAC_SPEAKER_LS:
    case AAC_SPEAKER_RS:
    case AAC_SPEAKER_LRS:
    case AAC_SPEAKER_RRS:
      route = AUDIO_STEREO_ROUTE_MUTE;
      break;
    case AAC_SPEAKER_LFE:
    default:
      route = AUDIO_STEREO_ROUTE_MONO;
      break;
  }
  audio_receiver_set_stereo_route(route);
  static const char *const names[] = {"stereo", "left", "right", "mono mix",
                                      "silent"};
  AP_LOGI(TAG, "stereo sources: %s (role %s)", names[route],
          aac_speaker_name(role));
}

static void apply_volume(void) {
  const double gain = pow(10.0, (double)s_trim_db10 / 200.0);
  audio_receiver_set_volume_q15((int32_t)lround(s_sender_volume_q15 * gain));
}

static void send_config(int fd) {
  const surround_config_t c = {
      .channel = (int8_t)aac_decoder_get_speaker(),
      .trim_db10 = (int16_t)s_trim_db10,
      .delay_us = s_delay_us,
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
      .multichannel = 1,
#endif
  };
  (void)send_msg(fd, SURROUND_MSG_CONFIG, &c, sizeof(c));
}

static void set_config(int fd, const surround_set_config_t *m) {
  if (m->fields & SURROUND_SET_CHANNEL) {
    if (m->channel >= AAC_SPEAKER_MIX && m->channel < AAC_SPEAKER_COUNT) {
      aac_decoder_set_speaker(m->channel);
      (void)settings_set_mc_speaker(m->channel);
      apply_stereo_route();
      AP_LOGI(TAG, "CMD set_config channel %d (%s)", (int)m->channel,
              aac_speaker_name(m->channel));
    } else {
      AP_LOGW(TAG, "CMD set_config: channel %d out of range", (int)m->channel);
    }
  }
  bool tuning = false;
  if (m->fields & SURROUND_SET_TRIM) {
    if (m->trim_db10 >= SURROUND_TRIM_MIN_DB10 &&
        m->trim_db10 <= SURROUND_TRIM_MAX_DB10) {
      s_trim_db10 = m->trim_db10;
      apply_volume();
      tuning = true;
      AP_LOGI(TAG, "CMD set_config trim %.1f dB", s_trim_db10 / 10.0);
    } else {
      AP_LOGW(TAG, "CMD set_config: trim %d/10 dB out of range",
              (int)m->trim_db10);
    }
  }
  if (m->fields & SURROUND_SET_DELAY) {
    if (m->delay_us >= SURROUND_DELAY_MIN_US &&
        m->delay_us <= SURROUND_DELAY_MAX_US &&
        audio_receiver_set_extra_delay_us(m->delay_us) == ESP_OK) {
      s_delay_us = m->delay_us;
      tuning = true;
      AP_LOGI(TAG, "CMD set_config delay %" PRId32 " us", s_delay_us);
    } else {
      AP_LOGW(TAG, "CMD set_config: delay %" PRId32 " us out of range",
              m->delay_us);
    }
  }
  if (tuning) (void)settings_set_surround_tuning(s_trim_db10, s_delay_us);
  send_config(fd);
}

/* Two free UDP ports for the realtime receiver, which binds exactly the
 * ports it is given (as RTSP SETUP does it). */
static bool pick_udp_ports(uint16_t *data_port, uint16_t *control_port) {
  const int a = socket_utils_bind_udp(0, 0, 0, data_port);
  const int b = socket_utils_bind_udp(0, 0, 0, control_port);
  if (a >= 0) close(a);
  if (b >= 0) close(b);
  return a >= 0 && b >= 0 && *data_port && *control_port &&
         *data_port != *control_port;
}

static void send_status(int fd) {
  audio_receiver_playback_info_t pi;
  audio_receiver_get_playback_info(&pi);
  surround_status_t st = {
      .stream_type = (uint8_t)pi.stream_type,
      .sample_rate = pi.format.sample_rate > 0 ? (uint32_t)pi.format.sample_rate : 0U,
      .bits = (uint8_t)pi.format.bits_per_sample,
      .stream_channels = (uint8_t)pi.format.channels,
      .output_channel = -1,
      .sync_us = pi.sync_us,
      .servo_ppm = (int16_t)pi.servo_ppm,
      .fifo_used_kib = (uint16_t)(pi.fifo_used_kib > 0xffffU ? 0xffffU : pi.fifo_used_kib),
      .fifo_capacity_kib =
          (uint16_t)(pi.fifo_capacity_kib > 0xffffU ? 0xffffU : pi.fifo_capacity_kib),
  };
  strlcpy(st.codec, pi.format.codec, sizeof(st.codec));
  if (pi.active) st.flags |= SURROUND_STATUS_ACTIVE;
  if (pi.playing) st.flags |= SURROUND_STATUS_PLAYING;
  if (pi.sync_valid) st.flags |= SURROUND_STATUS_SYNC;
  /* The SETUP format says 2 channels for 5.1/7.1 too; the decoder knows. */
  int channel = -1, channels = 0;
  if (pi.active && aac_decoder_get_mc_status(&channel, &channels, st.label,
                                             sizeof(st.label))) {
    st.flags |= SURROUND_STATUS_MC;
    st.output_channel = (int8_t)channel;
    if (channels > 0) st.stream_channels = (uint8_t)channels;
  }
  uint32_t rms_ns = 0;
  if (spk_time_status(&rms_ns)) st.flags |= SURROUND_STATUS_TIME;
  st.tsf_rms_ns10 = (uint16_t)(rms_ns / 10U > 0xffffU ? 0xffffU : rms_ns / 10U);
  (void)send_msg(fd, SURROUND_MSG_STATUS, &st, sizeof(st));
}

static void stream_ended(void) {
  if (!s_stream_started) return;
  s_stream_started = false;
  amp_control_session_disconnected();
}

/* One forwarded call: the same audio_receiver call RTSP made on the Gateway,
 * plus the amplifier and LED events RTSP would raise around it. */
static void dispatch(int fd, uint8_t type, const uint8_t *p, size_t len) {
#define PAYLOAD(T, v)                                                    \
  T v;                                                                   \
  if (len != sizeof(v)) {                                                \
    AP_LOGW(TAG, "message 0x%02x: %u bytes, expected %u", type,          \
            (unsigned)len, (unsigned)sizeof(v));                         \
    return;                                                              \
  }                                                                      \
  memcpy(&v, p, sizeof(v))

  switch (type) {
    case SURROUND_MSG_PING:
      (void)send_msg(fd, SURROUND_MSG_PONG, NULL, 0);
      break;
    case SURROUND_MSG_TIME: {
      PAYLOAD(surround_time_t, m);
      spk_time_handle(&m); /* 8 per second: not logged */
      break;
    }
    case SURROUND_MSG_SET_CONFIG: {
      PAYLOAD(surround_set_config_t, m);
      set_config(fd, &m);
      break;
    }
    case SURROUND_MSG_SET_STREAM_TYPE: {
      PAYLOAD(surround_stream_type_t, m);
      AP_LOGI(TAG, "CMD set_stream_type %u", (unsigned)m.stream_type);
      audio_receiver_set_stream_type((audio_stream_type_t)m.stream_type);
      break;
    }
    case SURROUND_MSG_SET_FORMAT: {
      PAYLOAD(surround_format_t, m);
      audio_format_t f = {
          .sample_rate = m.sample_rate,
          .channels = m.channels,
          .bits_per_sample = m.bits_per_sample,
          .frame_size = m.frame_size,
      };
      m.codec[sizeof(m.codec) - 1] = '\0';
      strlcpy(f.codec, m.codec, sizeof(f.codec));
      AP_LOGI(TAG, "CMD set_format %s %d Hz %d ch %d bit %d frames", f.codec,
              f.sample_rate, f.channels, f.bits_per_sample, f.frame_size);
      audio_receiver_set_format(&f);
      break;
    }
    case SURROUND_MSG_SET_ENCRYPTION: {
      PAYLOAD(surround_encryption_t, m);
      audio_encrypt_t e = {
          .type = (audio_encrypt_type_t)m.type,
          .key_len = m.key_len <= sizeof(e.key) ? m.key_len : 0U,
      };
      memcpy(e.key, m.key, sizeof(e.key));
      memcpy(e.iv, m.iv, sizeof(e.iv));
      AP_LOGI(TAG, "CMD set_encryption type=%u key_len=%u", (unsigned)m.type,
              (unsigned)e.key_len);
      audio_receiver_set_encryption(&e);
      break;
    }
    case SURROUND_MSG_START_BUFFERED: {
      const esp_err_t err = audio_receiver_start_buffered(0);
      const surround_start_result_t r = {
          .err = err,
          .port = err == ESP_OK ? audio_receiver_get_buffered_port() : 0,
      };
      AP_LOGI(TAG, "CMD start_buffered: %s, audio port %u",
              esp_err_to_name(err), (unsigned)r.port);
      if (err == ESP_OK && !s_stream_started) {
        s_stream_started = true;
        amp_control_session_connected();
      }
      (void)send_msg(fd, SURROUND_MSG_START_RESULT, &r, sizeof(r));
      break;
    }
    case SURROUND_MSG_START_STREAM: {
      surround_start_result_t r = {.err = ESP_FAIL};
      uint16_t data_port = 0, control_port = 0;
      if (pick_udp_ports(&data_port, &control_port)) {
        r.err = audio_receiver_start_stream(data_port, control_port, 0);
        if (r.err == ESP_OK) {
          r.rt_data_port = data_port;
          r.rt_control_port = control_port;
        }
      }
      AP_LOGI(TAG, "CMD start_stream realtime: %s, ports %u/%u",
              esp_err_to_name(r.err), (unsigned)r.rt_data_port,
              (unsigned)r.rt_control_port);
      if (r.err == ESP_OK && !s_stream_started) {
        s_stream_started = true;
        amp_control_session_connected();
      }
      (void)send_msg(fd, SURROUND_MSG_START_RESULT, &r, sizeof(r));
      break;
    }
    case SURROUND_MSG_SET_CLIENT_CONTROL: {
      PAYLOAD(surround_client_control_t, m);
      AP_LOGI(TAG, "CMD set_client_control: NACKs to the Gateway port %u",
              (unsigned)m.port);
      audio_receiver_set_client_control(s_gateway_ip, m.port);
      break;
    }
    case SURROUND_MSG_REALTIME_FLUSH_TO_RTP: {
      PAYLOAD(surround_realtime_flush_t, m);
      AP_LOGI(TAG, "CMD realtime_flush_to_rtp %" PRIu32, m.rtp);
      audio_receiver_realtime_flush_to_rtp(m.rtp);
      break;
    }
    case SURROUND_MSG_REALTIME_FLUSH_WAIT_ANCHOR:
      AP_LOGI(TAG, "CMD realtime_flush_wait_sender_anchor");
      audio_receiver_realtime_flush_wait_sender_anchor();
      break;
    case SURROUND_MSG_REALTIME_ANCHOR: {
      PAYLOAD(surround_realtime_anchor_t, m);
      /* Same conversion RTSP on the Gateway makes for SETRATEANCHORTIME,
       * with this speaker's clock. */
      ptp_realtime_snapshot_t ps = {0};
      ptp_clock_get_realtime_snapshot(&ps);
      uint64_t local_ns = 0;
      if (ptp_clock_realtime_snapshot_to_local(&ps, m.clock_id, m.remote_ptp_ns,
                                               &local_ns)) {
        audio_realtime_anchor_result_t res = {0};
        const bool published = audio_receiver_set_realtime_anchor_local(
            m.clock_id, ps.gm_change_count, ps.mastership_age_ms,
            m.remote_ptp_ns, local_ns, m.rtp_time, &res);
        AP_LOGI(TAG, "CMD realtime anchor clock=%016" PRIx64 " rtp=%" PRIu32
                     ": %s",
                m.clock_id, m.rtp_time, published ? "published" : "deferred");
      } else {
        AP_LOGI(TAG, "CMD realtime anchor clock=%016" PRIx64 " rtp=%" PRIu32
                     ": no realtime sender time yet (ready=%d gm=%016" PRIx64 ")",
                m.clock_id, m.rtp_time, ps.master_ready, ps.master_clock_id);
      }
      break;
    }
    case SURROUND_MSG_CAL_PLAY: {
      PAYLOAD(surround_cal_play_t, m);
      surround_cal_result_t r = {.seq = m.seq, .err = ESP_ERR_INVALID_STATE};
      int bursts = m.bursts;
      if (bursts < 1) bursts = 1;
      if (bursts > SURROUND_CAL_BURSTS_MAX) bursts = SURROUND_CAL_BURSTS_MAX;
      AP_LOGI(TAG, "CMD cal_play %s, %d dBFS, %d chirps",
              m.kind == CAL_CHIRP_LOW ? "low" : "wide", (int)m.level_db, bursts);
      /* The link task plays them itself (a few seconds): forwarded calls
       * and TIME wait in the socket meanwhile. Never during a stream. */
      if (!s_stream_started) {
        int64_t emit_us[SURROUND_CAL_BURSTS_MAX] = {0};
        int n = 0;
        esp_err_t err = audio_receiver_play_cal_chirps(m.kind, m.level_db,
                                                       bursts, emit_us, &n);
        /* The sound leaves the speaker the output latency later. */
        const int64_t out_ns =
            (int64_t)audio_receiver_get_output_latency_us() * 1000LL;
        for (int i = 0; err == ESP_OK && i < n; ++i) {
          int64_t tsf = 0;
          if (!spk_time_tsf_at(emit_us[i] * 1000LL + out_ns, &tsf)) {
            err = ESP_ERR_INVALID_STATE; /* no TSF fit yet */
            break;
          }
          r.emit_tsf_ns[i] = tsf;
        }
        r.err = (int16_t)err;
        r.n = err == ESP_OK ? (uint8_t)n : 0U;
      }
      (void)send_msg(fd, SURROUND_MSG_CAL_RESULT, &r, sizeof(r));
      break;
    }
    case SURROUND_MSG_STOP:
      AP_LOGI(TAG, "CMD stop");
      audio_receiver_stop();
      stream_ended();
      break;
    case SURROUND_MSG_SET_VOLUME: {
      PAYLOAD(surround_volume_t, m);
      AP_LOGI(TAG, "CMD set_volume_q15 %" PRId32 " (trim %.1f dB)",
              m.volume_q15, s_trim_db10 / 10.0);
      /* The same range audio_receiver_set_volume_q15() accepts. */
      s_sender_volume_q15 = m.volume_q15 < 0 ? 0
                            : m.volume_q15 > 32768 ? 32768 : m.volume_q15;
      apply_volume();
      break;
    }
    case SURROUND_MSG_SEEK_FLUSH:
      AP_LOGI(TAG, "CMD seek_flush");
      audio_receiver_seek_flush();
      break;
    case SURROUND_MSG_DEFERRED_FLUSH: {
      PAYLOAD(surround_deferred_flush_t, m);
      AP_LOGI(TAG,
              "CMD set_deferred_flush_range from %" PRIu32 "/%" PRIu32
              " until %" PRIu32 "/%" PRIu32 " ts_valid=%u",
              m.from_seq, m.from_ts, m.until_seq, m.until_ts,
              (unsigned)m.timestamps_valid);
      (void)audio_receiver_set_deferred_flush_range(
          m.from_seq, m.from_ts, m.until_seq, m.until_ts,
          m.timestamps_valid != 0U);
      break;
    }
    case SURROUND_MSG_IMMEDIATE_FLUSH: {
      PAYLOAD(surround_immediate_flush_t, m);
      AP_LOGI(TAG, "CMD set_immediate_flush until %" PRIu32 "/%" PRIu32,
              m.until_seq, m.until_ts);
      audio_receiver_set_immediate_flush(m.until_seq, m.until_ts);
      break;
    }
    case SURROUND_MSG_PAUSE:
      AP_LOGI(TAG, "CMD pause");
      audio_receiver_pause();
      rtsp_events_emit(RTSP_EVENT_PAUSED);
      break;
    case SURROUND_MSG_SET_PLAYING: {
      PAYLOAD(surround_playing_t, m);
      AP_LOGI(TAG, "CMD set_playing %u", (unsigned)m.playing);
      audio_receiver_set_playing(m.playing != 0U);
      rtsp_events_emit(m.playing ? RTSP_EVENT_PLAYING : RTSP_EVENT_PAUSED);
      break;
    }
    case SURROUND_MSG_SET_ANCHOR: {
      PAYLOAD(surround_anchor_t, m);
      AP_LOGI(TAG, "CMD set_anchor_time clock=%016" PRIx64 " ptp=%" PRIu64
                   " rtp=%" PRIu32,
              m.clock_id, m.network_time_ns, m.rtp_time);
      audio_receiver_set_anchor_time(m.clock_id, m.network_time_ns,
                                     m.rtp_time);
      break;
    }
    case SURROUND_MSG_SET_PLAYOUT_LATENCY: {
      PAYLOAD(surround_playout_latency_t, m);
      AP_LOGI(TAG, "CMD set_playout_latency_samples %" PRIu32,
              m.latency_samples);
      audio_receiver_set_playout_latency_samples(m.latency_samples);
      break;
    }
    default:
      AP_LOGW(TAG, "unknown message 0x%02x (%u bytes)", type, (unsigned)len);
      break;
  }
#undef PAYLOAD
}

/* Runs one connected link until it drops. */
static void run_link(int fd) {
  uint8_t rx[3U + SURROUND_MAX_PAYLOAD];
  size_t have = 0;
  int64_t last_rx_us = esp_timer_get_time();
  int64_t last_status_us = 0;
  for (;;) {
    const int r = recv(fd, rx + have, sizeof(rx) - have, 0);
    const int64_t now_us = esp_timer_get_time();
    if (r == 0) {
      ESP_LOGW(TAG, "Gateway closed the link");
      return;
    }
    if (r < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        ESP_LOGW(TAG, "link recv errno=%d", errno);
        return;
      }
      if (now_us - last_rx_us > (int64_t)SURROUND_LINK_TIMEOUT_MS * 1000LL) {
        ESP_LOGW(TAG, "no PING from the Gateway for %u ms",
                 (unsigned)SURROUND_LINK_TIMEOUT_MS);
        return;
      }
      continue;
    }
    last_rx_us = now_us;
    if (now_us - last_status_us >= (int64_t)SURROUND_STATUS_MS * 1000LL) {
      last_status_us = now_us;
      send_status(fd);
    }
    if (s_config_dirty) {
      s_config_dirty = false;
      apply_stereo_route(); /* the role may have changed */
      send_config(fd);
    }
    have += (size_t)r;
    while (have >= 2U) {
      const size_t body = ((size_t)rx[0] << 8) | rx[1];
      if (body < 1U || body > 1U + SURROUND_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "invalid frame length %u from the Gateway",
                 (unsigned)body);
        return;
      }
      if (have < 2U + body) break;
      dispatch(fd, rx[2], rx + 3, body - 1U);
      have -= 2U + body;
      memmove(rx, rx + 2U + body, have);
    }
  }
}

static void link_task(void *arg) {
  (void)arg;
  for (;;) {
    if (s_suspend) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    struct sockaddr_in gw;
    if (!find_gateway(&gw)) {
      AP_LOGI(TAG, "no Gateway found (%s.%s); retrying", SURROUND_MDNS_SERVICE,
              SURROUND_MDNS_PROTO);
      vTaskDelay(pdMS_TO_TICKS(SPK_LINK_RETRY_MS));
      continue;
    }
    char ip[16];
    inet_ntoa_r(gw.sin_addr, ip, sizeof(ip));
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
      ESP_LOGE(TAG, "socket errno=%d", errno);
      vTaskDelay(pdMS_TO_TICKS(SPK_LINK_RETRY_MS));
      continue;
    }
    if (connect(fd, (struct sockaddr *)&gw, sizeof(gw)) != 0) {
      AP_LOGW(TAG, "connect to Gateway %s:%u failed errno=%d", ip,
              (unsigned)ntohs(gw.sin_port), errno);
      close(fd);
      vTaskDelay(pdMS_TO_TICKS(SPK_LINK_RETRY_MS));
      continue;
    }
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const struct timeval tv = {.tv_sec = 0,
                               .tv_usec = SPK_LINK_RX_POLL_MS * 1000};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const struct timeval tx_tv = {.tv_sec = SPK_LINK_TX_TIMEOUT_MS / 1000,
                                  .tv_usec = (SPK_LINK_TX_TIMEOUT_MS % 1000) * 1000};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tx_tv, sizeof(tx_tv));

    ESP_LOGI(TAG, "connected to Gateway %s:%u", ip,
             (unsigned)ntohs(gw.sin_port));
    s_gateway_ip = gw.sin_addr.s_addr;
    s_idle = false;
    s_link_fd = fd;
    if (s_suspend) shutdown(fd, SHUT_RDWR); /* raced with spk_link_suspend() */
    send_hello(fd);
    send_config(fd);
    rtsp_events_emit(RTSP_EVENT_CLIENT_CONNECTED);
    run_link(fd);
    s_link_fd = -1;
    close(fd);
    spk_time_reset();

    /* Without the Gateway the stream cannot go on: stop it, so the next
     * link starts from a clean receiver like a new AirPlay session. */
    if (s_stream_started) {
      AP_LOGI(TAG, "link lost during a stream: stopping it");
      audio_receiver_set_playing(false);
      audio_receiver_stop();
      stream_ended();
    }
    s_idle = true;
    rtsp_events_emit(RTSP_EVENT_DISCONNECTED);
    vTaskDelay(pdMS_TO_TICKS(SPK_LINK_RETRY_MS));
  }
}

void spk_link_suspend(void) {
  s_suspend = true;
  const int fd = s_link_fd;
  if (fd >= 0) shutdown(fd, SHUT_RDWR); /* run_link returns at once */
  /* The link task stops the stream when the link ends; wait for that. */
  for (int i = 0; !s_idle && i < 300; ++i) vTaskDelay(pdMS_TO_TICKS(10));
  if (!s_idle) ESP_LOGW(TAG, "link task did not settle within 3 s");
}

void spk_link_resume(void) {
  s_suspend = false;
}

void spk_link_config_changed(void) { s_config_dirty = true; }

esp_err_t spk_link_start(void) {
  int32_t trim = 0, delay = 0;
  (void)settings_get_surround_tuning(&trim, &delay);
  if (trim >= SURROUND_TRIM_MIN_DB10 && trim <= SURROUND_TRIM_MAX_DB10)
    s_trim_db10 = trim;
  if (delay >= SURROUND_DELAY_MIN_US && delay <= SURROUND_DELAY_MAX_US &&
      audio_receiver_set_extra_delay_us(delay) == ESP_OK)
    s_delay_us = delay;
  apply_volume();
  apply_stereo_route();
  ESP_LOGI(TAG, "Surround tuning: trim %.1f dB, delay %" PRId32 " us",
           s_trim_db10 / 10.0, s_delay_us);

  /* mDNS is only a client here: a speaker is not an AirPlay receiver and is
   * not advertised to AirPlay senders. */
  esp_err_t err = mdns_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
  }
  if (xTaskCreatePinnedToCore(link_task, "spk_link", SPK_LINK_STACK, NULL,
                              SPK_LINK_PRIORITY, NULL,
                              SPK_LINK_CORE) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "looking for the Gateway");
  return ESP_OK;
}
