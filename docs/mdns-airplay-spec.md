# mdns_airplay: AirPlay announcement over mDNS

## Purpose

The Gateway firmware (ESP32-S3, ESP-IDF v6.1) is an AirPlay 2 receiver.
iPhones, iPads, Macs and Apple TVs find it through mDNS (Bonjour): two
services on TCP 7000, `_airplay._tcp` and `_raop._tcp`, with TXT records
that say what it is and what it supports. This part starts mDNS, sets the
mDNS host name and adds those two services.

The interface is `deps/mdns_airplay.h`. Its name and signature stay as they
are; the firmware's start-up already calls it. The public header goes to
`include/mdns_airplay.h`.

Sources: **[board]** the records a running Gateway announces, read on a Mac
with `dns-sd -B` / `dns-sd -L` on 2026-11-07; **[callers]** the code that
calls this function and the functions this part calls; **[ESP-IDF]**
ESP-IDF v6.1 and the Espressif `mdns` component 1.14.1 (its public API,
`mdns.h`); **[RFC 1035]**, **[RFC 6762]**, **[RFC 6763]**. Rules marked
"decided" were chosen for this firmware where no source shows the
behaviour.

## Placement

Source files in `src/`, the public header in `include/`. In the firmware
the code goes into the `ap2_core` component (Apache-2.0), next to the RTSP
server that answers on port 7000. It needs the ESP-IDF components `mdns`
(Espressif managed component `espressif/mdns` ^1.14.1), `log` and
`esp_common`, and these headers of the firmware, given in `deps/`:

- `airplay_identity.h`: model, source version, status flags, feature bits,
  protocol version and `airplay_get_pairing_id()`. The same values go into
  GET /info, so mDNS and GET /info always agree: use these macros, do not
  repeat the values.
- `ap2_hap.h`: `ap2_hap_identity_public_key()`, the 32-byte Ed25519 public
  key of the device.
- `wifi.h`: `wifi_get_mac_str()` and `wifi_sanitize_hostname()`.
- `settings.h`: `settings_get_device_name()`.

Code that needs no ESP-IDF (building the names and TXT values) is kept
apart, so that host tests on a computer can run it with fake `mdns_*`,
settings, Wi-Fi and identity functions.

## Environment

- ESP32-S3. The `mdns` component is configured with
  `CONFIG_MDNS_MAX_SERVICES=4` (this part adds 2; the speaker control link adds
  `_apsurround._tcp` later, so 3 are used), its task and memory in PSRAM
  (`CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM=y`,
  `CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y`) and
  `CONFIG_MDNS_RESPOND_REVERSE_QUERIES` off [ESP-IDF, sdkconfig]. With that
  setting the component keeps at most 64 bytes of a host or instance name
  and cuts longer ones without saying so [ESP-IDF, `mdns.h`
  `MDNS_NAME_MAX_LEN`].
- Gateway start-up, in one task [callers]: settings, Wi-Fi
  (`wifi_init_apsta`), the device identity (`ap2_hap_identity_init`), the
  audio engine, then `mdns_airplay_init()`, then the RTSP server on TCP
  7000, then the speaker control link, which adds `_apsurround._tcp` with
  `mdns_service_add()` and so needs mDNS already started. It is not called
  in the first-time setup mode (no saved network) and not on the speakers.
- Called once. A new device name is saved by the web page with the note
  "Restart to apply" [callers]: the announcement changes only at the next
  start (decided).

## mdns_airplay_init()

In this order:

1. `mdns_init()`. On an error: log it and return; nothing else is done.
2. Read the device name with `settings_get_device_name()` into a buffer of
   65 bytes (64 bytes and the NUL, as the speaker control link does) [callers]. On
   an error, or an empty name, the instance name is
   `AirPlay Surround Gateway`, the name the speaker control link uses in the same
   case [callers] (decided). An error is logged (one `ESP_LOGE`, see
   "Log lines"); an empty name is not. So the name that arrives has at
   most 64 bytes; how the settings shorten a longer saved name is theirs
   to decide. The 80- and 81-byte examples below are for the name rules
   themselves; through `mdns_airplay_init()` they are tested with a fake
   that cuts like `strlcpy` (decided).
