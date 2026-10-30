#include "rtsp_handlers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sodium.h"

#include "audio_receiver.h"
#include "airplay_audio_formats.h"
#include "audio_diag.h"
#include "amp_control.h"
#include "hap.h"
#include "ptp_clock.h"
#include "plist.h"
#include "rtsp_fairplay.h"
#include "settings.h"
#include "socket_utils.h"
#include "tlv8.h"

#include "rtsp_events.h"
#include "airplay_log.h"

static const char *TAG = "rtsp_handlers";

static void amp_session_activate_once(rtsp_conn_t *conn) {
  if (!conn || conn->amp_session_active) return;
  conn->amp_session_active = true;
  amp_control_session_connected();
}

static void amp_session_deactivate_once(rtsp_conn_t *conn) {
  if (!conn || !conn->amp_session_active) return;
  conn->amp_session_active = false;
  amp_control_session_disconnected();
}

// Default playout latency for realtime (type 96) streams when SETUP does not
// carry a usable latencyMin: 11025 samples = 250 ms at 44.1 kHz.  This is the
// standard AirPlay realtime-stream minimum latency; senders transmit audio
// ~2 s (88200 samples) ahead of this deadline.
#define AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES 11025
static void rtsp_get_device_id(char *device_id, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(device_id, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

static int rtsp_create_udp_socket(uint16_t *port) {
  uint16_t bound_port = 0;
  int sock = socket_utils_bind_udp(0, 0, 0, &bound_port);
  if (sock < 0) {
    return -1;
  }
  if (port) {
    *port = bound_port;
  }
  return sock;
}

static int rtsp_create_event_socket(uint16_t *port) {
  uint16_t bound_port = 0;
  int sock = socket_utils_bind_tcp_listener(0, 1, false, &bound_port);
  if (sock < 0) {
    return -1;
  }
  if (port) {
    *port = bound_port;
  }
  return sock;
}

/* Realtime (type 96) only: reserve free UDP data/control port numbers for the
 * SETUP response. The realtime receiver binds them when it starts. */
static void ensure_realtime_ports(rtsp_conn_t *conn) {
  int temp_socket;
  if (conn->data_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->data_port);
    if (temp_socket >= 0) {
      close(temp_socket);
    }
  }
  if (conn->control_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->control_port);
    if (temp_socket >= 0) {
      close(temp_socket);
    }
  }
}

static bool start_audio_receiver_or_fail(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         int64_t stream_type) {
  audio_receiver_set_stream_type((audio_stream_type_t)stream_type);
  esp_err_t err = audio_receiver_start_stream(
      conn->data_port, conn->control_port, conn->buffered_port);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start audio receiver: %s", esp_err_to_name(err));
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return false;
  }
  return true;
}

// Forward declarations of handlers
static void handle_options(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req, const uint8_t *raw,
                           size_t raw_len);
static void handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len);
static void handle_post(int socket, rtsp_conn_t *conn,
                        const rtsp_request_t *req, const uint8_t *raw,
                        size_t raw_len);
