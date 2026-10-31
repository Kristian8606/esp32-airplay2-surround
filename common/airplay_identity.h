#pragma once

#include <stddef.h>

#include "sdkconfig.h"

/* How this receiver presents itself to AirPlay 2 senders. Used by mDNS
 * (_airplay/_raop TXT records), GET /info and the RTSP "Server" header, so
 * all of them always agree. */

/* menuconfig AirPlay Receiver -> AirPlay model identifier; the sender
 * picks its icon from it (AudioAccessory6,1 = HomePod, 2nd generation). */
#define AIRPLAY_MODEL          CONFIG_AIRPLAY_MODEL_ID
#define AIRPLAY_SOURCE_VERSION "377.40.00"
#define AIRPLAY_SERVER_HEADER  "AirTunes/" AIRPLAY_SOURCE_VERSION
#define AIRPLAY_PROTOVERS      "1.1"
/* "vv" / protocol version: AirPlay 2 only. */
#define AIRPLAY_PROTOCOL_VERSION 2
/* statusFlags / "flags" / "sf": 0x4 = audio receiver. */
#define AIRPLAY_STATUS_FLAGS   0x4

/* Feature bits advertised in mDNS "features"/"ft" and GET /info.
 * Key bits:
 *   Bit 38: SupportsCoreUtilsPairingAndEncryption
 *   Bit 46: SupportsHKPairingAndAccessControl
 *   Bit 48: SupportsTransientPairing */
#define AIRPLAY_FEATURES_HI 0x1C340
#define AIRPLAY_FEATURES_LO 0x401C4A00 /* bit 22 off, as Shairport Sync */

/* GET /info "supportedFormats": bit N = AirPlay audio format N, as in
 * Shairport Sync generateInfoPlist(). The sender echoes its pick back as
 * "audioFormat" in the SETUP stream dict.
 */
#define AIRPLAY_FMT_ALAC_44100_S16_2 (1ULL << 18) /* 0x40000 */
#define AIRPLAY_FMT_ALAC_48000_S24_2 (1ULL << 21)
#define AIRPLAY_FMT_AAC_LC_44100_2   (1ULL << 22)
#define AIRPLAY_FMT_AAC_LC_48000_2   (1ULL << 23)
#define AIRPLAY_FMT_AAC_LC_48000_5_1 (1ULL << 39)
#define AIRPLAY_FMT_AAC_LC_48000_7_1 (1ULL << 40)
#define AIRPLAY_SUPPORTED_AUDIO_STREAM AIRPLAY_FMT_ALAC_44100_S16_2
#define AIRPLAY_SUPPORTED_STEREO_BUFFER_STREAM                    \
  (AIRPLAY_FMT_ALAC_44100_S16_2 | AIRPLAY_FMT_ALAC_48000_S24_2 |    \
   AIRPLAY_FMT_AAC_LC_44100_2 | AIRPLAY_FMT_AAC_LC_48000_2)
#if defined(CONFIG_AIRPLAY_MULTICHANNEL) && CONFIG_AIRPLAY_MULTICHANNEL
#define AIRPLAY_SUPPORTED_BUFFER_STREAM                          \
  (AIRPLAY_SUPPORTED_STEREO_BUFFER_STREAM | AIRPLAY_FMT_AAC_LC_48000_5_1 | \
   AIRPLAY_FMT_AAC_LC_48000_7_1)
#else
#define AIRPLAY_SUPPORTED_BUFFER_STREAM AIRPLAY_SUPPORTED_STEREO_BUFFER_STREAM
#endif

/* Pairing identity UUID ("pi" in mDNS and GET /info), lowercase
 * 8-4-4-4-12 form. Derived from the Wi-Fi MAC, so it is stable across
 * reboots and unique per device. */
#define AIRPLAY_PAIRING_ID_LEN 37 /* 36 chars + NUL */
void airplay_get_pairing_id(char *out, size_t len);
