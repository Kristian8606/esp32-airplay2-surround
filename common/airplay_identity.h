#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

/* How this receiver presents itself to AirPlay 2 senders. Used by mDNS
 * (_airplay/_raop TXT records), GET /info and the RTSP "Server" header, so
 * all of them always agree. */

/* menuconfig AirPlay Receiver -> AirPlay model identifier; the sender
 * picks its icon from it (AudioAccessory6,1 = HomePod, 2nd generation). */
#define AIRPLAY_MODEL          CONFIG_AIRPLAY_MODEL_ID
#ifdef CONFIG_AIRPLAY_HOMEKIT
/* As an AirPort Express reports it. */
#define AIRPLAY_SOURCE_VERSION "366.0"
#else
#define AIRPLAY_SOURCE_VERSION "377.40.00"
#endif
#define AIRPLAY_SERVER_HEADER  "AirTunes/" AIRPLAY_SOURCE_VERSION
#define AIRPLAY_PROTOVERS      "1.1"
/* "vv" / protocol version: AirPlay 2 only. */
#define AIRPLAY_PROTOCOL_VERSION 2
/* statusFlags / "flags" / "sf": 0x4 = audio receiver. The current value is
 * airplay_status_flags(): with HomeKit it adds the two bits below. */
#define AIRPLAY_STATUS_FLAGS   0x4
/* Bit 10: in a HomeKit home (an admin controller is paired), as an AirPort
 * Express in a home advertises 0x404. */
#define AIRPLAY_STATUS_HOMEKIT 0x400
/* Bit 11, DeviceSupportsRelay: while a sender holds the audio session. An
 * Apple TV then describes the speaker with canRelayCommunicationChannel and
 * shows the group's artwork and controls for it (an AirPort Express in a
 * group advertises 0xc04). */
#define AIRPLAY_STATUS_RELAY   0x800
uint32_t airplay_status_flags(void);
/* True when the flags changed (the caller publishes them again). */
bool airplay_set_session_active(bool active);

/* Feature bits of mDNS "features"/"ft" and GET /info, high and low 32 bits:
 * bits 9, 11, 14, 18, 19, 20, 30, 38, 40, 41, 46, 47 and 48 (bit 22 off).
 * The high ones announce AirPlay 2 pairing: 38 and 46 for the encrypted,
 * paired control channel, 48 for transient (PIN-less) pairing. */
#define AIRPLAY_FEATURES_HI 0x1C340
#ifdef CONFIG_AIRPLAY_HOMEKIT
/* + bit 16 (progress metadata), as an AirPort Express. Its MFi bit 26 is not
 * advertised: senders then demand /auth-setup, which needs Apple's chip. */
#define AIRPLAY_FEATURES_LO 0x401D4A00 /* bit 22 off */
#else
#define AIRPLAY_FEATURES_LO 0x401C4A00 /* bit 22 off */
#endif

/* GET /info "supportedFormats": bit N = AirPlay audio format N. The sender
 * echoes its pick back as "audioFormat" in the SETUP stream dict. */
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

/* Serial number (Wi-Fi MAC in hex, 12 characters) and firmware revision
 * (the project version without the leading "v"), as the Home app shows
 * them. */
#define AIRPLAY_SERIAL_LEN 13
void airplay_get_serial_number(char *out, size_t len);
void airplay_get_firmware_revision(char *out, size_t len);