static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_record(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len);
static void handle_set_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static void handle_get_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static void handle_pause(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_flush(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_flushbuffered(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static void handle_teardown(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len);
static void handle_setrateanchortime(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req,
                                     const uint8_t *raw, size_t raw_len);
static void handle_setpeers(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len);
static void handle_loudnessnormalization(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         const uint8_t *raw, size_t raw_len);

// Dispatch table
static const rtsp_method_handler_t method_handlers[] = {
    {"OPTIONS", handle_options},
    {"GET", handle_get},
    {"POST", handle_post},
    {"SETUP", handle_setup},
    {"RECORD", handle_record},
    {"SET_PARAMETER", handle_set_parameter},
    {"GET_PARAMETER", handle_get_parameter},
    {"PAUSE", handle_pause},
    {"FLUSH", handle_flush},
    {"FLUSHBUFFERED", handle_flushbuffered},
    {"TEARDOWN", handle_teardown},
    {"SETRATEANCHORTIME", handle_setrateanchortime},
    {"SETPEERS", handle_setpeers},
    {"SETPEERSX", handle_setpeers},
    {"LOUDNESSNORMALIZATION", handle_loudnessnormalization},
    {NULL, NULL}};

// Parse a named header value from raw RTSP request data (case-insensitive).
// Copies the trimmed value into the caller's buffer (two RTSP client tasks may
// parse concurrently, so no shared static storage) and returns it, or NULL.
static const char *parse_raw_header(const uint8_t *raw, size_t raw_len,
                                    const char *name, char *value_buf,
                                    size_t value_cap) {
  if (!raw || !name || !value_buf || value_cap == 0) {
    return NULL;
  }

  const uint8_t *header_end = rtsp_find_header_end(raw, raw_len);
  if (!header_end) {
    return NULL;
  }

  const char *line = (const char *)raw;
  const char *end = (const char *)header_end;
  size_t name_len = strlen(name);

  while (line < end) {
    const char *line_end = line;
    while (line_end < end && *line_end != '\r' && *line_end != '\n') {
      line_end++;
    }

    if ((size_t)(line_end - line) >= name_len &&
        strncasecmp(line, name, name_len) == 0) {
      const char *hdr = line + name_len;
      // Skip optional whitespace
      while (hdr < line_end && (*hdr == ' ' || *hdr == '\t')) {
        hdr++;
      }

      size_t i = 0;
      while (i < value_cap - 1 && hdr < line_end) {
        value_buf[i] = *hdr;
        hdr++;
        i++;
      }
      value_buf[i] = '\0';
      return value_buf;
    }

    line = line_end;
    while (line < end && (*line == '\r' || *line == '\n')) {
      line++;
    }
  }

  return NULL;
}

static bool parse_rtp_info_rtptime(const uint8_t *raw, size_t raw_len,
                                    uint32_t *rtptime) {
  if (!rtptime) return false;
  char info_buf[64];
  const char *info = parse_raw_header(raw, raw_len, "RTP-Info:", info_buf,
                                      sizeof(info_buf));
  if (!info) return false;
  const char *p = strcasestr(info, "rtptime=");
  if (!p) return false;
  p += 8;
  char *end = NULL;
  unsigned long value = strtoul(p, &end, 10);
  if (end == p) return false;
  *rtptime = (uint32_t)value;
  return true;
}

static bool request_uses_rtsp(const rtsp_request_t *req) {
  return req && strncasecmp(req->protocol, "RTSP/", 5) == 0;
}


/* Apply the phase-1 state a non-owner accepted while another connection owned
 * the session. Called once this connection has taken the play lock. */
static bool session_apply_deferred(rtsp_conn_t *conn) {
  if (conn->pending_encryption) {
    audio_receiver_set_encryption(&conn->setup_encryption);
    conn->pending_encryption = false;
  }
  if (conn->event_socket >= 0) {
    if (rtsp_start_event_port_task(conn->event_socket, conn) != ESP_OK) {
      close(conn->event_socket);
      conn->event_socket = -1;
      conn->event_port = 0;
      return false;
    }
    AP_LOGI(TAG, "SETUP: event port %u started after taking the play lock",
             conn->event_port);
    conn->event_socket = -1; /* Ownership transferred to event task. */
  }
  return true;
}

/* Methods that act on the global audio / PTP state. Only the connection that
 * holds the play lock may run them; one that merely probes the device gets a
 * plain OK and changes nothing (Shairport handles each connection separately). */
static bool method_needs_session(const char *m) {
  static const char *const names[] = {"RECORD", "SET_PARAMETER", "PAUSE", "FLUSH",
                                      "FLUSHBUFFERED", "TEARDOWN",
                                      "SETRATEANCHORTIME", NULL};
  for (int i = 0; names[i]; i++)
    if (strcasecmp(m, names[i]) == 0) return true;
  return false;
}

#if CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
/* One line per request: who sent it, what it is, and the readable strings of
 * a binary-plist body (key names such as rate / networkTimeSecs / flushFrom). */
__attribute__((noinline)) static void log_rtsp_request(int socket, const rtsp_conn_t *conn,
                             const rtsp_request_t *req) {
  char text[200];
  size_t n = 0;
  const uint8_t *b = req->body;
  const size_t len = req->body_len;
  if (b && len >= 8 && memcmp(b, "bplist00", 8) == 0) {
    size_t run = 0;
    for (size_t i = 8; i <= len && n + 2 < sizeof(text); i++) {
      const bool pr = i < len && b[i] >= 0x20 && b[i] < 0x7f;
      if (pr) {
        run++;
      } else {
        if (run >= 4) {
          size_t start = i - run;
          for (size_t k = start; k < i && n + 2 < sizeof(text); k++)
            text[n++] = (char)b[k];
          text[n++] = ' ';
        }
        run = 0;
      }
    }
  } else if (b && len) {
    for (size_t i = 0; i < len && n + 1 < sizeof(text); i++) {
      const uint8_t c = b[i];
      text[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : ((c == '\n' || c == '\r') ? ' ' : '.');
    }
  }
  text[n] = '\0';
  ESP_LOGI(TAG, "RTSP REQ c=%d %s %s %s cseq=%d ct=\"%s\" body=%zu lock=%d [%s]",
           socket, req->method, req->path, req->protocol, req->cseq,
           req->content_type, len, conn ? (int)conn->owns_session : -1, text);
}
#endif

int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len) {
  rtsp_request_t req;
  if (rtsp_request_parse(raw_request, raw_len, &req) < 0) {
    AP_LOGW(TAG, "Failed to parse RTSP request");
    return -1;
  }

#if CONFIG_AIRPLAY_LOG_RTSP_REQUESTS
  log_rtsp_request(socket, conn, &req);
#endif

  /* Only the SETUP of a connection takes the play lock (Shairport). These
   * methods never take it on their own: a connection that only probes the
   * device must not become the owner (and take over the volume) because it
   * sent a volume or FLUSH while nobody else happened to be playing. */
  if (method_needs_session(req.method) && !conn->owns_session) {
    AP_LOGI(TAG, "%s from a connection without the play lock: ignored",
             req.method);
    if (strcasecmp(req.method, "TEARDOWN") == 0) {
      /* Nothing of this connection's is playing; let it go. */
      conn->close_after_response = true;
    }
    rtsp_send_ok(socket, conn, req.cseq);
    return 0;
  }

  // Find handler in dispatch table
  for (const rtsp_method_handler_t *h = method_handlers; h->method; h++) {
    if (strcasecmp(req.method, h->method) == 0) {
      h->handler(socket, conn, &req, raw_request, raw_len);
      return 0;
    }
  }

  AP_LOGW(TAG, "Unknown method: %s", req.method);
  if (request_uses_rtsp(&req)) {
    rtsp_send_response(socket, conn, 501, "Not Implemented", req.cseq,
                       "Content-Type: text/plain\r\n", "Not Implemented", 15);
  } else {
    rtsp_send_http_response(socket, conn, 501, "Not Implemented", "text/plain",
                            "Not Implemented", 15);
  }
  return 0;
}

// ============================================================================
// Handler implementations
// ============================================================================

static void handle_options(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req, const uint8_t *raw,
                           size_t raw_len) {
  (void)raw;
  (void)raw_len;
  static const char public_methods[] =
      "Public: SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, "
      "OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, "
      "SETPEERSX, SETRATEANCHORTIME, LOUDNESSNORMALIZATION\r\n";
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, public_methods, NULL,
                     0);
}

static void handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (strcmp(req->path, "/info") == 0) {
    // Build info response
    char device_id[18];
    char device_name[65];

    rtsp_get_device_id(device_id, sizeof(device_id));
    settings_get_device_name(device_name, sizeof(device_name));
    const uint8_t *pk = hap_get_public_key();
    uint64_t features =
        ((uint64_t)AIRPLAY_FEATURES_HI << 32) | AIRPLAY_FEATURES_LO;

    /* The reply buffers are only needed while answering: allocate them per
     * request and free them right after sending. */
    if (request_uses_rtsp(req)) {
      const size_t body_cap = 1024;
      uint8_t *body = malloc(body_cap);
      size_t body_len =
          body ? bplist_build_info_response(body, body_cap, device_id,
                                            device_name, pk, 32, features,
                                            rtsp_conn_get_volume_db(conn))
               : 0;
      if (body_len == 0) {
        ESP_LOGE(TAG, "Failed to build binary /info response");
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        free(body);
        return;
      }
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/x-apple-binary-plist\r\n",
                         (const char *)body, body_len);
      free(body);
      return;
    }

    const size_t body_cap = 4096;
    char *body = malloc(body_cap);
    if (!body) {
      rtsp_send_http_response(socket, conn, 500, "Internal Error",
                              "text/plain", "Out of memory", 13);
      return;
    }
    plist_t p;
    char pairing_id[AIRPLAY_PAIRING_ID_LEN];
    airplay_get_pairing_id(pairing_id, sizeof(pairing_id));

    plist_init(&p, body, body_cap);
    plist_begin(&p);
    plist_dict_begin(&p);

    plist_dict_string(&p, "deviceid", device_id);
    plist_dict_uint(&p, "features", features);
    plist_dict_string(&p, "model", AIRPLAY_MODEL);
    plist_dict_string(&p, "protovers", AIRPLAY_PROTOVERS);
    plist_dict_string(&p, "srcvers", AIRPLAY_SOURCE_VERSION);
    plist_dict_int(&p, "vv", AIRPLAY_PROTOCOL_VERSION);
    plist_dict_int(&p, "statusFlags", AIRPLAY_STATUS_FLAGS);
    plist_dict_data(&p, "pk", pk, 32);
    plist_dict_string(&p, "pi", pairing_id);
    plist_dict_string(&p, "name", device_name);

    // Audio formats array
    plist_dict_array_begin(&p, "audioFormats");
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 96);
    plist_dict_int(&p, "audioInputFormats", 0x01000000);
    plist_dict_int(&p, "audioOutputFormats", 0x01000000);
    plist_dict_end(&p);
    plist_array_end(&p);

    // Audio latencies array
    // Both type 96 (realtime/UDP) and type 103 (buffered/TCP) use PTP-based
    // anchor timing; the receiver compensates its own output latency
    // (I2S DMA + configured DAC/DSP latency) internally. Report 0 so the
    // sender does NOT also adjust its anchor, otherwise the delay would be
    // compensated twice and the ESP would play ahead of other speakers.
    plist_dict_array_begin(&p, "audioLatencies");
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 96);
    plist_dict_int(&p, "audioType", 0x64);
    plist_dict_int(&p, "inputLatencyMicros", 0);
    plist_dict_int(&p, "outputLatencyMicros", 0);
    plist_dict_end(&p);
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 103);
    plist_dict_int(&p, "audioType", 0x64);
    plist_dict_int(&p, "inputLatencyMicros", 0);
    plist_dict_int(&p, "outputLatencyMicros", 0);
    plist_dict_end(&p);
    plist_array_end(&p);

    plist_dict_dict_begin(&p, "supportedFormats");
    plist_dict_uint(&p, "audioStream", AIRPLAY_SUPPORTED_AUDIO_STREAM);
    plist_dict_uint(&p, "bufferStream", AIRPLAY_SUPPORTED_BUFFER_STREAM);
    plist_dict_end(&p);

    plist_dict_end(&p);
    size_t body_len = plist_end(&p);

    rtsp_send_http_response(socket, conn, 200, "OK", "text/x-apple-plist+xml",
                            body, body_len);
    free(body);
  } else {
    AP_LOGW(TAG, "Unknown GET path: %s", req->path);
    if (request_uses_rtsp(req)) {
      rtsp_send_response(socket, conn, 404, "Not Found", req->cseq,
                         "Content-Type: text/plain\r\n", "Not Found", 9);
    } else {
      rtsp_send_http_response(socket, conn, 404, "Not Found", "text/plain",
                              "Not Found", 9);
    }
  }
}

