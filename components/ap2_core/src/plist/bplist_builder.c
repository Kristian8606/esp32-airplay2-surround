#include <math.h>
#include <string.h>

#include "ap2_plist.h"

/* A reply is described as a flat list of nodes in document order: a container
 * is followed by its contents, a dictionary by key, value, key, value. An
 * object's number in the file is its position in the list. */

typedef enum {
  NODE_DICT,
  NODE_ARRAY,
  NODE_STRING,
  NODE_DATA,
  NODE_INT,
  NODE_REAL,
  NODE_BOOL,
} node_kind_t;

typedef struct {
  node_kind_t kind;
  uint32_t len; /* entries of a container, bytes of data */
  union {
    const char *text;
    const uint8_t *bytes;
    int64_t integer;
    double real;
  } v;
} node_t;

#define N_DICT(entries)  {.kind = NODE_DICT, .len = (entries)}
#define N_ARRAY(entries) {.kind = NODE_ARRAY, .len = (entries)}
#define N_STR(s)         {.kind = NODE_STRING, .v.text = (s)}
#define N_INT(i)         {.kind = NODE_INT, .v.integer = (int64_t)(i)}
#define N_BOOL(b)        {.kind = NODE_BOOL, .v.integer = (b)}
#define N_REAL(r)        {.kind = NODE_REAL, .v.real = (r)}
#define N_DATA(p, n)     {.kind = NODE_DATA, .len = (uint32_t)(n), .v.bytes = (p)}

#define NODE_COUNT(list) (sizeof(list) / sizeof((list)[0]))

#define MAX_NODES       128
#define BP_TRAILER_LEN  32u
#define BP_MAX_OFFSET   0xFFFFu

typedef struct {
  uint8_t *out;
  size_t capacity;
  size_t pos;
  bool failed;
} writer_t;

static void put_bytes(writer_t *w, const void *bytes, size_t len) {
  if (w->failed || len > w->capacity - w->pos) {
    w->failed = true;
    return;
  }
  if (len) {
    memcpy(w->out + w->pos, bytes, len);
  }
  w->pos += len;
}

static void put_byte(writer_t *w, uint8_t value) {
  put_bytes(w, &value, 1);
}

static void put_be(writer_t *w, uint64_t value, size_t len) {
  for (size_t i = len; i > 0; i--) {
    put_byte(w, (uint8_t)(value >> (8u * (i - 1))));
  }
}

/* Marker with a count in its low nibble, or an integer object after it. */
static void put_marker(writer_t *w, uint8_t kind, size_t count) {
  if (count < 15) {
    put_byte(w, (uint8_t)((kind << 4) | count));
  } else if (count <= 0xFF) {
    put_byte(w, (uint8_t)((kind << 4) | 0x0F));
    put_byte(w, 0x10);
    put_byte(w, (uint8_t)count);
  } else {
    put_byte(w, (uint8_t)((kind << 4) | 0x0F));
    put_byte(w, 0x11);
    put_be(w, count, 2);
  }
}

static void put_integer(writer_t *w, int64_t value) {
  uint64_t bits = (uint64_t)value;
  if (value >= 0 && bits <= 0xFF) {
    put_byte(w, 0x10);
    put_be(w, bits, 1);
  } else if (value >= 0 && bits <= 0xFFFF) {
    put_byte(w, 0x11);
    put_be(w, bits, 2);
  } else if (value >= 0 && bits <= 0xFFFFFFFFu) {
    put_byte(w, 0x12);
    put_be(w, bits, 4);
  } else {
    put_byte(w, 0x13);
    put_be(w, bits, 8);
  }
}

static void put_real(writer_t *w, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  put_byte(w, 0x23);
  put_be(w, bits, 8);
}

