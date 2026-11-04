#pragma once

/* The names the RTSP code (main/rtsp) calls, mapped to ap2_core. It goes
 * away when the RTSP code is replaced. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ap2_hap.h"
#include "ap2_plist.h"
#include "ap2_tlv8.h"
#include "esp_err.h"

/* ---- Pairing ---- */

typedef ap2_hap_session_t hap_session_t;

#define PAIR_VERIFY_STATE_M2 2
#define PAIR_VERIFY_STATE_M4 4
#define TLV_TYPE_STATE       AP2_TLV_STATE

static inline hap_session_t *hap_session_create(void) {
  return ap2_hap_session_new();
}

static inline void hap_session_free(hap_session_t *session) {
  ap2_hap_session_delete(session);
}

static inline const uint8_t *hap_get_public_key(void) {
  return ap2_hap_identity_public_key();
}

static inline esp_err_t hap_pair_setup_m1(hap_session_t *s, const uint8_t *in,
                                          size_t in_len, uint8_t *out,
                                          size_t cap, size_t *out_len) {
  return ap2_hap_setup_m1(s, in, in_len, out, cap, out_len);
}

static inline esp_err_t hap_pair_setup_m3(hap_session_t *s, const uint8_t *in,
                                          size_t in_len, uint8_t *out,
                                          size_t cap, size_t *out_len) {
  return ap2_hap_setup_m3(s, in, in_len, out, cap, out_len);
}

static inline esp_err_t hap_pair_setup_m5(hap_session_t *s, const uint8_t *in,
                                          size_t in_len, uint8_t *out,
                                          size_t cap, size_t *out_len) {
  return ap2_hap_setup_m5(s, in, in_len, out, cap, out_len);
}

static inline esp_err_t hap_pair_verify_m1(hap_session_t *s, const uint8_t *in,
                                           size_t in_len, uint8_t *out,
                                           size_t cap, size_t *out_len) {
  return ap2_hap_verify_m1(s, in, in_len, out, cap, out_len);
}

static inline esp_err_t hap_pair_verify_m3(hap_session_t *s, const uint8_t *in,
                                           size_t in_len, uint8_t *out,
                                           size_t cap, size_t *out_len) {
  return ap2_hap_verify_m3(s, in, in_len, out, cap, out_len);
}

static inline esp_err_t hap_pair_verify_m1_raw(hap_session_t *s,
                                               const uint8_t *in,
                                               size_t in_len, uint8_t *out,
                                               size_t cap, size_t *out_len) {
  return ap2_hap_verify_raw_m1(s, in, in_len, out, cap, out_len);
}

static inline esp_err_t hap_pair_verify_m3_raw(hap_session_t *s,
                                               const uint8_t *in,
                                               size_t in_len, uint8_t *out,
                                               size_t cap, size_t *out_len) {
  return ap2_hap_verify_raw_m3(s, in, in_len, out, cap, out_len);
}

static inline esp_err_t hap_derive_audio_key(hap_session_t *s, uint8_t *key,
                                             size_t key_len) {
  return ap2_hap_audio_key(s, key, key_len);
}

static inline int hap_hkdf_sha512(const uint8_t *salt, size_t salt_len,
                                  const uint8_t *ikm, size_t ikm_len,
                                  const uint8_t *info, size_t info_len,
                                  uint8_t *okm, size_t okm_len) {
  return ap2_hkdf_sha512(salt, salt_len, ikm, ikm_len, info, info_len, okm,
                         okm_len);
}

static inline const uint8_t *tlv8_find(const uint8_t *data, size_t data_len,
                                       uint8_t type, size_t *value_len) {
  return ap2_tlv8_find(data, data_len, type, value_len);
}

/* ---- XML plist ---- */

typedef ap2_xml_t plist_t;

static inline void plist_init(plist_t *p, char *buffer, size_t capacity) {
  ap2_xml_init(p, buffer, capacity);
}

static inline void plist_begin(plist_t *p) {
  ap2_xml_begin(p);
}

static inline void plist_dict_begin(plist_t *p) {
  ap2_xml_dict_begin(p);
}

static inline void plist_dict_string(plist_t *p, const char *key,
                                     const char *value) {
  ap2_xml_string(p, key, value);
}

static inline void plist_dict_int(plist_t *p, const char *key, int64_t value) {
  ap2_xml_int(p, key, value);
}

static inline void plist_dict_uint(plist_t *p, const char *key,
                                   uint64_t value) {
  ap2_xml_uint(p, key, value);
}