static void handle_post(int socket, rtsp_conn_t *conn,
                        const rtsp_request_t *req, const uint8_t *raw,
                        size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  if (strstr(req->path, "/pair-setup")) {
    // Create session if needed
    if (!conn->hap_session) {
      conn->hap_session = hap_session_create();
      if (!conn->hap_session) {
        ESP_LOGE(TAG, "Failed to create HAP session");
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
    }

    uint8_t *response = malloc(2048);
    if (!response) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    size_t response_len = 0;
    esp_err_t err = ESP_FAIL;

    if (body && body_len > 0) {
      size_t state_len;
      const uint8_t *state =
          tlv8_find(body, body_len, TLV_TYPE_STATE, &state_len);

      if (state && state_len == 1) {
        switch (state[0]) {
        default:
          break;
        case 1:
          err = hap_pair_setup_m1(conn->hap_session, body, body_len, response,
                                  2048, &response_len);
          break;
        case 3:
          err = hap_pair_setup_m3(conn->hap_session, body, body_len, response,
                                  2048, &response_len);
          break;
        case 5:
          err = hap_pair_setup_m5(conn->hap_session, body, body_len, response,
                                  2048, &response_len);
          break;
        }
      }
    }

    if (err == ESP_OK && response_len > 0) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         (const char *)response, response_len);

      if (conn->hap_session && conn->hap_session->pair_setup_state == 4 &&
          conn->hap_session->session_established) {
        conn->encrypted_mode = true;
      }
    } else {
      ESP_LOGE(TAG, "Pair-setup failed: err=%d", err);
      static const uint8_t error_response[] = {0x06, 0x01, 0x02,
                                               0x07, 0x01, 0x02};
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         (const char *)error_response, sizeof(error_response));
    }

    free(response);

  } else if (strstr(req->path, "/pair-verify")) {
    if (!conn->hap_session) {
      conn->hap_session = hap_session_create();
      if (!conn->hap_session) {
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
    }

    uint8_t *response = malloc(1024);
    if (!response) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    size_t response_len = 0;
    esp_err_t err = ESP_FAIL;

    if (body && body_len > 0) {
      size_t state_len;
      const uint8_t *state =
          tlv8_find(body, body_len, TLV_TYPE_STATE, &state_len);

      if (state && state_len == 1) {
        if (state[0] == 0x01) {
          err = hap_pair_verify_m1(conn->hap_session, body, body_len, response,
                                   1024, &response_len);
        } else if (state[0] == 0x03) {
          err = hap_pair_verify_m3(conn->hap_session, body, body_len, response,
                                   1024, &response_len);
          // TLV8 pair-verify M3 establishes RTSP channel encryption
          if (err == ESP_OK &&
              conn->hap_session->pair_verify_state == PAIR_VERIFY_STATE_M4) {
            conn->encrypted_mode = true;
            AP_LOGI(TAG, "RTSP encryption enabled (TLV8 pair-verify)");
          }
        }
      } else {
        // Raw format - used for audio encryption keys, not RTSP encryption
        if (conn->hap_session->pair_verify_state == 0) {
          err = hap_pair_verify_m1_raw(conn->hap_session, body, body_len,
                                       response, 1024, &response_len);
        } else if (conn->hap_session->pair_verify_state ==
                   PAIR_VERIFY_STATE_M2) {
          err = hap_pair_verify_m3_raw(conn->hap_session, body, body_len,
                                       response, 1024, &response_len);
          if (err == ESP_OK) {
            AP_LOGI(TAG, "Raw pair-verify complete (RTSP unencrypted)");
          }
        }
      }
    }

    if (err == ESP_OK && response_len > 0) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         (const char *)response, response_len);
    } else {
      ESP_LOGE(TAG, "Pair-verify failed, err=%d", err);
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: application/octet-stream\r\n",
                         "\x06\x01\x04\x07\x01\x02", 6);
    }

    free(response);

  } else if (strstr(req->path, "/fp-setup")) {
    uint8_t *fp_response = NULL;
    size_t fp_response_len = 0;

    if (body && body_len >= 16) {
      if (rtsp_fairplay_handle(body, body_len, &fp_response,
                               &fp_response_len) == 0) {
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/octet-stream\r\n",
                           (const char *)fp_response, fp_response_len);
        free(fp_response);
        return;
      }
    }

    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Content-Type: application/octet-stream\r\n", "\x00", 1);

  } else if (strstr(req->path, "/command")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      char cmd_type[64];
      if (bplist_find_string(body, body_len, "type", cmd_type, sizeof(cmd_type))) {
        AP_LOGI(TAG, "Sender /command type=%s", cmd_type);
      }
    }
    rtsp_send_ok(socket, conn, req->cseq);

  } else if (strstr(req->path, "/feedback")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        AP_LOGI(TAG, "/feedback has networkTimeSecs=%lld", (long long)value);
      }
    }

    // For buffered audio streams (type 103), send a proper feedback response
    // with stream status. This acts as a keepalive to prevent iPhone from
    // sending TEARDOWN during extended pause.
    if (conn->stream_type == 103) {
      uint8_t response[128];
      const uint32_t sample_rate = audio_receiver_get_sample_rate();
      size_t response_len = bplist_build_feedback_response(
          response, sizeof(response), conn->stream_type,
          (double)sample_rate);

      if (response_len > 0) {
        AP_LOGD(
            TAG,
            "/feedback responding with stream status (type=%lld, sr=%u)",
            (long long)conn->stream_type, (unsigned)sample_rate);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/x-apple-binary-plist\r\n",
                           (const char *)response, response_len);
      } else {
        // Fallback to simple OK if response build fails
        rtsp_send_ok(socket, conn, req->cseq);
      }
    } else {
      // For non-buffered streams, simple OK is fine
      rtsp_send_ok(socket, conn, req->cseq);
    }

  } else {
    rtsp_send_ok(socket, conn, req->cseq);
  }
}

