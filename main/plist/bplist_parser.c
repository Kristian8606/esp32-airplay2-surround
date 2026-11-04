#include <string.h>

#include "plist.h"

/* Layout of a binary plist: "bplist00", the objects, a table with the offset
 * of each object, and a 32-byte trailer. Containers refer to objects by their
 * number in that table, so a malformed file can point anywhere and can form
 * cycles: everything below is range-checked, and walking is limited. */

#define BP_HEADER_LEN  8u
#define BP_TRAILER_LEN 32u
#define BP_MAX_DEPTH   16
#define BP_MAX_VISITS  2048
#define BP_MAX_REAL_RATE 2147483647.0

enum {
  BP_KIND_SIMPLE = 0x0,
  BP_KIND_INT = 0x1,
  BP_KIND_REAL = 0x2,
  BP_KIND_DATA = 0x4,
  BP_KIND_ASCII = 0x5,
  BP_KIND_UTF16 = 0x6,
  BP_KIND_UID = 0x8,
  BP_KIND_ARRAY = 0xA,
  BP_KIND_DICT = 0xD,
};

typedef struct {
  const uint8_t *bytes;
  size_t table_start;
  uint64_t object_count;
  uint64_t root;
  uint8_t offset_size;
  uint8_t ref_size;
} bp_doc_t;

typedef struct {
  uint8_t kind;
  uint8_t low;
  uint64_t count;       /* elements: bytes, characters, entries */
  const uint8_t *body;
} bp_obj_t;

static uint64_t read_be(const uint8_t *bytes, size_t len) {
  uint64_t value = 0;
  for (size_t i = 0; i < len; i++) {
    value = (value << 8) | bytes[i];
  }
  return value;
}

static bool bp_open(bp_doc_t *doc, const uint8_t *plist, size_t len) {
  if (!plist || len < BP_HEADER_LEN + BP_TRAILER_LEN ||
      memcmp(plist, "bplist00", BP_HEADER_LEN) != 0) {
    return false;
  }
  const uint8_t *trailer = plist + len - BP_TRAILER_LEN;
  size_t end = len - BP_TRAILER_LEN;
  uint64_t table = read_be(trailer + 24, 8);

  doc->bytes = plist;
  doc->offset_size = trailer[6];
  doc->ref_size = trailer[7];
  doc->object_count = read_be(trailer + 8, 8);
  doc->root = read_be(trailer + 16, 8);

  if (doc->offset_size < 1 || doc->offset_size > 8 || doc->ref_size < 1 ||
      doc->ref_size > 8 || doc->object_count == 0 ||
      doc->root >= doc->object_count || table < BP_HEADER_LEN || table > end ||
      doc->object_count > (end - table) / doc->offset_size) {
    return false;
  }
  doc->table_start = (size_t)table;
  return true;
}

/* Bytes of an object's body, or false if the count makes it impossible. */
static bool body_size(const bp_doc_t *doc, const bp_obj_t *obj, uint64_t *size) {
  uint64_t unit;

  switch (obj->kind) {
  case BP_KIND_DATA:
  case BP_KIND_ASCII:
    unit = 1;
    break;
  case BP_KIND_UTF16:
    unit = 2;
    break;
  case BP_KIND_ARRAY:
    unit = doc->ref_size;
    break;
  case BP_KIND_DICT:
    unit = 2u * doc->ref_size;
    break;
  default:
    unit = 1;
    break;
  }
  if (obj->count > UINT64_MAX / unit) {
    return false;
  }
  *size = obj->count * unit;
  return true;
}

