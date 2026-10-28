#pragma once

/* Gateway <-> speaker control link (one TCP connection per speaker).
 *
 * The speaker finds the Gateway by mDNS (_apsurround._tcp) and connects.
 * Every frame is [u16 big-endian length of type+payload][u8 type][payload].
 * Payloads are the packed structs below; both ends are ESP32-S3 built from
 * this tree, so they share layout and byte order. HELLO carries the protocol
 * version and the Gateway drops a speaker whose version differs.
 *
 * Gateway -> speaker messages are the audio_receiver.h calls RTSP makes on
 * the Gateway, with the same arguments; the speaker makes the same calls
 * locally. The audio stream travels on a separate TCP connection
 * (gw_relay.c connects to the port in START_RESULT), so a backlog of audio
 * never delays a command. */

#include <assert.h>
#include <stdint.h>

#define SURROUND_PROTO_VERSION   6
#define SURROUND_STR_(x)         #x
#define SURROUND_STR(x)          SURROUND_STR_(x)
#define SURROUND_HELLO_MAGIC     0x53524e44U /* "SRND" */
#define SURROUND_MDNS_SERVICE    "_apsurround"
#define SURROUND_MDNS_PROTO      "_tcp"
#define SURROUND_CONTROL_PORT    7100U
#define SURROUND_MAX_SPEAKERS    8U
/* Largest payload of any message; frames are bounded by it. */
#define SURROUND_MAX_PAYLOAD     64U

/* Liveness: the Gateway sends PING every interval and the speaker answers
 * PONG; either side drops the link after the timeout without traffic. */
#define SURROUND_PING_INTERVAL_MS 2000U
#define SURROUND_LINK_TIMEOUT_MS  6000U

/* Sender time: the Gateway sends TIME at the sender's PTP Sync rate. Both
 * ends are stations of the same Wi-Fi access point, so they share its TSF
 * (802.11 beacon timer): the Gateway says "sender PTP time P is TSF T", and
 * the speaker turns T into its own clock through its own TSF reading. The
 * transport delay of the message does not enter the result. */
#define SURROUND_TIME_INTERVAL_MS 125U
#define SURROUND_STATUS_MS        2000U

/* audio_stream_type_t values, for code that does not include
 * audio_receiver.h. */
#define SURROUND_STREAM_REALTIME 96U
#define SURROUND_STREAM_BUFFERED 103U

typedef enum {
  /* speaker -> Gateway */
  SURROUND_MSG_HELLO = 0x01,
  SURROUND_MSG_START_RESULT = 0x02,
  SURROUND_MSG_PONG = 0x03,
  SURROUND_MSG_CONFIG = 0x04, /* after HELLO and after every SET_CONFIG */
  SURROUND_MSG_STATUS = 0x05, /* what it plays, every SURROUND_STATUS_MS */

  /* Gateway -> speaker: audio_receiver.h calls */
  SURROUND_MSG_SET_STREAM_TYPE = 0x10,
  SURROUND_MSG_SET_FORMAT = 0x11,
  SURROUND_MSG_SET_ENCRYPTION = 0x12,
  SURROUND_MSG_START_BUFFERED = 0x13,
  SURROUND_MSG_STOP = 0x14,
  SURROUND_MSG_SET_VOLUME = 0x15,
  SURROUND_MSG_SEEK_FLUSH = 0x16,
  SURROUND_MSG_DEFERRED_FLUSH = 0x17,
  SURROUND_MSG_IMMEDIATE_FLUSH = 0x18,
  SURROUND_MSG_PAUSE = 0x19,
  SURROUND_MSG_SET_PLAYING = 0x1a,
  SURROUND_MSG_SET_ANCHOR = 0x1b,
  SURROUND_MSG_SET_PLAYOUT_LATENCY = 0x1c,
  /* Realtime (type 96) ALAC: the UDP packets go through gw_rt_relay.c. */
  SURROUND_MSG_START_STREAM = 0x1d,      /* answered by START_RESULT */
  SURROUND_MSG_SET_CLIENT_CONTROL = 0x1e, /* NACKs go to the Gateway */
  SURROUND_MSG_REALTIME_FLUSH_TO_RTP = 0x1f,
  SURROUND_MSG_REALTIME_FLUSH_WAIT_ANCHOR = 0x20,
  SURROUND_MSG_REALTIME_ANCHOR = 0x21,

  /* Gateway -> speaker: link */
  SURROUND_MSG_PING = 0x30,
  SURROUND_MSG_TIME = 0x31,
  /* Gateway -> speaker: its own settings (Gateway web page) */
  SURROUND_MSG_SET_CONFIG = 0x32,
} surround_msg_type_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint8_t mac[6];
  int8_t channel;     /* aac_speaker_t the speaker plays, -1 = mono mix */
  char name[24];      /* device name, NUL-terminated */
  char firmware[24];  /* esp_app_desc_t version, NUL-terminated */
} surround_hello_t;

typedef struct __attribute__((packed)) {
  int32_t err;   /* esp_err_t of the start call */
  uint16_t port; /* buffered: the speaker's TCP audio port, 0 otherwise */
  uint16_t rt_data_port;    /* realtime: the speaker's UDP ports */
  uint16_t rt_control_port;
} surround_start_result_t;

typedef struct __attribute__((packed)) {
  uint16_t port; /* the Gateway's realtime control port (its IP is the link's) */
} surround_client_control_t;

typedef struct __attribute__((packed)) {
  uint32_t rtp;
} surround_realtime_flush_t;

/* A realtime SETRATEANCHORTIME in the sender's clock; the speaker maps it to
 * its own clock exactly as RTSP does on a standalone receiver. */