static inline void plist_dict_data(plist_t *p, const char *key,
                                   const uint8_t *data, size_t len) {
  ap2_xml_data(p, key, data, len);
}

static inline void plist_dict_end(plist_t *p) {
  ap2_xml_dict_end(p);
}

static inline void plist_dict_array_begin(plist_t *p, const char *key) {
  ap2_xml_array_begin(p, key);
}

static inline void plist_dict_dict_begin(plist_t *p, const char *key) {
  ap2_xml_child_dict_begin(p, key);
}

static inline void plist_array_end(plist_t *p) {
  ap2_xml_array_end(p);
}

static inline size_t plist_end(plist_t *p) {
  return ap2_xml_end(p);
}

/* ---- Binary plist ---- */

typedef ap2_bplist_key_t bplist_kv_info_t;

#define BPLIST_VALUE_INT AP2_BPLIST_INT

static inline bool bplist_find_data_deep(const uint8_t *plist, size_t len,
                                         const char *key, uint8_t *out,
                                         size_t cap, size_t *out_len) {
  return ap2_bplist_data_deep(plist, len, key, out, cap, out_len);
}

static inline bool bplist_find_int(const uint8_t *plist, size_t len,
                                   const char *key, int64_t *out) {
  return ap2_bplist_int(plist, len, key, out);
}

static inline bool bplist_find_real(const uint8_t *plist, size_t len,
                                    const char *key, double *out) {
  return ap2_bplist_real(plist, len, key, out);
}

static inline bool bplist_find_string(const uint8_t *plist, size_t len,
                                      const char *key, char *out,
                                      size_t cap) {
  return ap2_bplist_string(plist, len, key, out, cap);
}

static inline bool bplist_get_streams_count(const uint8_t *plist, size_t len,
                                            size_t *count) {
  return ap2_bplist_stream_count(plist, len, count);
}

static inline bool bplist_get_stream_info(const uint8_t *plist, size_t len,
                                          size_t index, int64_t *type,
                                          size_t *ekey_len, size_t *eiv_len,
                                          size_t *shk_len) {
  return ap2_bplist_stream_info(plist, len, index, type, ekey_len, eiv_len,
                                shk_len);
}

static inline bool bplist_get_stream_kv_info(const uint8_t *plist, size_t len,
                                             size_t index,
                                             bplist_kv_info_t *out,
                                             size_t cap, size_t *count) {
  return ap2_bplist_stream_keys(plist, len, index, out, cap, count);
}

static inline bool bplist_find_stream_crypto(
    const uint8_t *plist, size_t len, int64_t stream_type, uint8_t *ekey,
    size_t ekey_cap, size_t *ekey_len, uint8_t *eiv, size_t eiv_cap,
    size_t *eiv_len, uint8_t *shk, size_t shk_cap, size_t *shk_len) {
  return ap2_bplist_stream_crypto(plist, len, stream_type, ekey, ekey_cap,
                                  ekey_len, eiv, eiv_cap, eiv_len, shk,
                                  shk_cap, shk_len);
}

static inline size_t bplist_build_initial_setup(uint8_t *out, size_t cap,
                                                uint16_t event_port) {
  return ap2_reply_setup_session(out, cap, event_port);
}

static inline size_t bplist_build_stream_setup(uint8_t *out, size_t cap,
                                               int64_t stream_type,
                                               uint16_t data_port,
                                               uint16_t control_port,
                                               uint32_t buffer_size) {
  return ap2_reply_setup_stream(out, cap, stream_type, data_port,
                                control_port, buffer_size);
}

static inline size_t bplist_build_feedback_response(uint8_t *out, size_t cap,
                                                    int64_t stream_type,
                                                    double sample_rate) {
  return ap2_reply_feedback(out, cap, stream_type, sample_rate);
}

/* The /info reply with this receiver's identity (airplay_identity.h). */
size_t bplist_build_info_response(uint8_t *out, size_t cap,
                                  const char *device_id,
                                  const char *device_name,
                                  const uint8_t *public_key,
                                  size_t public_key_len, uint64_t features,
                                  double initial_volume);

static inline size_t bplist_build_media_remote_command(uint8_t *out,
                                                       size_t cap,
                                                       uint8_t command) {
  return ap2_event_command(out, cap, command);
}

static inline size_t bplist_build_media_remote_volume(uint8_t *out, size_t cap,
                                                      double volume) {
  return ap2_event_volume(out, cap, volume);
}