static bool bp_get(const bp_doc_t *doc, uint64_t number, bp_obj_t *obj) {
  if (number >= doc->object_count) {
    return false;
  }
  uint64_t offset = read_be(doc->bytes + doc->table_start +
                                (size_t)number * doc->offset_size,
                            doc->offset_size);
  size_t end = doc->table_start;
  if (offset < BP_HEADER_LEN || offset >= end) {
    return false;
  }

  size_t pos = (size_t)offset;
  uint8_t marker = doc->bytes[pos++];
  obj->kind = marker >> 4;
  obj->low = marker & 0x0F;
  obj->count = obj->low;

  switch (obj->kind) {
  case BP_KIND_SIMPLE:
    obj->count = 0;
    break;
  case BP_KIND_INT:
  case BP_KIND_REAL:
    if (obj->low > 4) {
      return false;
    }
    obj->count = 1ull << obj->low;
    break;
  case 0x3: /* date */
    obj->count = 8;
    break;
  case BP_KIND_UID:
    obj->count = (uint64_t)obj->low + 1;
    break;
  case BP_KIND_DATA:
  case BP_KIND_ASCII:
  case BP_KIND_UTF16:
  case BP_KIND_ARRAY:
  case 0xC: /* set */
  case BP_KIND_DICT:
    if (obj->low == 0x0F) {
      /* A longer count follows as an integer object. */
      if (pos >= end || (doc->bytes[pos] >> 4) != BP_KIND_INT ||
          (doc->bytes[pos] & 0x0F) > 3) {
        return false;
      }
      size_t count_len = 1u << (doc->bytes[pos] & 0x0F);
      pos++;
      if (end - pos < count_len) {
        return false;
      }
      obj->count = read_be(doc->bytes + pos, count_len);
      pos += count_len;
    }
    break;
  default:
    return false;
  }

  uint64_t size;
  if (!body_size(doc, obj, &size) || size > end - pos) {
    return false;
  }
  obj->body = doc->bytes + pos;
  return true;
}

static uint64_t bp_ref(const bp_doc_t *doc, const uint8_t *refs, uint64_t i) {
  return read_be(refs + (size_t)i * doc->ref_size, doc->ref_size);
}

static bool bp_dict_entry(const bp_doc_t *doc, const bp_obj_t *dict, uint64_t i,
                          bp_obj_t *key, bp_obj_t *value) {
  const uint8_t *key_refs = dict->body;
  const uint8_t *value_refs = dict->body + (size_t)dict->count * doc->ref_size;
  return bp_get(doc, bp_ref(doc, key_refs, i), key) &&
         bp_get(doc, bp_ref(doc, value_refs, i), value);
}

static bool key_equals(const bp_obj_t *key, const char *text, size_t text_len) {
  if (key->count != text_len) {
    return false;
  }
  if (key->kind == BP_KIND_ASCII) {
    return memcmp(key->body, text, text_len) == 0;
  }
  if (key->kind == BP_KIND_UTF16) {
    for (size_t i = 0; i < text_len; i++) {
      if (key->body[2 * i] != 0 ||
          key->body[2 * i + 1] != (uint8_t)text[i]) {
        return false;
      }
    }
    return true;
  }
  return false;
}

/* The first entry with this key. A broken entry before it stops the search. */
static bool bp_dict_find(const bp_doc_t *doc, const bp_obj_t *dict,
                         const char *key_text, bp_obj_t *value) {
  size_t key_len = strlen(key_text);
  for (uint64_t i = 0; i < dict->count; i++) {
    bp_obj_t key;
    if (!bp_dict_entry(doc, dict, i, &key, value)) {
      return false;
    }
    if (key_equals(&key, key_text, key_len)) {
      return true;
    }
  }
  return false;
}

static bool open_root(bp_doc_t *doc, bp_obj_t *root, const uint8_t *plist,
                      size_t plist_len) {
  return bp_open(doc, plist, plist_len) && bp_get(doc, doc->root, root) &&
         root->kind == BP_KIND_DICT;
}

static bool read_int(const bp_obj_t *obj, int64_t *value) {
  /* Lengths under 8 bytes are unsigned, 8 bytes signed. */
  if (obj->kind != BP_KIND_INT || obj->low > 3) {
    return false;
  }
  *value = (int64_t)read_be(obj->body, (size_t)obj->count);
  return true;
}