typedef struct __attribute__((packed)) {
  uint64_t clock_id;
  uint64_t remote_ptp_ns;
  uint32_t rtp_time;
} surround_realtime_anchor_t;

typedef struct __attribute__((packed)) {
  uint8_t stream_type; /* audio_stream_type_t */
} surround_stream_type_t;

typedef struct __attribute__((packed)) {
  char codec[16];
  int32_t sample_rate;
  int32_t channels;
  int32_t bits_per_sample;
  int32_t frame_size;
} surround_format_t;

typedef struct __attribute__((packed)) {
  uint8_t type; /* audio_encrypt_type_t */
  uint8_t key_len;
  uint8_t key[32];
  uint8_t iv[16];
} surround_encryption_t;

typedef struct __attribute__((packed)) {
  int32_t volume_q15;
} surround_volume_t;

typedef struct __attribute__((packed)) {
  uint32_t from_seq;
  uint32_t from_ts;
  uint32_t until_seq;
  uint32_t until_ts;
  uint8_t timestamps_valid;
} surround_deferred_flush_t;

typedef struct __attribute__((packed)) {
  uint32_t until_seq;
  uint32_t until_ts;
} surround_immediate_flush_t;

typedef struct __attribute__((packed)) {
  uint8_t playing;
} surround_playing_t;

typedef struct __attribute__((packed)) {
  uint64_t clock_id;
  uint64_t network_time_ns;
  uint32_t rtp_time;
} surround_anchor_t;

typedef struct __attribute__((packed)) {
  uint32_t latency_samples;
} surround_playout_latency_t;

#define SURROUND_TIME_PTP_VALID  0x01U /* ptp_ns and the clock ids are set */
#define SURROUND_TIME_PTP_LOCKED 0x02U /* the Gateway's PTP lock */
#define SURROUND_TIME_REALTIME   0x04U /* realtime-mode estimate (type 96) */

typedef struct __attribute__((packed)) {
  /* The Gateway's TSF (ns) at one instant, from its smoothed TSF fit. */
  int64_t tsf_ns;
  /* The sender's PTP time at the same instant (Gateway estimate). */
  int64_t ptp_ns;
  uint64_t source_clock_id;
  uint64_t grandmaster_clock_id;
  uint32_t mastership_age_ms; /* of the Gateway's PTP estimate */
  uint32_t sample_count;
  uint8_t bssid[6]; /* the access point whose TSF this is */
  uint8_t flags;    /* SURROUND_TIME_* */
} surround_time_t;

/* A speaker's own settings, kept in its NVS. The Gateway web page shows and
 * edits them; the speaker applies a change at once and answers CONFIG. */
#define SURROUND_TRIM_MIN_DB10  (-300) /* level trim, tenths of a dB */
#define SURROUND_TRIM_MAX_DB10  0
#define SURROUND_DELAY_MIN_US   0      /* extra delay, e.g. distance */
#define SURROUND_DELAY_MAX_US   50000

typedef struct __attribute__((packed)) {
  int8_t channel;      /* aac_speaker_t, -1 = mono mix */
  int16_t trim_db10;
  int32_t delay_us;
  uint8_t multichannel; /* built with CONFIG_AIRPLAY_MULTICHANNEL */
} surround_config_t;

#define SURROUND_SET_CHANNEL 0x01U
#define SURROUND_SET_TRIM    0x02U
#define SURROUND_SET_DELAY   0x04U

typedef struct __attribute__((packed)) {
  uint8_t fields; /* SURROUND_SET_*: which of the values below to apply */
  int8_t channel;
  int16_t trim_db10;
  int32_t delay_us;
} surround_set_config_t;

#define SURROUND_STATUS_ACTIVE    0x01U /* a stream is started */
#define SURROUND_STATUS_PLAYING   0x02U
#define SURROUND_STATUS_SYNC      0x04U /* sync_us/servo_ppm are valid */
#define SURROUND_STATUS_MC        0x08U /* multichannel decode: label valid */
#define SURROUND_STATUS_TIME      0x10U /* sender time from the Gateway */

/* What a speaker plays, for the Gateway web page. */
typedef struct __attribute__((packed)) {
  uint8_t flags;           /* SURROUND_STATUS_* */
  uint8_t stream_type;     /* SURROUND_STREAM_*, 0 = none */
  char codec[6];           /* "AAC", "ALAC" */
  uint32_t sample_rate;
  uint8_t bits;            /* bits per sample of the stream */
  uint8_t stream_channels; /* channels in the stream (8 = 7.1) */
  int8_t output_channel;   /* decoded channel played, -1 = mix */
  char label[10];          /* decoder's name for it, e.g. "front #1" */
  int32_t sync_us;
  int16_t servo_ppm;
  uint16_t fifo_used_kib;
  uint16_t fifo_capacity_kib;
  uint16_t tsf_rms_ns10;   /* TSF fit residual, units of 10 ns */
} surround_status_t;

static_assert(sizeof(surround_hello_t) <= SURROUND_MAX_PAYLOAD, "hello");
static_assert(sizeof(surround_status_t) <= SURROUND_MAX_PAYLOAD, "status");
static_assert(sizeof(surround_config_t) <= SURROUND_MAX_PAYLOAD, "config");
static_assert(sizeof(surround_time_t) <= SURROUND_MAX_PAYLOAD, "time");
static_assert(sizeof(surround_format_t) <= SURROUND_MAX_PAYLOAD, "format");
static_assert(sizeof(surround_encryption_t) <= SURROUND_MAX_PAYLOAD, "key");
static_assert(sizeof(surround_deferred_flush_t) <= SURROUND_MAX_PAYLOAD,
              "flush");
static_assert(sizeof(surround_anchor_t) <= SURROUND_MAX_PAYLOAD, "anchor");
