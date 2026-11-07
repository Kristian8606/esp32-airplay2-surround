/* SPDX-License-Identifier: Apache-2.0 */
#include "mdns_airplay_txt.h"

#include <stdio.h>
#include <string.h>

/* Copies at most dst_size - 1 bytes of src and the NUL. */
static void copy_cut(char *dst, size_t dst_size, const char *src)
{
  size_t len = strnlen(src, dst_size - 1);
  memcpy(dst, src, len);
  dst[len] = '\0';
}

size_t mdns_airplay_utf8_prefix_len(const char *s, size_t max_len)
{
  size_t len = strnlen(s, max_len + 1);
  if (len <= max_len) {
    return len;
  }
  size_t cut = max_len;
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) {
    cut--;
  }
  return cut;
}

/* Appends name cut to whole UTF-8 sequences so that out keeps at most
 * MDNS_AIRPLAY_LABEL_MAX bytes; out holds used bytes already. A cut that
 * leaves nothing appends the fallback name instead. */
static void append_name(char out[MDNS_AIRPLAY_LABEL_MAX + 1], size_t used,
                        const char *name)
{
  size_t len = mdns_airplay_utf8_prefix_len(name, MDNS_AIRPLAY_LABEL_MAX - used);
  if (len == 0 && name[0] != '\0') {
    name = MDNS_AIRPLAY_FALLBACK_NAME;
    len = mdns_airplay_utf8_prefix_len(name, MDNS_AIRPLAY_LABEL_MAX - used);
  }
  memcpy(out + used, name, len);
  out[used + len] = '\0';
}

void mdns_airplay_airplay_instance(const char *device_name,
                                   char out[MDNS_AIRPLAY_LABEL_MAX + 1])
{
  append_name(out, 0, device_name ? device_name : "");
}

void mdns_airplay_raop_instance(const char *mac_str, const char *device_name,
                                char out[MDNS_AIRPLAY_LABEL_MAX + 1])
{
  size_t used = 0;
  if (mac_str) {
    /* At most the 17 bytes of a MAC string, so a broken one still leaves
     * room for "@" and a part of the name. */
    for (size_t i = 0; i < MDNS_AIRPLAY_MAC_STR_LEN - 1 && mac_str[i] != '\0';
         i++) {
      if (mac_str[i] != ':') {
        out[used++] = mac_str[i];
      }
    }
  }
  out[used++] = '@';
  append_name(out, used, device_name ? device_name : "");
}

static void hex_lower(char *out, const uint8_t *bytes, size_t len)
{
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = digits[bytes[i] >> 4];
    out[2 * i + 1] = digits[bytes[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

void mdns_airplay_build_txt(mdns_airplay_txt_records_t *rec,
                            const char *mac_str, const uint8_t *public_key,
                            const char *pairing_id)
{
  copy_cut(rec->deviceid, sizeof rec->deviceid, mac_str);
  snprintf(rec->features, sizeof rec->features, "0x%X,0x%X",
           (unsigned)AIRPLAY_FEATURES_LO, (unsigned)AIRPLAY_FEATURES_HI);
  snprintf(rec->flags, sizeof rec->flags, "0x%X",
           (unsigned)AIRPLAY_STATUS_FLAGS);
  hex_lower(rec->pk, public_key, MDNS_AIRPLAY_PUBLIC_KEY_LEN);
  copy_cut(rec->pi, sizeof rec->pi, pairing_id);
  snprintf(rec->vv, sizeof rec->vv, "%d", (int)AIRPLAY_PROTOCOL_VERSION);

  const mdns_airplay_txt_t airplay[MDNS_AIRPLAY_AIRPLAY_TXT_COUNT] = {
      {"deviceid", rec->deviceid},
      {"features", rec->features},
      {"flags", rec->flags},
      {"model", AIRPLAY_MODEL},
      {"pk", rec->pk},
      {"pi", rec->pi},
      {"srcvers", AIRPLAY_SOURCE_VERSION},
      {"vv", rec->vv},
      {"acl", "0"},
  };
  const mdns_airplay_txt_t raop[MDNS_AIRPLAY_RAOP_TXT_COUNT] = {
      {"am", AIRPLAY_MODEL},
      {"cn", "0,1,2,3"},
      {"da", "true"},
      {"et", "0,3,5"},
      {"ft", rec->features},
      {"md", "0,2"},
      {"pk", rec->pk},
      {"sf", rec->flags},
      {"tp", "UDP"},
      {"vn", "65537"},
      {"vs", AIRPLAY_SOURCE_VERSION},
      {"vv", rec->vv},
  };
  memcpy(rec->airplay, airplay, sizeof airplay);
  memcpy(rec->raop, raop, sizeof raop);
}
