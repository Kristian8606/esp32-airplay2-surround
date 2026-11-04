#include "ap2_compat.h"

#include "airplay_identity.h"

size_t bplist_build_info_response(uint8_t *out, size_t cap,
                                  const char *device_id,
                                  const char *device_name,
                                  const uint8_t *public_key,
                                  size_t public_key_len, uint64_t features,
                                  double initial_volume) {
  char pairing_id[AIRPLAY_PAIRING_ID_LEN];

  airplay_get_pairing_id(pairing_id, sizeof(pairing_id));
  const ap2_info_t info = {
      .device_id = device_id,
      .name = device_name,
      .model = AIRPLAY_MODEL,
      .source_version = AIRPLAY_SOURCE_VERSION,
      .protocol_versions = AIRPLAY_PROTOVERS,
      .protocol_version = AIRPLAY_PROTOCOL_VERSION,
      .status_flags = AIRPLAY_STATUS_FLAGS,
      .features = features,
      .public_key = public_key,
      .public_key_len = public_key_len,
      .pairing_id = pairing_id,
      .initial_volume = initial_volume,
      .audio_stream_formats = AIRPLAY_SUPPORTED_AUDIO_STREAM,
      .buffer_stream_formats = AIRPLAY_SUPPORTED_BUFFER_STREAM,
  };
  return ap2_reply_info(out, cap, &info);
}