static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw;
  (void)raw_len;

  audio_format_t proposed_format = {0};
  audio_encrypt_t proposed_encryption = conn->setup_encryption;
  int64_t proposed_type = conn->stream_type;
  uint16_t proposed_control_port = conn->client_control_port;
  uint32_t proposed_latency = conn->setup_latency;
  bool has_format = false;
  bool has_encryption = false;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  /* AirPlay 2 SETUP always carries a binary plist: the session SETUP without
   * streams[] and the stream SETUP with streams[]. Anything else is a classic
   * RAOP (AirPlay 1) request, which this receiver does not support. */
  if (!body || body_len < 8 || memcmp(body, "bplist00", 8) != 0) {
    AP_LOGW(TAG, "SETUP without binary plist rejected (AirPlay 1 not supported)");
    rtsp_send_response(socket, conn, 461, "Unsupported Transport", req->cseq,
                       NULL, NULL, 0);
    return;
  }

  // Check for streams array
  size_t stream_count = 0;
  const bool request_has_streams =
      bplist_get_streams_count(body, body_len, &stream_count);

  AP_LOGI(TAG, "SETUP: has_streams=%d, stream_count=%zu", request_has_streams,
           stream_count);

  if (request_has_streams) {

    /* This receiver owns one audio stream at a time. Validate the first
     * negotiated stream before committing any codec/stream state. Do not let
     * a later unsupported stream dictionary overwrite the active audio format. */
    int64_t stream_type = -1;
    size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
    if (stream_count == 0 ||
        !bplist_get_stream_info(body, body_len, 0, &stream_type, &ekey_len,
                                &eiv_len, &shk_len)) {
      ESP_LOGE(TAG, "SETUP: malformed AirPlay 2 audio stream");
      rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    /* The parser rejects a stream dict with more keys than this instead of
     * cutting it off; realtime SETUPs carry ~20 keys, so leave room. */
    bplist_kv_info_t kv[32];
    size_t kv_count = 0;
    int64_t audio_format = -1;
    int64_t codec_type = -1;
    int64_t latency_min = 0;
    int64_t sample_rate = -1;
    int64_t samples_per_frame = -1;
    int64_t bits_per_sample = -1;
    int64_t channels = -1;
    if (!bplist_get_stream_kv_info(body, body_len, 0, kv,
                                   sizeof(kv) / sizeof(kv[0]), &kv_count)) {
      ESP_LOGE(TAG, "SETUP: missing AirPlay 2 audio format dictionary");
      rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                         NULL, 0);
      return;
    }

    for (size_t k = 0; k < kv_count; k++) {
      int64_t *field = NULL;
      if (strcmp(kv[k].key, "audioFormat") == 0) field = &audio_format;
      else if (strcmp(kv[k].key, "ct") == 0) field = &codec_type;
      else if (strcmp(kv[k].key, "sr") == 0) field = &sample_rate;
      else if (strcmp(kv[k].key, "spf") == 0) field = &samples_per_frame;
      else if (strcmp(kv[k].key, "ss") == 0) field = &bits_per_sample;
      else if (strcmp(kv[k].key, "ch") == 0) field = &channels;
      if (field) {
        if (kv[k].value_type != BPLIST_VALUE_INT || kv[k].int_value <= 0 ||
            *field != -1) {
          rtsp_send_response(socket, conn, 400, "Invalid Audio Format",
                             req->cseq, NULL, NULL, 0);
          return;
        }
        *field = kv[k].int_value;
        continue;
      }
      if (kv[k].value_type != BPLIST_VALUE_INT) continue;
      if (strcmp(kv[k].key, "controlPort") == 0) {
        if (kv[k].int_value < 0 || kv[k].int_value > UINT16_MAX) {
          rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL, NULL, 0);
          return;
        }
        proposed_control_port = (uint16_t)kv[k].int_value;
      } else if (strcmp(kv[k].key, "latencyMin") == 0) {
        latency_min = kv[k].int_value;
      }
    }

    const airplay_audio_format_t *selected = airplay_audio_format_resolve(
        stream_type, audio_format == -1 ? 0 : (uint64_t)audio_format,
        codec_type, sample_rate, samples_per_frame, bits_per_sample, channels);
    if (!selected) {
      ESP_LOGE(TAG,
               "SETUP: unsupported/conflicting audio format type=%lld "
               "audioFormat=0x%llx ct=%lld sr=%lld spf=%lld ss=%lld ch=%lld",
               (long long)stream_type, (unsigned long long)audio_format,
               (long long)codec_type, (long long)sample_rate,
               (long long)samples_per_frame, (long long)bits_per_sample,
               (long long)channels);
      /* Reject only this SETUP dictionary.  A malformed/unsupported request
       * must not clobber a stream that is already owned by the control
       * session; the sender may retry with a supported stream immediately. */
      rtsp_send_response(socket, conn, 400, "Unsupported Audio Format",
                         req->cseq, NULL, NULL, 0);
      return;
    }

    audio_format_t format = {
        .sample_rate = selected->sample_rate,
        .channels = 2,
        .bits_per_sample = selected->bits_per_sample,
        .frame_size = selected->frame_size,
    };
    strlcpy(format.codec, selected->codec, sizeof(format.codec));
    ESP_LOGI(TAG, "SETUP audioFormat=0x%llx -> %s, block=%d frames (type=%lld)",
             (unsigned long long)selected->audio_format, selected->name,
             selected->frame_size, (long long)stream_type);
    proposed_type = stream_type;
    proposed_format = format;
    has_format = true;
    proposed_latency = stream_type == AUDIO_STREAM_BUFFERED ? 0 :
        (latency_min > 0 && latency_min <= 5 * 44100 ?
         (uint32_t)latency_min : AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES);
  }

  // Process encryption keys
  {
    uint8_t ekey_encrypted[64];
    size_t ekey_len = 0;
    uint8_t eiv[16];
    size_t eiv_len = 0;
    uint8_t shk[crypto_aead_chacha20poly1305_ietf_KEYBYTES];
    size_t shk_len = 0;

    int64_t crypto_stream_type = proposed_type > 0 ? proposed_type : 96;
    bool has_stream_crypto = bplist_find_stream_crypto(
        body, body_len, crypto_stream_type, ekey_encrypted,
        sizeof(ekey_encrypted), &ekey_len, eiv, sizeof(eiv), &eiv_len, shk,
        sizeof(shk), &shk_len);

    if (!has_stream_crypto || (ekey_len == 0 && shk_len == 0)) {
      bplist_find_data_deep(body, body_len, "ekey", ekey_encrypted,
                            sizeof(ekey_encrypted), &ekey_len);
      bplist_find_data_deep(body, body_len, "eiv", eiv, sizeof(eiv), &eiv_len);
      bplist_find_data_deep(body, body_len, "shk", shk, sizeof(shk), &shk_len);
    }

    if (eiv_len != 0 && eiv_len != 16) {
      rtsp_send_response(socket, conn, 400, "Invalid Audio IV", req->cseq, NULL, NULL, 0);
      return;
    }
    audio_encrypt_t audio_encrypt = {0};
    bool encryption_set = false;

    if (shk_len == crypto_aead_chacha20poly1305_ietf_KEYBYTES) {
      // Shairport Sync 5.5+: "shk" is a ChaCha20-Poly1305-IETF key and
      // must be exactly crypto_aead_chacha20poly1305_ietf_KEYBYTES bytes.
      audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
      memcpy(audio_encrypt.key, shk, sizeof(shk));
      audio_encrypt.key_len = sizeof(shk);
      if (eiv_len >= 16) {
        memcpy(audio_encrypt.iv, eiv, 16);
      }
      proposed_encryption = audio_encrypt;
      has_encryption = true;
      encryption_set = true;
    } else if (shk_len != 0) {
      rtsp_send_response(socket, conn, 400, "Invalid Audio Key", req->cseq, NULL, NULL, 0);
      return;
    } else if (ekey_len > 16 && conn->hap_session &&
               conn->hap_session->session_established) {
      uint8_t nonce[12] = {0};
      /* AEAD may output every ciphertext byte except the authentication tag. */
      uint8_t decrypted_key[sizeof(ekey_encrypted)];
      unsigned long long decrypted_len;

      if (crypto_aead_chacha20poly1305_ietf_decrypt(
              decrypted_key, &decrypted_len, NULL, ekey_encrypted, ekey_len,
              NULL, 0, nonce, conn->hap_session->shared_secret) == 0 &&
          decrypted_len == sizeof(audio_encrypt.key)) {
        audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
        memcpy(audio_encrypt.key, decrypted_key, sizeof(audio_encrypt.key));
        audio_encrypt.key_len = sizeof(audio_encrypt.key);
        if (eiv_len >= 16) {
          memcpy(audio_encrypt.iv, eiv, 16);
        }
        proposed_encryption = audio_encrypt;
        has_encryption = true;
        encryption_set = true;
      }
    }

    if (ekey_len && !encryption_set && !has_encryption) {
      rtsp_send_response(socket, conn, 400, "Invalid Audio Key", req->cseq, NULL, NULL, 0);
      return;
    }

    if (!encryption_set && conn->setup_encryption.type != AUDIO_ENCRYPT_NONE) {
      /* Omitted keys retain the committed session key, including retries. */
      if (eiv_len == 16) {
        memcpy(proposed_encryption.iv, eiv, 16);
        has_encryption = true;
      }
    } else if (!encryption_set && conn->hap_session &&
               conn->hap_session->session_established) {
      audio_encrypt.type = AUDIO_ENCRYPT_CHACHA20_POLY1305;
      if (hap_derive_audio_key(conn->hap_session, audio_encrypt.key,
                               sizeof(audio_encrypt.key)) == ESP_OK) {
        audio_encrypt.key_len = 32;
        if (eiv_len >= 16) {
          memcpy(audio_encrypt.iv, eiv, 16);
        }
        proposed_encryption = audio_encrypt;
        has_encryption = true;
      }
    }
  }

  if (request_has_streams && !has_format) {
    rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL, NULL, 0);
    return;
  }

  /* Validate the entire request before publishing format/key. A live stream
   * only accepts the same negotiated configuration and returns its ports. */
  if (conn->stream_active) {
    const audio_format_t *old = &conn->setup_format;
    bool same_format = !has_format || (conn->setup_format_valid &&
        proposed_type == conn->stream_type &&
        strcmp(proposed_format.codec, old->codec) == 0 &&
        proposed_format.sample_rate == old->sample_rate &&
        proposed_format.channels == old->channels &&
        proposed_format.bits_per_sample == old->bits_per_sample &&
        proposed_format.frame_size == old->frame_size &&
        proposed_control_port == conn->client_control_port &&
        proposed_latency == conn->setup_latency);
    const audio_encrypt_t *old_key = &conn->setup_encryption;
    bool same_key = !has_encryption || (
        proposed_encryption.type == old_key->type &&
        proposed_encryption.key_len == old_key->key_len &&
        memcmp(proposed_encryption.key, old_key->key, sizeof(old_key->key)) == 0 &&
        memcmp(proposed_encryption.iv, old_key->iv, sizeof(old_key->iv)) == 0);
    if (!same_format || !same_key) {
      rtsp_send_response(socket, conn, 455, "Method Not Valid In This State",
                         req->cseq, NULL, NULL, 0);
      return;
    }
    uint8_t reply[256];
    size_t reply_len;
    if (request_has_streams) {
      bool buffered = conn->stream_type == AUDIO_STREAM_BUFFERED;
      reply_len = bplist_build_stream_setup(reply, sizeof(reply), conn->stream_type,
          buffered ? conn->buffered_port : conn->data_port, conn->control_port,
          buffered ? (uint32_t)AP2_BUFFERED_AUDIO_ADVERTISED_BYTES : 0U);
    } else {
      reply_len = bplist_build_initial_setup(reply, sizeof(reply), conn->event_port);
    }
    if (!reply_len) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
      return;
    }
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
        "Content-Type: application/x-apple-binary-plist\r\n", (const char *)reply, reply_len);
    return;
  }
  /* Shairport 5.6-dev-157: the play lock (and so the right to end a running
   * session) is taken at the stream SETUP, when this connection really wants
   * to play. The session SETUP (no streams) takes it only if nobody has it. */
  if (!rtsp_session_acquire(conn, request_has_streams)) {
    if (request_has_streams) {
      AP_LOGW(TAG, "SETUP: could not take the play lock");
      rtsp_send_response(socket, conn, 503, "Service Unavailable", req->cseq,
                         NULL, NULL, 0);
      return;
    }
    AP_LOGI(TAG, "SETUP: another connection is playing; its session is kept");
  } else if (request_has_streams && !session_apply_deferred(conn)) {
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return;
  }
  if (conn->owns_session && (has_format || has_encryption) &&
      !audio_receiver_is_idle()) {
    rtsp_send_response(socket, conn, 455, "Method Not Valid In This State",
                       req->cseq, NULL, NULL, 0);
    return;
  }
  if (has_format && conn->owns_session) {
    conn->stream_type = proposed_type;
    conn->client_control_port = proposed_control_port;
    conn->setup_format = proposed_format;
    conn->setup_format_valid = true;
    conn->setup_latency = proposed_latency;
    audio_receiver_set_stream_type((audio_stream_type_t)proposed_type);
    audio_receiver_set_format(&proposed_format);
  }
  if (has_encryption) {
    conn->setup_encryption = proposed_encryption;
    if (conn->owns_session)
      audio_receiver_set_encryption(&proposed_encryption);
    else
      conn->pending_encryption = true;
  }

  // Create event port if needed
  if (conn->event_port == 0) {
    conn->event_socket = rtsp_create_event_socket(&conn->event_port);
    if (conn->event_socket < 0) {
      conn->event_port = 0;
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL, NULL, 0);
      return;
    }
    if (!conn->owns_session) {
      /* The event task is a single global one: keep the listener open (the
       * port is reported to the sender) and start it after the play lock. */
      AP_LOGI(TAG, "SETUP: event port %u reserved", conn->event_port);
    } else if (conn->event_socket >= 0) {
      if (rtsp_start_event_port_task(conn->event_socket, conn) == ESP_OK) {
        AP_LOGI(TAG, "SETUP: Created event port %u", conn->event_port);
        conn->event_socket = -1; /* Ownership transferred to event task. */
      } else {
        close(conn->event_socket);
        conn->event_socket = -1;
        conn->event_port = 0;
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                           NULL, 0);
        return;
      }
    }
  }

  // Handle initial SETUP vs stream SETUP
  if (!request_has_streams) {
    AP_LOGI(TAG, "SETUP: Initial connection setup (no streams)");

    uint8_t plist_body[128];
    size_t plist_len = bplist_build_initial_setup(
        plist_body, sizeof(plist_body), conn->event_port);
    if (plist_len == 0) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Content-Type: application/x-apple-binary-plist\r\n",
                       (const char *)plist_body, plist_len);
    if (conn->owns_session) amp_session_activate_once(conn);
    return;
  }

  // Stream SETUP
  int64_t stream_type = conn->stream_type;
  if (stream_type == 0) {
    stream_type = 96;
  }

  AP_LOGI(TAG, "SETUP: Stream setup, stream_type=%lld",
           (long long)stream_type);

  bool buffered = (stream_type == AUDIO_STREAM_BUFFERED);

  /* Shairport development rtsp.c:2642-2644 sends T <RTSP client IP>,
   * then B. NQPTP starts one new timing record at this stream SETUP;
   * SETPEERS and SETRATEANCHORTIME must not reset it again.
   * Reference: 8c146e25183a354ae96f91fa069c2e7173f75a71. */
  ptp_clock_start_timing(!buffered, conn->client_ip);

  if (buffered) {
    esp_err_t err = audio_receiver_start_buffered(0);
    if (err != ESP_OK) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                         NULL, 0);
      return;
    }
    conn->buffered_port = audio_receiver_get_buffered_port();
  }

  // Buffered audio (type 103) uses only the TCP dataPort plus PTP; realtime
  // (type 96) needs UDP data and control (retransmit) ports.
  if (!buffered) {
    ensure_realtime_ports(conn);
  } else {
    conn->data_port = 0;
    conn->control_port = 0;
  }

  uint16_t response_data_port =
      buffered ? conn->buffered_port : conn->data_port;

  if (!start_audio_receiver_or_fail(socket, conn, req, stream_type)) {
    return;
  }

  // Playout latency.  For REALTIME streams (type 96) the anchor from
  // SETRATEANCHORTIME maps an RTP timestamp onto the sender's source
  // timeline; actual playout is expected latencyMin samples later.  Every
  // reference receiver applies this, so playing at the anchor directly puts
  // this device ~250 ms AHEAD of the rest of a multi-room group (issue #54:
  // an empirical +300 ms offset on a build that subtracted 46 ms of
  // hardware latency — net +254 ms — gave near-perfect sync; 11025 samples
  // is 250.0 ms).  Use the sender's latencyMin from the SETUP stream dict
  // when present and sane, else the AirPlay default of 11025.
  // Buffered streams (type 103) schedule playout with the anchor directly.
  if (buffered) {
    audio_receiver_set_playout_latency_samples(0);
  } else {
    audio_receiver_set_playout_latency_samples(conn->setup_latency);
    AP_LOGI(TAG, "Realtime playout latency: %u samples", (unsigned)conn->setup_latency);
  }

  uint8_t plist_body[256];
  uint32_t audio_buffer_size = buffered
      ? (uint32_t)AP2_BUFFERED_AUDIO_ADVERTISED_BYTES : 0U;
  size_t plist_len = bplist_build_stream_setup(
      plist_body, sizeof(plist_body), stream_type, response_data_port,
      conn->control_port, audio_buffer_size);
  if (plist_len == 0) {
    audio_receiver_stop();
    rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, NULL,
                       NULL, 0);
    return;
  }
  AP_LOGI(TAG,
           "SETUP response: type=%lld dataPort=%u controlPort=%u audioBufferSize=%u",
           (long long)stream_type, response_data_port, conn->control_port,
           (unsigned)audio_buffer_size);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Content-Type: application/x-apple-binary-plist\r\n",
                     (const char *)plist_body, plist_len);

  // Realtime: enable NACK retransmission requests to the sender's control port
  if (conn->client_control_port > 0 && conn->client_ip != 0) {
    audio_receiver_set_client_control(conn->client_ip,
                                      conn->client_control_port);
  }

  conn->stream_active = true;
  amp_session_activate_once(conn);

  if (stream_type == AUDIO_STREAM_REALTIME) {
    /* Realtime ALAC (type 96) needs the play gate open here: in this
     * transport the presentation mapping can arrive through D7 and the
     * sender may never send SETRATEANCHORTIME rate=1. Buffered AAC
     * (type 103) remains gated exclusively by SETRATEANCHORTIME rate=1. */
    audio_receiver_set_playing(true);
    conn->stream_paused = false;
    rtsp_events_emit(RTSP_EVENT_PLAYING);
  }
}