static bool read_real(const bp_obj_t *obj, double *value) {
  if (obj->kind != BP_KIND_REAL) {
    return false;
  }
  if (obj->low == 3) {
    uint64_t bits = read_be(obj->body, 8);
    memcpy(value, &bits, sizeof(*value));
    return true;
  }
  if (obj->low == 2) {
    uint32_t bits = (uint32_t)read_be(obj->body, 4);
    float single;
    memcpy(&single, &bits, sizeof(single));
    *value = single;
    return true;
  }
  return false;
}

bool bplist_find_int(const uint8_t *plist, size_t plist_len, const char *key,
                     int64_t *out_value) {
  bp_doc_t doc;
  bp_obj_t root, value;
  return open_root(&doc, &root, plist, plist_len) &&
         bp_dict_find(&doc, &root, key, &value) && read_int(&value, out_value);
}

bool bplist_find_real(const uint8_t *plist, size_t plist_len, const char *key,
                      double *out_value) {
  bp_doc_t doc;
  bp_obj_t root, value;
  int64_t whole;

  if (!open_root(&doc, &root, plist, plist_len) ||
      !bp_dict_find(&doc, &root, key, &value)) {
    return false;
  }
  if (read_real(&value, out_value)) {
    return true;
  }
  if (read_int(&value, &whole)) {
    *out_value = (double)whole;
    return true;
  }
  return false;
}

bool bplist_find_string(const uint8_t *plist, size_t plist_len, const char *key,
                        char *out_str, size_t out_capacity) {
  bp_doc_t doc;
  bp_obj_t root, value;

  if (!open_root(&doc, &root, plist, plist_len) ||
      !bp_dict_find(&doc, &root, key, &value) ||
      value.kind != BP_KIND_ASCII || value.count >= out_capacity) {
    return false;
  }
  memcpy(out_str, value.body, (size_t)value.count);
  out_str[value.count] = '\0';
  return true;
}

static bool copy_data(const bp_obj_t *value, uint8_t *out, size_t capacity,
                      size_t *out_len) {
  if (value->kind != BP_KIND_DATA || value->count > capacity) {
    return false;
  }
  if (value->count) {
    memcpy(out, value->body, (size_t)value->count);
  }
  *out_len = (size_t)value->count;
  return true;
}

typedef struct {
  const bp_doc_t *doc;
  const char *key;
  size_t key_len;
  uint8_t *out;
  size_t capacity;
  size_t *out_len;
  int visits;
} bp_search_t;

static bool search_value(bp_search_t *s, const bp_obj_t *obj, int depth);

static bool search_dict(bp_search_t *s, const bp_obj_t *dict, int depth) {
  bp_obj_t key, value;

  for (uint64_t i = 0; i < dict->count; i++) {
    if (bp_dict_entry(s->doc, dict, i, &key, &value) &&
        key_equals(&key, s->key, s->key_len) &&
        copy_data(&value, s->out, s->capacity, s->out_len)) {
      return true;
    }
  }
  for (uint64_t i = 0; i < dict->count; i++) {
    if (bp_dict_entry(s->doc, dict, i, &key, &value) &&
        search_value(s, &value, depth + 1)) {
      return true;
    }
  }
  return false;
}

static bool search_value(bp_search_t *s, const bp_obj_t *obj, int depth) {
  if (depth > BP_MAX_DEPTH || ++s->visits > BP_MAX_VISITS) {
    return false;
  }
  if (obj->kind == BP_KIND_DICT) {
    return search_dict(s, obj, depth);
  }
  if (obj->kind == BP_KIND_ARRAY) {
    for (uint64_t i = 0; i < obj->count; i++) {
      bp_obj_t item;
      if (bp_get(s->doc, bp_ref(s->doc, obj->body, i), &item) &&
          search_value(s, &item, depth + 1)) {
        return true;
      }
    }
  }
  return false;
}