3. Host name: `wifi_sanitize_hostname(name, host, 32)` with the device name
   as read (an empty string when step 2 failed), then
   `mdns_hostname_set(host)`. The Wi-Fi module makes the DHCP host name
   from the device name with the same function, so the two agree
   [callers]; for an empty name it gives `esp32-airplay-` and the last
   three MAC bytes. The result has at most 31 bytes (`WIFI_HOSTNAME_MAX_LEN`).
   On an error: log it and go on.
4. `mdns_service_add()` for `_airplay`, `_tcp`, port 7000 with the TXT
   records below. On an error: log it and go on.
5. `mdns_service_add()` for `_raop`, `_tcp`, port 7000 with its TXT
   records. On an error: log it.
6. One log line when all of steps 3–5 succeeded (decided), tag
   `mdns_airplay`:
   `AirPlay on mDNS as "<instance>" (<host>.local, port 7000)`, where
   `<instance>` is the `_airplay._tcp` instance name. Each error is one
   `ESP_LOGE` line, see "Log lines".

The public key is never NULL: `ap2_hap_identity_public_key()` returns a
fixed 32-byte array, and start-up stops (`ESP_ERROR_CHECK`) when
`ap2_hap_identity_init()` fails, before this function is called [callers].
No check for NULL (decided).

`wifi_get_mac_str()` gives `AA:BB:CC:DD:EE:FF` upper case, or an empty
string when the MAC cannot be read [callers]. The value is used as it is
(an empty MAC gives an empty `deviceid` and a `_raop` instance `@<name>`);
the `_raop` name part is cut to 63 bytes minus the prefix with its `@`
(50 with a normal MAC). No other check (decided).

`wifi_sanitize_hostname()` never gives an empty string [callers]; its
result is passed to `mdns_hostname_set()` without a check (decided).

### Log lines (decided)

Tag `mdns_airplay`, `<err>` is `esp_err_to_name()`:

- `settings_get_device_name failed: <err>` (then the fallback name)
- `mdns_init failed: <err>`
- `mdns_hostname_set("<host>") failed: <err>`
- `mdns_service_add(_airplay._tcp) failed: <err>`
- `mdns_service_add(_raop._tcp) failed: <err>`
- the success line of step 6.

No other `mdns_*` calls (no instance name of the host, no delegated host,
no subtype) [board: none are announced].

### Instance names [RFC 6763, decided]

A DNS label has at most 63 bytes [RFC 1035 2.3.4]; a service instance name
is one label [RFC 6763 4.1.1]. The names are cut so that they fit:

- `_airplay._tcp`: the device name, cut to at most 63 bytes.
- `_raop._tcp`: the station MAC as 12 upper-case hex digits without
  colons (from `wifi_get_mac_str()`, colons removed), `@`, then the device
  name cut so that the whole instance name has at most 63 bytes (so the
  name part at most 50 bytes).
- A cut never splits a UTF-8 sequence: it ends before the first byte of a
  sequence that would not fit whole (a byte `10xxxxxx` never starts the
  remainder). Bytes are otherwise passed as they are (names may hold any
  UTF-8, spaces, `=`, `.`; the `mdns` component escapes them on the wire).
- The name is not checked for valid UTF-8: the web page sends it from a
  browser as UTF-8 [callers]. The cut rule is applied as written, also to
  invalid bytes (a lead byte without its continuation bytes may stay at
  the end). If a cut leaves an empty name, the fallback name is used
  instead, for that instance (decided).

The fallback name `AirPlay Surround Gateway` follows the same rules.

### `_airplay._tcp` TXT records [board]

In this order (the order does not matter to senders):

| Key | Value | Source of the value |
|---|---|---|
| `deviceid` | the station MAC, `AA:BB:CC:DD:EE:FF` upper case | `wifi_get_mac_str()` |
| `features` | `0x401C4A00,0x1C340` | `AIRPLAY_FEATURES_LO`, `AIRPLAY_FEATURES_HI` |
| `flags` | `0x4` | `AIRPLAY_STATUS_FLAGS` |
| `model` | `AudioAccessory6,1` | `AIRPLAY_MODEL` (menuconfig) |
| `pk` | 64 lower-case hex digits | `ap2_hap_identity_public_key()` |
| `pi` | the pairing id, e.g. `079479db-7e21-4838-8197-534cbc1bf687` | `airplay_get_pairing_id()` (`AIRPLAY_PAIRING_ID_LEN`) |
| `srcvers` | `377.40.00` | `AIRPLAY_SOURCE_VERSION` |
| `vv` | `2` | `AIRPLAY_PROTOCOL_VERSION` |
| `acl` | `0` | fixed |

