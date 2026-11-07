# wifi

Wi-Fi of both firmwares: the station on a saved network, the setup access
point `ESP32-AirPlay-Setup` with its captive DNS, and the DHCP host name. An
ESP-IDF component under the **Apache License 2.0** ([`LICENSE`](LICENSE),
[`NOTICE`](NOTICE)).

- `include/wifi.h`: the functions the firmwares call
- `src/wifi_logic.c`: host names, MAC and IP text, choosing a network, the
  setup page list, retry times, reason names; without ESP-IDF
- `src/wifi_ctl.c`: the station and access point rules as a state machine;
  without ESP-IDF
- `src/wifi.c`: netifs, events, timers, scans
- `tests/`: host tests of the first two (`make -C tests`)

`settings.h` (saved networks, device name) is a source of each firmware's
main component: `common/common.cmake` gives this component its include path
and links it with main. The captive DNS (`dns_server.h`) is in
`components/helpers`.

## Start

`wifi_init_apsta(NULL, NULL)` once at start-up: netifs, default event loop,
host name from the device name before the first DHCP request, **power save
off** (`WIFI_PS_NONE`: power save would hold received packets up to a beacon
interval). With no saved network only the access point runs; otherwise the
station connects in the background. A non-NULL access point SSID of 1 to 32
bytes and a password of 8 to 63 bytes (WPA2-PSK) replace the defaults (open
`ESP32-AirPlay-Setup`); anything else keeps them with a warning.

## Station

- **Choosing:** scan all channels; among saved SSIDs with a fitting
  security take the strongest access point and join that BSSID. None seen:
  try every saved network by SSID alone, one round counting as one failure.
- **Security:** a saved `""` joins only `WIFI_AUTH_OPEN`; a password joins
  WPA2-PSK, WPA/WPA2, WPA3-SAE or WPA2/WPA3 (PMF capable, not required).
- **No roaming:** 802.11k/v/r and MBO off; the station stays on its access
  point, because every board of a system must share one access point's
  beacon timer.
- **Retries:** after failure 1 at once, then 1, 2, 4, 8, 16 s to the same
  BSSID; from failure 7 on every 30 s a full choice (60 s while a client is on
  the access point). An address resets the count.
- **DHCP:** associated without an address for 20 s (after joining or after
  losing it): disconnect, one failure.
- **Connected** means associated with an IPv4 address.

## Access point

On with no saved network, or after 30 s without a station address (not in
the middle of an attempt or scan); off as soon as the station has an
address. At most 4 clients, `192.168.4.1/24`. The captive DNS runs exactly
while it is on.

## Functions

| Function | Behaviour |
|---|---|
| `wifi_wait_connected(ms)` | waits for an address; 0 = no limit; `false` before start |
| `wifi_is_connected()` | connected now |
| `wifi_get_ip_str(buf, len)` | `ESP_OK`; NULL `ESP_ERR_INVALID_ARG`; `len` < 16 `ESP_ERR_INVALID_SIZE`; not connected `ESP_ERR_INVALID_STATE`; `""` on errors |
| `wifi_get_mac_str(buf, len)` | `AA:BB:CC:01:02:03`; `""` when `len` < 18 or the MAC read fails |
| `wifi_scan(&list, &n)` | one entry per SSID (strongest), no hidden networks, by RSSI, at most 32; caller frees; 10 s limit; waits for the station's own scan or attempt |
| `wifi_sanitize_hostname(name, out, len)` | letters and digits kept, other runs one `-`, no `-` at the ends, at most 31; empty: `esp32-airplay-` + last 3 MAC bytes |
| `wifi_set_hostname(name)` | for the next DHCP request |

Scans: every scan gets a number; a `SCAN_DONE` while none runs is ignored.
A scan with no result in 10 s is stopped and the right to scan kept until
its `SCAN_DONE` or 1 s more (for the station one failure). A `SCAN_DONE`
later than that, during the next scan, cannot be told apart in ESP-IDF
v6.0.2 and is taken as that scan's; its list holds real networks too.

Log tag `wifi`, one line per event, never a password. No task of its own;
the work runs in the default event loop task, scan results go to PSRAM.

## Stack of the event loop task

The work runs in ESP-IDF's default event loop task (`sys_evt`, 2304 B
stack, shared with ESP-IDF's own handlers). So only `wifi_init_apsta()`
(main task) reads the saved networks (`src/wifi_saved.c`), into one PSRAM
block kept for the module's life; in `sys_evt` there is no `settings_*` or
NVS call and nothing over 64 B on the stack (`wifi_config_t` and scan
records are in PSRAM). The deepest chain, on the Xtensa build of v4.5.52
with `-fstack-usage`: `on_event` 128 B, `wl_ctl_on_scan_done` 64,
`wl_choose` 64, `attempt` 48, `fail` 48, `ap_check` 48, `ap_on` 32,
`op_connect` 112: about 544 B of module frames, without ESP-IDF's (logging,
Wi-Fi calls). `make -C tests stack` checks the same on the host compiler:
no recursion, chains under 1 KiB, frames under 256 B, no `settings_*` or
`nvs_*` calls from that code.

## Examples

1. No saved network: access point on at once, open, captive DNS on;
   `wifi_wait_connected(30000)` `false` after 30 s.
2. Saved `Home` in range: connected in seconds, access point never on.
3. `Home` (-70) and `Home-2G` (-50): joins `Home-2G`.
4. Mesh `Home` with nodes at -45 and -60: joins -45 and stays there.
5. Saved network away at start-up: access point and DNS on after 30 s, off
   when the station connects.
6. Router gone for 90 s: same BSSID after 0, 1, 2, 4, 8, 16 s, then every
   30 s with a scan; access point on 30 s after the address was lost.
7. Wrong password: failures logged, retry times grow, access point after
   30 s.
8. `wifi_scan()` while streaming: the station stays connected.
9. Hidden saved `Attic`: joined by SSID alone.
10. `wifi_set_hostname("Front Left")`: `Front-Left` from the next DHCP
    request.
11. Associated, no DHCP address: disconnect after 20 s, one failure.
12. A phone on the access point while the network is away: full choices
    every 60 s.

## Host tests

```bash
make -C tests
```

`make -C tests guard` runs them again under macOS Guard Malloc. Where
AddressSanitizer works: `make -C tests SAN=address,undefined`.