static void handle_record(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len) {
  (void)raw;
  (void)raw_len;

  /* AirPlay 2 RECORD is a control-plane acknowledgement.  The buffered
   * transport may not even have been SETUP yet, and there is no presentation
   * anchor here.  Do not change playing/paused state here: buffered AP2 is
   * opened by SETRATEANCHORTIME, while realtime AP2 is opened by stream SETUP
   * and remains blocked downstream until D7/PTP timing becomes valid.
   * Output latency is compensated internally, so report 0. */
  rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                     "Audio-Latency: 0\r\n"
                     "Audio-Jack-Status: connected\r\n",
                     NULL, 0);
}

#if defined(CONFIG_AIRPLAY_LOG_METADATA) && CONFIG_AIRPLAY_LOG_METADATA
/* Track metadata log (menuconfig: AirPlay Logging -> Log track metadata).
 * Only logged, never used: title/artist/album/genre from the DMAP body and
 * the "progress: start/current/end" RTP triple. Compiled out when off. */
#define META_FIELD_MAX    96
#define DMAP_MAX_NESTING  8

typedef struct {
  char title[META_FIELD_MAX];
  char artist[META_FIELD_MAX];
  char album[META_FIELD_MAX];
  char genre[META_FIELD_MAX];
} meta_log_t;

