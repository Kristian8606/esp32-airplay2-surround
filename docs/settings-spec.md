# settings: the saved settings in NVS

## Purpose

Both firmwares (ESP32-S3, ESP-IDF v6.0.2), a Gateway and the speakers,
keep a few settings in NVS: the device name, up to 8 Wi-Fi networks, the
AirPlay volume and the speaker's audio settings. This part reads and
writes them.

**Compatibility is the main rule.** Boards in use already hold these
settings, written by the firmware this part replaces. After an update the
new code must read them exactly (same namespace, keys, types and the
binary layout of the network list), and what it writes must be readable by
that older firmware too (a board may be rolled back). [black box] marks
rules taken from the current firmware's behaviour, recorded through a fake
NVS; they are part of that compatibility.

The interface is `deps/settings.h`. Names, types, macros and signatures
stay as they are; other code calls them. The public header goes to
`include/settings.h`.

Sources: **[black box]** the current settings code run on a computer
against a fake NVS that recorded every call; **[callers]** the code that
calls these functions; **[ESP-IDF]** ESP-IDF v6.0.2 (NVS API). Rules marked
"decided" were chosen where the current behaviour was a flaw or is not
seen.

## Placement

An ESP-IDF component `settings` (Apache-2.0): sources in `src/`,
`include/settings.h` its only public header. It needs `nvs_flash`,
`esp_hw_support` (`esp_mac.h`), `log`, `freertos`. The rules that need no
ESP-IDF (the network list layout and its changes, the default name, the
volume rounding) are kept apart so that host tests on a computer can run
them against a fake NVS.

## Environment [callers]

- `nvs_flash_init()` is called by the firmware before `settings_init()`;
  this part never initialises or erases the NVS partition.
- `settings_init()` once in start-up, in the main task, before Wi-Fi.
- Callers: the main task, the web server task, the RTSP server's settings
  task, the speaker link task, the audio code (latency, loudness). Several
  may call at the same time. ESP-IDF's NVS API is safe from several tasks;
  this part's own state (the volume in RAM, a read-modify-write of the
  network list) is protected with a mutex (decided).
- A flash write from a task whose stack is in PSRAM asserts in ESP-IDF;
  callers that write run on stacks in internal RAM. This part does not
  create tasks.
- Options from `sdkconfig.h`: `CONFIG_AIRPLAY_OUTPUT_LATENCY_US` (always
  defined, −100000..150000, default 0), `CONFIG_AIRPLAY_MULTICHANNEL` (y in
  both firmwares) and, only with it, `CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER`
  (0..8).

## NVS layout [black box]

Namespace `airplay`, one for everything. Reads open it `NVS_READONLY`,
writes `NVS_READWRITE`; every write is followed by `nvs_commit()`; the
handle is closed after each call. A read-only open of a namespace that does
not exist yet fails with `ESP_ERR_NVS_NOT_FOUND`; that means "nothing
saved".

| Key | Type | Meaning |
|---|---|---|
| `device_name` | str | device name, UTF-8 |
| `wifi_list` | blob, 796 bytes | the network list (below) |
| `wifi_ssid` | str | the network set last (also an older layout, see below) |
| `wifi_pass` | str | its password |
| `volume_mdb` | i32 | volume in thousandths of a dB |
| `out_lat_us` | i32 | output latency in µs |
| `loud_en` | u8 | loudness on (1) / off (0) |
| `loud_ref` | i32 | loudness reference offset in dB |
| `mc_speaker` | i32 | 5.1/7.1 speaker index, −1 = mono mix |
| `sur_trim` | i32 | surround level trim, tenths of a dB |
| `sur_delay` | i32 | surround extra delay in µs |

### The network list `wifi_list`