/* Next code point of UTF-8 text; a bad byte reads as U+FFFD. */
static uint32_t next_code_point(const char **text) {
  const uint8_t *s = (const uint8_t *)*text;
  uint32_t cp = s[0];
  int extra = 0;

  if (cp >= 0xF0 && cp <= 0xF4) {
    cp &= 0x07;
    extra = 3;
  } else if (cp >= 0xE0 && cp < 0xF0) {
    cp &= 0x0F;
    extra = 2;
  } else if (cp >= 0xC2 && cp < 0xE0) {
    cp &= 0x1F;
    extra = 1;
  } else if (cp >= 0x80) {
    *text += 1;
    return 0xFFFD;
  }
  for (int i = 1; i <= extra; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *text += 1;
      return 0xFFFD;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if ((extra == 2 && (cp < 0x800 || (cp >= 0xD800 && cp < 0xE000))) ||
      (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF))) {
    *text += 1;
    return 0xFFFD;
  }
  *text += extra + 1;
  return cp;
}

static void put_string(writer_t *w, const char *text) {
  bool ascii = true;
  size_t units = 0;

  for (const char *s = text; *s;) {
    uint32_t cp = next_code_point(&s);
    ascii = ascii && cp < 0x80;
    units += cp >= 0x10000 ? 2 : 1;
  }
  if (ascii) {
    put_marker(w, 0x5, units);
    put_bytes(w, text, units);
    return;
  }
  put_marker(w, 0x6, units);
  for (const char *s = text; *s;) {
    uint32_t cp = next_code_point(&s);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put_be(w, 0xD800 + (cp >> 10), 2);
      put_be(w, 0xDC00 + (cp & 0x3FF), 2);
    } else {
      put_be(w, cp, 2);
    }
  }
}

/* Position of the node after this one and everything inside it. */
static size_t skip_node(const node_t *nodes, size_t at) {
  size_t next = at + 1;
  if (nodes[at].kind == NODE_ARRAY) {
    for (uint32_t i = 0; i < nodes[at].len; i++) {
      next = skip_node(nodes, next);
    }
  } else if (nodes[at].kind == NODE_DICT) {
    for (uint32_t i = 0; i < 2u * nodes[at].len; i++) {
      next = skip_node(nodes, next);
    }
  }
  return next;
}

static void put_container(writer_t *w, const node_t *nodes, size_t at) {
  /* Object references are one byte: MAX_NODES keeps numbers below 256. */
  size_t child = at + 1;

  if (nodes[at].kind == NODE_ARRAY) {
    put_marker(w, 0xA, nodes[at].len);
    for (uint32_t i = 0; i < nodes[at].len; i++) {
      put_byte(w, (uint8_t)child);
      child = skip_node(nodes, child);
    }
    return;
  }
  put_marker(w, 0xD, nodes[at].len);
  /* Keys first, then values, with each key directly before its value. */
  size_t first = child;
  for (uint32_t i = 0; i < nodes[at].len; i++) {
    put_byte(w, (uint8_t)child);
    child = skip_node(nodes, child);
    child = skip_node(nodes, child);
  }
  child = first;
  for (uint32_t i = 0; i < nodes[at].len; i++) {
    child = skip_node(nodes, child);
    put_byte(w, (uint8_t)child);
    child = skip_node(nodes, child);
  }
}