static uint32_t s_meta_log_hash;
static bool s_meta_log_hash_valid;

static void meta_copy(char *dst, const uint8_t *src, uint32_t len) {
  const uint32_t n = len < META_FIELD_MAX - 1U ? len : META_FIELD_MAX - 1U;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

/* DMAP: 4-byte tag, 4-byte big-endian length, payload. */
static void dmap_collect(const uint8_t *data, size_t len, meta_log_t *m,
                         int depth) {
  if (depth > DMAP_MAX_NESTING) return;
  size_t pos = 0;
  while (pos + 8U <= len) {
    const uint8_t *tag = data + pos;
    const uint32_t item_len = ((uint32_t)data[pos + 4] << 24) |
                              ((uint32_t)data[pos + 5] << 16) |
                              ((uint32_t)data[pos + 6] << 8) | data[pos + 7];
    pos += 8U;
    if (item_len > len - pos) break; /* malformed */
    if (memcmp(tag, "minm", 4) == 0) {
      meta_copy(m->title, data + pos, item_len);
    } else if (memcmp(tag, "asar", 4) == 0) {
      meta_copy(m->artist, data + pos, item_len);
    } else if (memcmp(tag, "asal", 4) == 0) {
      meta_copy(m->album, data + pos, item_len);
    } else if (memcmp(tag, "asgn", 4) == 0) {
      meta_copy(m->genre, data + pos, item_len);
    } else if (memcmp(tag, "mlit", 4) == 0 || memcmp(tag, "cmst", 4) == 0 ||
               memcmp(tag, "mdst", 4) == 0) {
      dmap_collect(data + pos, item_len, m, depth + 1); /* containers */
    }
    pos += item_len;
  }
}

static void log_dmap_metadata(const uint8_t *data, size_t len) {
  meta_log_t m = {0};
  dmap_collect(data, len, &m, 0);
  /* The sender repeats identical metadata: log only changes (FNV-1a). */
  uint32_t hash = 2166136261U;
  const char *fields[] = {m.title, m.artist, m.album, m.genre};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    for (const unsigned char *c = (const unsigned char *)fields[i]; *c; ++c) {
      hash ^= *c;
      hash *= 16777619U;
    }
    hash ^= 0xffU;
    hash *= 16777619U;
  }
  if (s_meta_log_hash_valid && hash == s_meta_log_hash) return;
  s_meta_log_hash = hash;
  s_meta_log_hash_valid = true;
  ESP_LOGI(TAG, "Now playing: \"%s\" - %s", m.title[0] ? m.title : "?",
           m.artist[0] ? m.artist : "?");
  if (m.album[0] || m.genre[0]) {
    ESP_LOGI(TAG, "  Album: %s%s%s", m.album[0] ? m.album : "?",
             m.genre[0] ? " | Genre: " : "", m.genre);
  }
}

/* Find "key" inside text[0..len) (text need not be NUL-terminated). */
static const char *find_param(const char *text, size_t len, const char *key) {
  const size_t klen = strlen(key);
  for (size_t i = 0; i + klen <= len; ++i) {
    if (strncasecmp(text + i, key, klen) == 0) return text + i + klen;
  }
  return NULL;
}

static void log_progress(const char *text, size_t len) {
  const char *v = find_param(text, len, "progress:");
  if (!v) return;
  char buf[64];
  size_t n = 0;
  while (v + n < text + len && n < sizeof(buf) - 1U && v[n] != '\r' &&
         v[n] != '\n') {
    buf[n] = v[n];
    ++n;
  }
  buf[n] = '\0';
  uint64_t start = 0, current = 0, end = 0;
  // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
  if (sscanf(buf, " %" SCNu64 "/%" SCNu64 "/%" SCNu64, &start, &current,
             &end) != 3) {
    return;
  }
  /* RTP timestamps, modulo 2^32, at 44.1 kHz. */
  const uint32_t duration = (uint32_t)end - (uint32_t)start;
  const uint32_t position = (uint32_t)current - (uint32_t)start;
  if (duration >= UINT32_C(0x80000000) || position > duration) return;
  const uint32_t pos_s = position / 44100U, dur_s = duration / 44100U;
  ESP_LOGI(TAG, "Progress: %" PRIu32 ":%02" PRIu32 " / %" PRIu32 ":%02" PRIu32,
           pos_s / 60U, pos_s % 60U, dur_s / 60U, dur_s % 60U);
}
#endif /* CONFIG_AIRPLAY_LOG_METADATA */