### `_raop._tcp` TXT records [board]

| Key | Value | Source of the value |
|---|---|---|
| `am` | `AudioAccessory6,1` | `AIRPLAY_MODEL` |
| `cn` | `0,1,2,3` | fixed |
| `da` | `true` | fixed |
| `et` | `0,3,5` | fixed |
| `ft` | `0x401C4A00,0x1C340` | as `features` |
| `md` | `0,2` | fixed |
| `pk` | as in `_airplay` | as in `_airplay` |
| `sf` | `0x4` | as `flags` |
| `tp` | `UDP` | fixed |
| `vn` | `65537` | fixed |
| `vs` | `377.40.00` | as `srcvers` |
| `vv` | `2` | as `vv` |

Number formats: `features`/`ft` are the low then the high 32 bits, each
`0x` and upper-case hex without leading zeros (`printf` `0x%X,0x%X`);
`flags`/`sf` are `0x%X`; `vv` is decimal. The model and the other values
are copied as they are.

The TXT values must stay valid until `mdns_service_add()` returns (the
component copies them) [ESP-IDF].

## Examples

With the fakes: device name `Living Room`, MAC `AA:BB:CC:01:02:03`, public
key bytes `k[i] = i * 7 + 1` (i = 0..31), pairing id
`11111111-2222-4333-8444-555555555555`, a host name function that gives
`h-Living-Room`, the calls are:

```
mdns_init()
mdns_hostname_set("h-Living-Room")
mdns_service_add("Living Room", "_airplay", "_tcp", 7000,
  deviceid=AA:BB:CC:01:02:03 features=0x401C4A00,0x1C340 flags=0x4
  model=AudioAccessory6,1
  pk=01080f161d242b323940474e555c636a71787f868d949ba2a9b0b7bec5ccd3da
  pi=11111111-2222-4333-8444-555555555555 srcvers=377.40.00 vv=2 acl=0)
mdns_service_add("AABBCC010203@Living Room", "_raop", "_tcp", 7000,
  am=AudioAccessory6,1 cn=0,1,2,3 da=true et=0,3,5 ft=0x401C4A00,0x1C340
  md=0,2
  pk=01080f161d242b323940474e555c636a71787f868d949ba2a9b0b7bec5ccd3da
  sf=0x4 tp=UDP vn=65537 vs=377.40.00 vv=2)
```

The running Gateway [board], device name `ESP32 AirPlay EC8`, MAC
`14:C1:9F:27:BE:C8`:

```
ESP32 AirPlay EC8._airplay._tcp.local. at ESP32-AirPlay-EC8.local.:7000
  deviceid=14:C1:9F:27:BE:C8 features=0x401C4A00,0x1C340 flags=0x4
  model=AudioAccessory6,1
  pk=c6539e94022b22c73830398a994698438e61448b195521a9c7ff83f585baf44f
  pi=079479db-7e21-4838-8197-534cbc1bf687 srcvers=377.40.00 vv=2 acl=0
14C19F27BEC8@ESP32 AirPlay EC8._raop._tcp.local. at ESP32-AirPlay-EC8.local.:7000
  am=AudioAccessory6,1 cn=0,1,2,3 da=true et=0,3,5 ft=0x401C4A00,0x1C340
  md=0,2 pk=(as above) sf=0x4 tp=UDP vn=65537 vs=377.40.00 vv=2
```

Names that are cut (UTF-8):

- 80 × `n`: `_airplay` instance 63 × `n`; `_raop` instance
  `AABBCC010203@` and 50 × `n`.
- 40 × `я` (2 bytes each, 80 bytes): `_airplay` 31 × `я` (62 bytes);
  `_raop` `AABBCC010203@` and 25 × `я` (50 bytes).
- `x` and 40 × `я` (81 bytes): `_airplay` `x` and 31 × `я` (63 bytes);
  `_raop` `AABBCC010203@`, `x` and 24 × `я` (49 bytes).
- `Хол x`: passed as it is (8 bytes).

## Tests on a computer

With fake `mdns_*` functions that record their calls, and fakes of the
settings, Wi-Fi and identity functions: the example above byte for byte;
the cut names above; a settings error and an empty name (fallback instance
name, host name from the empty string); an error from each `mdns_*` call
(what is still called and what is logged); names with `=`, `.` and spaces.
None of these may crash or read or write outside a buffer.

## Open points

None.