static size_t write_plist(uint8_t *out, size_t capacity, const node_t *nodes,
                          size_t count) {
  uint16_t offsets[MAX_NODES];
  writer_t w = {.out = out, .capacity = capacity};

  if (!out || count > MAX_NODES) {
    return 0;
  }
  put_bytes(&w, "bplist00", 8);
  for (size_t i = 0; i < count && !w.failed; i++) {
    if (w.pos > BP_MAX_OFFSET) {
      return 0;
    }
    offsets[i] = (uint16_t)w.pos;
    switch (nodes[i].kind) {
    case NODE_DICT:
    case NODE_ARRAY:
      put_container(&w, nodes, i);
      break;
    case NODE_STRING:
      put_string(&w, nodes[i].v.text);
      break;
    case NODE_DATA:
      put_marker(&w, 0x4, nodes[i].len);
      put_bytes(&w, nodes[i].v.bytes, nodes[i].len);
      break;
    case NODE_INT:
      put_integer(&w, nodes[i].v.integer);
      break;
    case NODE_BOOL:
      put_byte(&w, nodes[i].v.integer ? 0x09 : 0x08);
      break;
    case NODE_REAL:
      put_real(&w, nodes[i].v.real);
      break;
    }
  }

  size_t table_start = w.pos;
  size_t offset_size = offsets[count - 1] > 0xFF ? 2 : 1;
  for (size_t i = 0; i < count; i++) {
    put_be(&w, offsets[i], offset_size);
  }
  /* Trailer: six unused bytes, then the sizes, the count, the root, the table. */
  for (int i = 0; i < 6; i++) {
    put_byte(&w, 0);
  }
  put_byte(&w, (uint8_t)offset_size);
  put_byte(&w, 1);
  put_be(&w, count, 8);
  put_be(&w, 0, 8);
  put_be(&w, table_start, 8);
  return w.failed ? 0 : w.pos;
}

size_t ap2_reply_setup_session(uint8_t *out, size_t capacity,
                               uint16_t event_port) {
  const node_t doc[] = {
      N_DICT(2),
      N_STR("eventPort"), N_INT(event_port),
      N_STR("timingPort"), N_INT(0),
  };
  return write_plist(out, capacity, doc, NODE_COUNT(doc));
}

size_t ap2_reply_setup_stream(uint8_t *out, size_t capacity,
                              int64_t stream_type, uint16_t data_port,
                              uint16_t control_port,
                              uint32_t audio_buffer_size) {
  if (stream_type == 103) {
    const node_t doc[] = {
        N_DICT(1),
        N_STR("streams"), N_ARRAY(1),
        N_DICT(4),
        N_STR("type"), N_INT(stream_type),
        N_STR("dataPort"), N_INT(data_port),
        N_STR("audioBufferSize"), N_INT(audio_buffer_size),
        N_STR("controlPort"), N_INT(control_port),
    };
    return write_plist(out, capacity, doc, NODE_COUNT(doc));
  }
  const node_t doc[] = {
      N_DICT(1),
      N_STR("streams"), N_ARRAY(1),
      N_DICT(3),
      N_STR("type"), N_INT(stream_type),
      N_STR("dataPort"), N_INT(data_port),
      N_STR("controlPort"), N_INT(control_port),
  };
  return write_plist(out, capacity, doc, NODE_COUNT(doc));
}

size_t ap2_reply_feedback(uint8_t *out, size_t capacity,
                          int64_t stream_type, double sample_rate) {
  const node_t doc[] = {
      N_DICT(1),
      N_STR("streams"), N_ARRAY(1),
      N_DICT(2),
      N_STR("type"), N_INT(stream_type),
      N_STR("sr"), N_REAL(sample_rate),
  };
  return write_plist(out, capacity, doc, NODE_COUNT(doc));
}