bool bplist_find_data_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, uint8_t *out_data,
                           size_t out_capacity, size_t *out_len) {
  bp_doc_t doc;
  bp_obj_t root, value;

  if (!open_root(&doc, &root, plist, plist_len)) {
    return false;
  }
  if (bp_dict_find(&doc, &root, key, &value)) {
    return copy_data(&value, out_data, out_capacity, out_len);
  }

  bp_search_t search = {&doc, key, strlen(key), out_data, out_capacity, out_len, 0};
  return search_dict(&search, &root, 0);
}

static bool get_streams(bp_doc_t *doc, bp_obj_t *streams, const uint8_t *plist,
                        size_t plist_len) {
  bp_obj_t root;
  return open_root(doc, &root, plist, plist_len) &&
         bp_dict_find(doc, &root, "streams", streams) &&
         streams->kind == BP_KIND_ARRAY;
}

static bool get_stream(bp_doc_t *doc, bp_obj_t *stream, const uint8_t *plist,
                       size_t plist_len, size_t index) {
  bp_obj_t streams;
  return get_streams(doc, &streams, plist, plist_len) &&
         index < streams.count &&
         bp_get(doc, bp_ref(doc, streams.body, index), stream) &&
         stream->kind == BP_KIND_DICT;
}

bool bplist_get_streams_count(const uint8_t *plist, size_t plist_len,
                              size_t *count) {
  bp_doc_t doc;
  bp_obj_t streams;

  if (!get_streams(&doc, &streams, plist, plist_len)) {
    return false;
  }
  *count = (size_t)streams.count;
  return true;
}

static size_t data_len_of(const bp_doc_t *doc, const bp_obj_t *dict,
                          const char *key) {
  bp_obj_t value;
  if (bp_dict_find(doc, dict, key, &value) && value.kind == BP_KIND_DATA) {
    return (size_t)value.count;
  }
  return 0;
}

bool bplist_get_stream_info(const uint8_t *plist, size_t plist_len,
                            size_t index, int64_t *type, size_t *ekey_len,
                            size_t *eiv_len, size_t *shk_len) {
  bp_doc_t doc;
  bp_obj_t stream, value;

  *type = -1;
  *ekey_len = 0;
  *eiv_len = 0;
  *shk_len = 0;
  if (!get_stream(&doc, &stream, plist, plist_len, index) ||
      !bp_dict_find(&doc, &stream, "type", &value) ||
      !read_int(&value, type)) {
    return false;
  }
  *ekey_len = data_len_of(&doc, &stream, "ekey");
  *eiv_len = data_len_of(&doc, &stream, "eiv");
  *shk_len = data_len_of(&doc, &stream, "shk");
  return true;
}

/* Copies a key as ASCII. Returns 1 if copied, 0 if it should be left out and
 * -1 if the key cannot be represented. */
static int copy_key(const bp_obj_t *key, char out[64]) {
  if (key->kind != BP_KIND_ASCII && key->kind != BP_KIND_UTF16) {
    return 0;
  }
  if (key->count >= 64) {
    return 0;
  }
  size_t len = (size_t)key->count;
  if (key->kind == BP_KIND_ASCII) {
    memcpy(out, key->body, len);
  } else {
    for (size_t i = 0; i < len; i++) {
      if (key->body[2 * i] != 0 || key->body[2 * i + 1] >= 0x80) {
        return -1;
      }
      out[i] = (char)key->body[2 * i + 1];
    }
  }
  out[len] = '\0';
  return 1;
}