796 bytes, little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x57494649` (bytes `49 46 49 57`) |
| 4 | 2 | version, 1 |
| 6 | 2 | count, 0..8 |
| 8 | 2 | index of the network set last, 0..7 (0 when empty) |
| 10 | 2 | next slot to replace when the list is full, 0..7 |
| 12 | 8 × 98 | slots: `ssid[33]` then `password[65]`, NUL-padded |

Slots `0..count-1` are in use; the rest are zero. A list is valid only when
its size is 796, the magic and version match, `count <= 8` and, when
`count > 0`, the last-set index is `< count` [black box: any of these
wrong → treated as no list], and "next" is `< 8` (decided). Also invalid
(decided; the current code reads past the field): a slot in use whose
`ssid` has no NUL in its 33 bytes or whose `password` has none in its 65.

An empty `ssid` inside a valid list is kept and returned as it is
[black box]; the Wi-Fi module skips it [callers].

### The older single network

Before the list, one network was kept in `wifi_ssid` / `wifi_pass`. When
there is no valid list and `wifi_ssid` holds a non-empty string, that
network counts as the only saved one (count 1, password `wifi_pass` or ""
when that key is missing) [black box]. The keys are still written with
every new network, so that older firmware finds the last one [black box].

## Functions

All return `ESP_ERR_INVALID_ARG` for a NULL output pointer or a zero
length [black box]. "NVS error" below means the error code of the failing
NVS call, returned as it is.

### settings_init()

- Opens the namespace read-write; on failure returns that error (the
  firmware stops with `ESP_ERROR_CHECK`) [black box].
- When there is no valid `wifi_list` and the older single network exists
  (non-empty `wifi_ssid`): writes a list holding it (count 1, last-set 0,
  next 0) and logs `Migrated the saved Wi-Fi network into the network list`
  (tag `settings`) [black box: the same step; the text is new].
- Otherwise writes nothing (decided: the current code also writes an
  empty list on a blank NVS, which is not needed).
- Returns `ESP_OK`.

### Wi-Fi networks

- `settings_get_wifi_network_count()`: the count of the valid list, else
  1 for the older single network, else 0 (also when NVS cannot be read).
- `settings_has_wifi_credentials()`: count > 0.
- `settings_get_wifi_network(i, out)`: slot `i` of the list (or the older
  single network for `i = 0`), both strings NUL-terminated. `i >= count`:
  `ESP_ERR_NOT_FOUND` [black box]. Slot order, not by age [black box].
- `settings_set_wifi_credentials(ssid, password)`:
  - `ssid` NULL, empty, or 33 bytes or longer: `ESP_ERR_INVALID_ARG`.
    `password` NULL counts as "" (an open network); 65 bytes or longer:
    `ESP_ERR_INVALID_ARG`. No other check (a 7-byte password is taken; a
    64-byte one, a hex key, too) [black box].
  - Starts from the valid list, else from the older single network, else
    from an empty list (decided; after `settings_init()` the older network
    is already in the list).
  - The same `ssid` (byte for byte) already in a slot: its password is
    replaced and the last-set index points to it.
  - Else, count < 8: the new network goes into slot `count`, which becomes
    the last-set index; count + 1.
  - Else (full): it replaces the slot at "next", which becomes the
    last-set index; next = (next + 1) mod 8.
  - Writes `wifi_list`, then `wifi_ssid` and `wifi_pass` with this network,
    then commits. On an NVS error: that error, logged with `ESP_LOGE`
    (decided).

Examples [black box]: an empty NVS, then `NetA/password1`: header
`49 46 49 57 01 00 01 00 00 00 00 00`, slot 0 `NetA`/`password1`. Then
`NetB/password2`: `… 02 00 01 00 00 00`. Then `NetA/newpass11`:
`… 02 00 00 00 00 00`, slot 0 password `newpass11`. A full list (S0..S7,
next 0) and `Y` then `Z`: slots `Y Z S2 S3 S4 S5 S6 S7`, header
`… 08 00 01 00 02 00`. Eight more networks after a full list fill the
slots in turn, so that the list then holds the last eight.

### Device name

- `settings_get_device_name(name, len)`: the saved name when it exists and
  is not empty. Else (missing, empty, NVS cannot be read) the default name:
  `ESP32 AirPlay ` and the last three hex digits of the Wi-Fi station MAC,
  upper case (the low 4 bits of byte 4, then byte 5, `%X%02X`): MAC
  `14:C1:9F:27:BE:C8` → `ESP32 AirPlay EC8`, `AA:BB:CC:01:02:03` →
  `ESP32 AirPlay 203` [black box]. Always `ESP_OK` apart from bad
  arguments [black box].
- The result is cut to `len - 1` bytes. Cut at a whole UTF-8 character
  (decided; the current code gives the default name instead when the saved
  name does not fit, and cuts the default name byte-wise). All callers use
  65 bytes [callers].
- `settings_set_device_name(name)`: NULL counts as "" [black box]; longer
  than 64 bytes: `ESP_ERR_INVALID_ARG` (decided; the current code saves it
  and then reads back the default name). Saved as it is (an empty name
  makes the default name come back). NVS error: that error, logged.

### Volume

- The volume lives in RAM; NVS holds it as `volume_mdb`, the dB value times
  1000 cut toward zero (`(int32_t)(db * 1000)`): −15.5 → −15500,
  −12.3456 → −12345, −0.0006 → 0, −99.9995 → −99999 [black box].
- `settings_get_volume(&db)`: the RAM value once there is one (after a set,
  or after the first read of NVS found one). Before that it reads
  `volume_mdb` once: found → `db = mdb / 1000.0`, kept in RAM, `ESP_OK`;
  not found or NVS cannot be read → that error (`ESP_ERR_NVS_NOT_FOUND` for
  nothing saved) and `*db` is not changed (decided; the caller then uses
  its own default) [black box: an error].
- `settings_set_volume(db)`: RAM only, no range check, `ESP_OK`
  [black box].
- `settings_persist_volume()`: when the RAM value in thousandths differs
  from the last value read from or written to NVS (or none is known),
  writes `volume_mdb` and commits; else does nothing; `ESP_OK`, or the NVS
  error [black box].

### Speaker audio settings

No range checks on set [black box]. On get, nothing saved gives the
default and `ESP_OK` [black box]; so does an NVS that cannot be read
[black box for the latency; the same for the others, decided].

| Function | Keys | Default |
|---|---|---|
| `settings_get/set_output_latency_us` | `out_lat_us` | `CONFIG_AIRPLAY_OUTPUT_LATENCY_US` |
| `settings_clear_output_latency` | erases `out_lat_us`; `ESP_OK` also when it was not there [black box] | — |
| `settings_get/set_loudness` | `loud_en` (u8; any non-zero reads as on, decided), `loud_ref` | off, 0 |
| `settings_get/set_mc_speaker` | `mc_speaker` | `CONFIG_AIRPLAY_MULTICHANNEL_DEFAULT_SPEAKER`; −1 without `CONFIG_AIRPLAY_MULTICHANNEL` [black box] |
| `settings_get/set_surround_tuning` | `sur_trim`, `sur_delay` | 0, 0 |

A set writes all its keys, then commits; an NVS error is returned and
logged.

## Answers to the first questions (decided)

1. The older single network with `wifi_ssid` over 32 bytes, `wifi_pass`
   over 64 bytes, or an error reading `wifi_pass` other than
   `ESP_ERR_NVS_NOT_FOUND`: treated as not there (count 0, nothing
   migrated).
2. `settings_init()` when writing the migrated list fails: logged
   (`Wi-Fi network list not saved: <err>`), `ESP_OK`; the older network is
   still read from `wifi_ssid` / `wifi_pass` and the next start tries again.
3. `esp_read_mac()` failing: the MAC counts as all zeros
   (`ESP32 AirPlay 000`).
4. `settings_persist_volume()` with no volume in RAM: writes nothing,
   `ESP_OK`.
5. A NaN volume persists as 0; a value outside the `int32_t` range of
   thousandths as `INT32_MAX` / `INT32_MIN`; every other value is the
   plain cast.
6. `settings_get_wifi_network()` when NVS cannot be read: the count is 0,
   so `ESP_ERR_NOT_FOUND` for every index.
7. Loudness and surround tuning with only one of their two keys saved:
   each key is read on its own; the missing one gets its default.
8. The migration line is `ESP_LOGI`.
9. The options are defined by the firmware's `airplay_options` component
   (its `Kconfig`); this component ships no `Kconfig`.
10. The mutex is created by `settings_init()`, or by the first call if one
    comes earlier. The firmware calls `settings_init()` in the main task
    before any other task that uses settings starts [callers], so two first
    calls at once do not happen.

## Log (decided)

Tag `settings`: the migration line above, and one `ESP_LOGE` per failed
write (`<what> not saved: <err>`). Nothing on reads.

## Tests on a computer

With a fake NVS (namespaces, typed keys, read-only and read-write handles,
`ESP_ERR_NVS_NOT_FOUND` for a missing namespace or key, an error to inject
on open or set): every example above, byte for byte where a blob is
written; reading lists written by the examples; every invalid list
(each header field out of range, size 795 and 797, an ssid or password
without a NUL); the older single network with and without `wifi_pass`;
the default name for both MACs and for short buffers; names cut inside a
UTF-8 character; volume rounding and persist only on change; defaults of
each speaker setting; errors on open and on set; calls from several
threads at once. None may crash or read past a buffer.

## Open points

None.