static void handle_set_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
#if defined(CONFIG_AIRPLAY_LOG_METADATA) && CONFIG_AIRPLAY_LOG_METADATA
  if (req->body && req->body_len > 0 &&
      strstr(req->content_type, "application/x-dmap-tagged")) {
    log_dmap_metadata(req->body, req->body_len);
  }
  /* Progress comes as a header or inside a text/parameters body. */
  if (raw && raw_len > 0) {
    log_progress((const char *)raw, raw_len);
  }
#else
  (void)raw;
  (void)raw_len;
#endif

  /* Only the volume changes playback. Metadata/progress above are just
   * logged (menuconfig); artwork and other bodies are accepted and ignored. */
  if (req->body && strstr(req->content_type, "text/parameters")) {
    const char *vol = strstr((const char *)req->body, "volume:");
    if (vol) {
      char *end = NULL;
      errno = 0;
      float volume = strtof(vol + 7, &end);
      const bool parsed = end != vol + 7 && errno != ERANGE && isfinite(volume);
      while (*end == ' ' || *end == '\t') ++end;
      if (!parsed || (*end != '\0' && *end != '\r' && *end != '\n')) {
        rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                           NULL, 0);
        return;
      }
      rtsp_conn_set_volume(conn, volume);
    }
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_get_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (req->body && req->body_len > 0) {
    if (strstr((const char *)req->body, "volume")) {
      char vol_response[32];
      int vol_len = snprintf(vol_response, sizeof(vol_response),
                             "volume: %.2f\r\n", rtsp_conn_get_volume_db(conn));
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: text/parameters\r\n", vol_response,
                         vol_len);
      return;
    }
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

/* LOUDNESSNORMALIZATION: newer iOS/tvOS senders send this during SETUP with
 * a bplist { loudnessNormalizationEnabled: <bool> }. Accept it with 200 OK
 * like a real speaker instead of 501; the setting is not applied. */