static void summarize(const bp_obj_t *value, bplist_kv_info_t *info) {
  info->value_type = BPLIST_VALUE_UNKNOWN;
  info->value_len = 0;
  info->int_value = 0;
  switch (value->kind) {
  case BP_KIND_INT:
    info->value_type = BPLIST_VALUE_INT;
    (void)read_int(value, &info->int_value);
    break;
  case BP_KIND_DATA:
    info->value_type = BPLIST_VALUE_DATA;
    info->value_len = (size_t)value->count;
    break;
  case BP_KIND_ASCII:
  case BP_KIND_UTF16:
    info->value_type = BPLIST_VALUE_STRING;
    info->value_len = (size_t)value->count;
    break;
  case BP_KIND_UID:
    info->value_type = BPLIST_VALUE_UID;
    break;
  case BP_KIND_ARRAY:
    info->value_type = BPLIST_VALUE_ARRAY;
    break;
  case BP_KIND_DICT:
    info->value_type = BPLIST_VALUE_DICT;
    break;
  default:
    break;
  }
}

bool bplist_get_stream_kv_info(const uint8_t *plist, size_t plist_len,
                               size_t index, bplist_kv_info_t *out,
                               size_t out_capacity, size_t *out_count) {
  bp_doc_t doc;
  bp_obj_t stream;
  size_t stored = 0;

  *out_count = 0;
  if (!get_stream(&doc, &stream, plist, plist_len, index) ||
      stream.count == 0) {
    return false;
  }
  for (uint64_t i = 0; i < stream.count; i++) {
    bp_obj_t key, value;
    char key_text[64];

    if (!bp_dict_entry(&doc, &stream, i, &key, &value)) {
      return false;
    }
    int copied = copy_key(&key, key_text);
    if (copied < 0) {
      return false;
    }
    if (copied == 0) {
      continue;
    }
    /* Senders may send the sample rate as a real. */
    double rate = 0;
    bool real_rate = strcmp(key_text, "sr") == 0 && read_real(&value, &rate);
    if (real_rate && !(rate >= 1.0 && rate <= BP_MAX_REAL_RATE &&
                       (double)(int64_t)rate == rate)) {
      return false;
    }
    if (stored == out_capacity) {
      return false;
    }
    memcpy(out[stored].key, key_text, sizeof(key_text));
    summarize(&value, &out[stored]);
    if (real_rate) {
      out[stored].value_type = BPLIST_VALUE_INT;
      out[stored].int_value = (int64_t)rate;
    }
    stored++;
  }
  *out_count = stored;
  return true;
}

static bool copy_field(const bp_doc_t *doc, const bp_obj_t *dict,
                       const char *key, uint8_t *out, size_t capacity,
                       size_t *out_len) {
  bp_obj_t value;

  *out_len = 0;
  return out && bp_dict_find(doc, dict, key, &value) &&
         copy_data(&value, out, capacity, out_len) && *out_len > 0;
}

bool bplist_find_stream_crypto(const uint8_t *plist, size_t plist_len,
                               int64_t stream_type, uint8_t *ekey,
                               size_t ekey_capacity, size_t *ekey_len,
                               uint8_t *eiv, size_t eiv_capacity,
                               size_t *eiv_len, uint8_t *shk,
                               size_t shk_capacity, size_t *shk_len) {
  bp_doc_t doc;
  bp_obj_t streams;

  *ekey_len = 0;
  *eiv_len = 0;
  *shk_len = 0;
  if (!get_streams(&doc, &streams, plist, plist_len)) {
    return false;
  }
  for (uint64_t i = 0; i < streams.count; i++) {
    bp_obj_t stream, value;
    int64_t type;

    if (!bp_get(&doc, bp_ref(&doc, streams.body, i), &stream) ||
        stream.kind != BP_KIND_DICT ||
        !bp_dict_find(&doc, &stream, "type", &value) ||
        !read_int(&value, &type) || type != stream_type) {
      continue;
    }
    bool found = copy_field(&doc, &stream, "ekey", ekey, ekey_capacity, ekey_len);
    found |= copy_field(&doc, &stream, "eiv", eiv, eiv_capacity, eiv_len);
    found |= copy_field(&doc, &stream, "shk", shk, shk_capacity, shk_len);
    return found;
  }
  return false;
}