size_t ap2_reply_info(uint8_t *out, size_t capacity, const ap2_info_t *info) {
  if (!info || !info->device_id || !info->name || !info->model ||
      !info->source_version || !info->protocol_versions || !info->pairing_id ||
      !info->public_key || !isfinite(info->initial_volume)) {
    return 0;
  }

  const bool homekit = info->manufacturer && info->serial_number &&
                       info->firmware_revision;
  const node_t doc[] = {
      N_DICT(homekit ? 25 : 14),
      N_STR("deviceid"), N_STR(info->device_id),
      N_STR("features"), N_INT(info->features),
      N_STR("model"), N_STR(info->model),
      N_STR("protovers"), N_STR(info->protocol_versions),
      N_STR("srcvers"), N_STR(info->source_version),
      N_STR("vv"), N_INT(info->protocol_version),
      N_STR("statusFlags"), N_INT(info->status_flags),
      N_STR("pk"), N_DATA(info->public_key, info->public_key_len),
      N_STR("pi"), N_STR(info->pairing_id),
      N_STR("name"), N_STR(info->name),
      N_STR("audioFormats"), N_ARRAY(1),
      N_DICT(3),
      N_STR("type"), N_INT(96),
      N_STR("audioInputFormats"), N_INT(0x01000000),
      N_STR("audioOutputFormats"), N_INT(0x01000000),
      N_STR("audioLatencies"), N_ARRAY(2),
      N_DICT(4),
      N_STR("type"), N_INT(96),
      N_STR("audioType"), N_INT(100),
      N_STR("inputLatencyMicros"), N_INT(0),
      N_STR("outputLatencyMicros"), N_INT(0),
      N_DICT(4),
      N_STR("type"), N_INT(103),
      N_STR("audioType"), N_INT(100),
      N_STR("inputLatencyMicros"), N_INT(0),
      N_STR("outputLatencyMicros"), N_INT(0),
      N_STR("initialVolume"), N_REAL(info->initial_volume),
      N_STR("supportedFormats"), N_DICT(2),
      N_STR("audioStream"), N_INT(info->audio_stream_formats),
      N_STR("bufferStream"), N_INT(info->buffer_stream_formats),
      N_STR("deviceID"), N_STR(info->device_id),
      N_STR("manufacturer"), N_STR(info->manufacturer),
      N_STR("serialNumber"), N_STR(info->serial_number),
      N_STR("firmwareRevision"), N_STR(info->firmware_revision),
      N_STR("build"), N_STR(info->firmware_revision),
      N_STR("sourceVersion"), N_STR(info->source_version),
      N_STR("protocolVersion"), N_STR(info->protocol_versions),
      N_STR("keepAliveLowPower"), N_BOOL(true),
      N_STR("keepAliveSendStatsAsBody"), N_BOOL(true),
      N_STR("sdk"), N_STR("AirPlay;2.0.2"),
      N_STR("firmwareBuildDate"), N_STR(__DATE__),
  };
  return write_plist(out, capacity, doc, NODE_COUNT(doc) - (homekit ? 0 : 22));
}

size_t ap2_reply_configure(uint8_t *out, size_t capacity, const char *identifier,
                            const uint8_t public_key[32], const char *name,
                            bool hk_access_control) {
  if (!identifier || !public_key || !name) return 0;
  const node_t doc[] = {
      N_DICT(5),
      N_STR("Identifier"), N_STR(identifier),
      N_STR("PublicKey"), N_DATA(public_key, 32),
      N_STR("Device_Name"), N_STR(name),
      N_STR("Enable_HK_Access_Control"), N_BOOL(hk_access_control),
      /* Playback stays open, as advertised by acl=0. */
      N_STR("Access_Control_Level"), N_INT(0),
  };
  return write_plist(out, capacity, doc, NODE_COUNT(doc));
}

size_t ap2_event_command(uint8_t *out, size_t capacity,
                         uint8_t command) {
  if (command > 5) {
    return 0;
  }
  const node_t doc[] = {
      N_DICT(2),
      N_STR("type"), N_STR("sendMediaRemoteCommand"),
      N_STR("modernMediaRemoteCommand"), N_INT(command),
  };
  return write_plist(out, capacity, doc, NODE_COUNT(doc));
}

size_t ap2_event_volume(uint8_t *out, size_t capacity,
                        double volume) {
  if (!isfinite(volume) || volume < 0.0 || volume > 1.0) {
    return 0;
  }
  const node_t doc[] = {
      N_DICT(4),
      N_STR("type"), N_STR("sendMediaRemoteCommand"),
      N_STR("value"), N_STR("dvlc"),
      N_STR("volume"), N_REAL(volume),
      N_STR("params"), N_DICT(1),
      N_STR("volume"), N_REAL(volume),
  };
  return write_plist(out, capacity, doc, NODE_COUNT(doc));
}