static void handle_loudnessnormalization(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req,
                                         const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_pause(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw;
  (void)raw_len;

  AP_LOGI(TAG, "PAUSE received");

  // Stop the audio consumer but leave the buffer filling.  The phone will
  // send a fresh SETRATEANCHORTIME (rate=1) anchor on resume that re-aligns
  // the buffered frames to the correct wall-clock position.
  audio_receiver_pause();
  conn->stream_paused = true;

  rtsp_send_ok(socket, conn, req->cseq);
  (void)settings_persist_volume(); /* playback stopped: no flash write mid-music */
}

static void handle_flush(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  AP_LOGI(TAG, "FLUSH received");
  if (conn->stream_type == AUDIO_STREAM_REALTIME) {
    uint32_t flush_rtp = 0;
    if (parse_rtp_info_rtptime(raw, raw_len, &flush_rtp)) {
      AP_LOGI(TAG,
               "FLUSH realtime RTP-Info rtptime=%" PRIu32
               " — preserving D7/SETRATE timing",
               flush_rtp);
      audio_receiver_realtime_flush_to_rtp(flush_rtp);
    } else {
      /* No RTP boundary means we cannot know which buffered realtime samples
       * remain valid. Invalidate the epoch and wait for the next REAL sender
       * D7/SETRATE anchor. realtime_pcm_sink has no local-arrival fallback. */
      AP_LOGW(TAG,
               "FLUSH realtime without RTP-Info — waiting for fresh sender anchor");
      audio_receiver_realtime_flush_wait_sender_anchor();
    }
  } else {
    /* No buffered 23-bit endpoint: stop old media and wait for a real anchor. */
    audio_receiver_seek_flush();
  }
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_flushbuffered(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  // AirPlay 2 FLUSHBUFFERED carries an optional bplist with:
  //   flushFromSeq / flushFromTS  — first sequence/timestamp to discard
  //   flushUntilSeq / flushUntilTS — exclusive end boundary
  //
  // Shairport-style sequential semantics:
  // - no flushFromSeq: immediate flush; the packet consumer discards forward
  //   until it reaches/overshoots flushUntilSeq.
  // - flushFromSeq present: register a deferred [from, until) discard rule.
  // The raw TCP byte FIFO is never searched or rebound by a FLUSH command.
  if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    int64_t flush_from_seq = 0, flush_from_ts = 0;
    int64_t flush_until_seq = 0, flush_until_ts = 0;
    bool got_from_seq =
        bplist_find_int(body, body_len, "flushFromSeq", &flush_from_seq);
    bool got_from_ts =
        bplist_find_int(body, body_len, "flushFromTS", &flush_from_ts);
    bool got_until_seq =
        bplist_find_int(body, body_len, "flushUntilSeq", &flush_until_seq);
    bool got_until_ts =
        bplist_find_int(body, body_len, "flushUntilTS", &flush_until_ts);

    /* Key presence is retained in diagnostics. A deferred zero endpoint may
     * be real sequence wrap; only immediate no-boundary recovery is special. */
    if (got_until_seq && flush_until_seq == 0) {
      AP_LOGW(TAG,
               "FLUSHBUFFERED explicit untilSeq=0 (%s); "
               "%s",
               got_from_seq ? "deferred" : "immediate",
               got_from_seq
                   ? "deferred request keeps normal sequence semantics"
                   : "consumer will wait for the next anchor, then resync RTP");
    }

    /* Shairport Sync 5.5.2 handle_flushbuffered(): deferred iff flushFromSeq is
     * present (the other fields default to 0 when missing). */
    if (got_from_seq) {
      AP_LOGI(TAG,
               "FLUSHBUFFERED deferred: fromSeq=%" PRId64 " fromTS=%" PRId64
               " untilSeq=%" PRId64 " untilTS=%" PRId64,
               flush_from_seq, flush_from_ts, flush_until_seq, flush_until_ts);
      esp_err_t flush_err = audio_receiver_set_deferred_flush_range(
          (uint32_t)flush_from_seq, (uint32_t)flush_from_ts,
          (uint32_t)flush_until_seq, (uint32_t)flush_until_ts,
          got_from_ts && got_until_ts);
      if (flush_err != ESP_OK) {
        /* Shairport: "no more room for deferred flush request records" is
         * only logged; the reply is still 200. */
        AP_LOGW(TAG, "FLUSHBUFFERED deferred not stored (%s)",
                 esp_err_to_name(flush_err));
      }
    } else {
      /* The absence of flushFromSeq selects immediate mode. Non-zero
       * flushUntilSeq follows Shairport sequence semantics. An explicit zero
       * uses the receiver's experimental no-boundary policy: wait for the new
       * anchor, then sequentially classify compressed packets for admission. */
      if (!got_until_seq) {
        AP_LOGW(TAG,
                 "FLUSHBUFFERED immediate without flushUntilSeq; "
                 "no sequence boundary; waiting for fresh anchor");
      }
      AP_LOGI(TAG,
               "FLUSHBUFFERED immediate: untilSeq=%" PRId64
               " untilTS=%" PRId64,
               flush_until_seq, flush_until_ts);
      audio_receiver_set_immediate_flush((uint32_t)flush_until_seq,
                                         (uint32_t)flush_until_ts);
    }
  }

  /* Shairport: a FLUSHBUFFERED without a plist does nothing and returns 200. */

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_teardown(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  bool has_streams = false;
  size_t stream_count = 0;

  if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    if (bplist_get_streams_count(body, body_len, &stream_count)) {
      has_streams = true;
    }
  }

  // TEARDOWN with streams = stream teardown (may be followed by new SETUP)
  // TEARDOWN without streams = full session teardown (disconnect)
  /* Shairport Sync 5.5.2 handle_teardown_2(): no plist -> nothing done. */
  if (!(body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0)) {
    AP_LOGW(TAG, "TEARDOWN without plist - nothing done (as Shairport)");
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }
  AP_LOGI(TAG, "TEARDOWN: has_streams=%d stream_count=%zu", has_streams,
           stream_count);
  // Stream-level teardown is a pause: close the play gate immediately and
  // emit PAUSED (LED state) before the receiver/decoder teardown below
  // (normally 10-30 ms; its waits are only capped at ~1.25 s).
  if (has_streams) {
    audio_receiver_set_playing(false);
    rtsp_events_emit(RTSP_EVENT_PAUSED);
  }
  audio_receiver_stop();
  /* A stream-level TEARDOWN preserves the control session and its PTP clock.
   * Clearing it here needlessly forces a new PTP STEP/relock before the next
   * buffered stream can produce PCM.  Only a full session boundary owns a
   * full PTP reset. */
  /* Client cleanup remains the final reset owner when stop times out. */
  if (!has_streams && audio_receiver_is_idle()) {
    ptp_clock_clear();
    audio_receiver_set_stream_type(AUDIO_STREAM_NONE);
    audio_receiver_set_encryption(NULL);
  }
  conn->stream_active = false;
  conn->stream_paused =
      has_streams; // Keep session ready if only streams torn down

  if (!has_streams) {
    /* Full AirPlay session teardown starts the amplifier grace period now.
     * rtsp_conn_free() remains the backstop for unexpected socket loss. */
    amp_session_deactivate_once(conn);

    // Full teardown — server cleanup will emit RTSP_EVENT_DISCONNECTED
    // when the TCP connection closes.
    // Shairport Sync 5.1+ handle_teardown_2(): a valid AP2 TEARDOWN plist
    // without a streams item means terminate the RTSP connection. Send the
    // response first, advertise the close, then let the client task unwind.
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Connection: close\r\n", NULL, 0);
    conn->close_after_response = true;
    return;
  }

  rtsp_send_ok(socket, conn, req->cseq);
  (void)settings_persist_volume(); /* stream stopped: no flash write mid-music */
}

static void handle_setrateanchortime(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req,
                                     const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  double rate = 1.0;
  bool have_rate = false;
  uint64_t clock_id = 0;
  uint64_t network_time_secs = 0;
  uint64_t network_time_frac = 0;
  uint64_t rtp_time = 0;
  bool have_network_time_secs = false;
  bool have_rtp_time = false;

  if (body && body_len > 0 && body_len >= 8 &&
      memcmp(body, "bplist00", 8) == 0) {
    if (bplist_find_real(body, body_len, "rate", &rate)) {
      have_rate = true;
    } else {
      int64_t rate_int;
      if (bplist_find_int(body, body_len, "rate", &rate_int)) {
        rate = (double)rate_int;
        have_rate = true;
      }
    }

    int64_t value;
    if (bplist_find_int(body, body_len, "networkTimeTimelineID", &value)) {
      clock_id = (uint64_t)value;
    }
    if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
      network_time_secs = (uint64_t)value;
      have_network_time_secs = true;
    }
    if (bplist_find_int(body, body_len, "networkTimeFrac", &value)) {
      network_time_frac = (uint64_t)value;
    }
    if (bplist_find_int(body, body_len, "rtpTime", &value)) {
      rtp_time = (uint64_t)value;
      have_rtp_time = true;
    }

    AP_LOGI(TAG,
             "SETRATEANCHORTIME: secs=%llu frac=0x%016llx rtp=%llu"
             " clock=%016llx rate=%.1f stream=%lld",
             (unsigned long long)network_time_secs,
             (unsigned long long)network_time_frac,
             (unsigned long long)rtp_time,
             (unsigned long long)clock_id, rate,
             (long long)conn->stream_type);

    /* Pause first.  Do not publish an anchor from a rate=0 message and wake a
     * processor immediately before closing the play gate. */
    if (have_rate && rate == 0.0) {
      AP_LOGI(TAG, "SETRATEANCHORTIME: rate=0 -> PAUSING");
      conn->stream_paused = true;
      audio_receiver_pause();
      rtsp_events_emit(RTSP_EVENT_PAUSED);
      rtsp_send_ok(socket, conn, req->cseq);
      (void)settings_persist_volume(); /* playback stopped: no flash write mid-music */
      return;
    }

    /* RTP is a wrapping 32-bit timeline. rtpTime==0 is therefore perfectly
     * valid; validity comes from key presence, not from the numeric value. */
    /* A real RTP key and timeline ID are required to commit a new map.
     * Missing/default RTP must not release zero-seq recovery at timestamp 0. */
    if (have_network_time_secs && have_rtp_time && clock_id != 0U) {
      uint64_t frac = network_time_frac >> 32;
      frac = (frac * 1000000000ULL) >> 32;
      uint64_t network_time_ns = network_time_secs * 1000000000ULL + frac;
      AP_LOGI(TAG,
               "SETRATEANCHORTIME MAP: clock=%016llx ptp=%llu rtp=%llu",
               (unsigned long long)clock_id,
               (unsigned long long)network_time_ns,
               (unsigned long long)rtp_time);

      /* Realtime SETRATEANCHORTIME follows the same rule as D7: convert the
       * sender's current GM timestamp once into ESP monotonic time. If the new
       * GM is still acquiring, keep the already-running local ALAC map and
       * defer this observation; never inject a raw PTP epoch into audio. */
      ptp_realtime_snapshot_t ps = {0};
      ptp_clock_get_realtime_snapshot(&ps);
      if (ps.realtime_mode) {
        uint64_t local_ns = 0;
        if (ptp_clock_realtime_snapshot_to_local(&ps, clock_id, network_time_ns,
                                              &local_ns)) {
          AP_LOGI(TAG,
                   "SETRATEANCHORTIME RT local raw=%llu local=%llu gm=%016llx age=%lums",
                   (unsigned long long)network_time_ns,
                   (unsigned long long)local_ns,
                   (unsigned long long)ps.master_clock_id,
                   (unsigned long)ps.mastership_age_ms);
          audio_realtime_anchor_result_t anchor_result = {0};
          if (!audio_receiver_set_realtime_anchor_local(
                  clock_id, ps.gm_change_count, ps.mastership_age_ms,
                  network_time_ns, local_ns, (uint32_t)rtp_time,
                  &anchor_result)) {
            AP_LOGI(TAG,
                     "SETRATEANCHORTIME RT media rebase deferred clock=%016llx gm=%016llx epoch=%lu age=%lums",
                     (unsigned long long)clock_id,
                     (unsigned long long)ps.master_clock_id,
                     (unsigned long)ps.gm_change_count,
                     (unsigned long)ps.mastership_age_ms);
          }
        } else {
          AP_LOGI(TAG,
                   "SETRATEANCHORTIME RT deferred clock=%016llx gm=%016llx ready=%d age=%lums",
                   (unsigned long long)clock_id,
                   (unsigned long long)ps.master_clock_id,
                   ps.master_ready ? 1 : 0,
                   (unsigned long)ps.mastership_age_ms);
        }
      } else {
        audio_receiver_set_anchor_time(clock_id, network_time_ns,
                                       (uint32_t)rtp_time);
      }
    }
  }

  /* Shairport Sync 5.5.2 handle_setrateanchori(): play state changes only
   * when "rate" is present. Without rate, update any supplied anchor and
   * leave the play state unchanged. */
  if (!have_rate) {
    AP_LOGI(TAG, "SETRATEANCHORTIME: no rate -> play state unchanged");
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }
  AP_LOGI(TAG, "SETRATEANCHORTIME: rate=%.1f -> RESUMING (was_paused=%d)",
           rate, conn->stream_paused);
  conn->stream_paused = false;
  audio_receiver_set_playing(true);
  rtsp_events_emit(RTSP_EVENT_PLAYING);

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_setpeers(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;
  /* Shairport development handle_setpeers / handle_setpeersx only return
   * 200. The timing source is selected at stream SETUP, not by these lists.
   * Reference: rtsp.c:2158-2201 at
   * 8c146e25183a354ae96f91fa069c2e7173f75a71. */
  rtsp_send_ok(socket, conn, req->cseq);
}
