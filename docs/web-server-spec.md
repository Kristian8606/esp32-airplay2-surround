# web_server: the shared web server

## Purpose

Both firmwares (ESP32-S3, ESP-IDF v6.0.2), a Gateway and the speakers,
serve a web page for set-up and maintenance on port 80. This part is the
shared core: it starts the ESP-IDF HTTP server, serves the shared pages
and APIs (system info, Wi-Fi, device name, restart, firmware update,
speed test, captive portal), and gives each firmware's own pages and APIs
(`main/web_gateway.c`, `speaker/main/web_speaker.c`) the helpers they use.

The interfaces are `deps/web_server.h` and `deps/web_server_internal.h`.
Names, types and signatures stay as they are; other code calls them. The
public headers go to `include/` with these names.

Sources: **[black box]** the current web server run on a computer against
a fake HTTP server and fakes of the firmware, recording every call and
response; **[board]** `curl` against a running Gateway (v4.5.61,
2026-11-07); **[pages]** the browser pages that call these APIs
(`data/www/index.html`, `speedtest.html`); **[callers]** the code that
calls the helpers; **[ESP-IDF]** ESP-IDF v6.0.2 (`esp_http_server`).
Rules marked "decided" were chosen where the current behaviour was a flaw
or is not seen.

## Placement

An ESP-IDF component `web_server` (Apache-2.0): sources in `src/`, the two
headers in `include/`. It needs `esp_http_server`, the managed component
`espressif__cjson`, `esp_wifi`, `esp_app_format`, `esp_system`,
`esp_timer`, `mbedtls` (base64), `heap`, `log`, `freertos`, and the
components `settings`, `wifi`, `ota`, `log_stream` (their headers are in
`deps/`).

It also calls code compiled into each firmware's main component: the
`web_role_*` functions (`web_server_internal.h`), `firmware_id()`
(`deps/firmware_id.h`), three audio engine functions
(`deps/audio_receiver.h`) and the three embedded pages below. The firmware
gives the component those headers and links it with main (as it does for
other components); the component must not include any other firmware
header.

The pages are embedded into the firmware image by the firmware's build as
binary data; their symbols are:

| Page | Start, end |
|---|---|
| `/` | `_binary_web_index_html_start`, `_binary_web_index_html_end` |
| `/logs` | `_binary_web_logs_html_start`, `_binary_web_logs_html_end` |
| `/speedtest` | `_binary_web_speedtest_html_start`, `_binary_web_speedtest_html_end` |

(`extern const uint8_t x[] asm("…")`.) The rules that need no ESP-IDF (the
JSON answers, the Basic auth check, the download sizes, the reset reason
and PHY names) are kept apart so that host tests can run them.

## Options [menuconfig]

`CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD` (string, default ""): the admin
password; empty = no password. Defined by the firmware's `airplay_options`
component; this component ships no `Kconfig`.

## web_server_start(port) [black box]

- Once; a second call: `ESP_ERR_INVALID_STATE` (decided).
- `httpd_start()` with `HTTPD_DEFAULT_CONFIG()` and: `server_port = port`,
  `task_priority = 3`, `stack_size = 8192` (in internal RAM, the default:
  handlers write flash), `max_uri_handlers = 40`, `lru_purge_enable = true`.
  Everything else default (7 sockets, no wildcard matching). On an error:
  returned (the firmware stops with `ESP_ERROR_CHECK`).
- Then: `web_role_register(server)`, the shared handlers (table below),
  the 404 handler (`httpd_register_err_handler`) and
  `log_stream_register(server)` (its error is logged, not returned). The
  order is decided; about 30 of the 40 handler slots are used.
- Log, tag `web_server`: `Web UI started on port <port>`.

## Shared pages and APIs

| Method, URI | Admin | Answer |
|---|---|---|
| GET `/` | — | page `index` |
| GET `/logs` | — | page `logs` |
| GET `/speedtest` | — | page `speedtest` |
| GET `/favicon.ico` | — | `204 No Content`, no body |
| GET `/api/system/info` | — | JSON, below |
| GET `/api/wifi/scan` | — | JSON, below |
| POST `/api/wifi/config` | yes | JSON, below; restarts |
| POST `/api/device/name` | yes | JSON, below |
| POST `/api/system/restart` | yes | text, restarts |
| POST `/api/ota/update` | yes | text, below; restarts |
| GET `/api/speedtest/ping` | — | `ok` |
| GET `/api/speedtest/download` | — | bytes, below |
| POST `/api/speedtest/upload` | — | `received=<n>` |
| GET `/hotspot-detect.html`, `/library/test/success.html`, `/generate_204`, `/connecttest.txt` | — | captive redirect |

[black box, board] "Admin": the handler first calls `web_admin_ok()`; on
false it returns at once.

### Pages

`Content-Type: text/html; charset=utf-8`, `Cache-Control: no-store`, the
embedded bytes as they are, in chunks of at most 1024 bytes
(`httpd_resp_send_chunk`), then the empty chunk [black box, board: `/` is
20000 bytes].

### GET /api/system/info [black box, board]

`application/json`, keys in this order:

```json
{"info":{"ip":"192.168.1.98","mac":"14:C1:9F:27:BE:C8","device_name":"ESP32 AirPlay EC8",
"wifi_connected":true,"role":"gateway","free_heap":2956072,"wifi_ssid":"UniFi",
"wifi_bssid":"8a:78:48:7e:d0:97","wifi_rssi":-35,"wifi_channel":11,"wifi_phy":"11n",
"firmware_version":"v4.5.61","firmware_build":"v4.5.61 29a5ab1d",
"reset_reason":"software","uptime_s":1013},"success":true}
```

- `ip`: `wifi_get_ip_str()`, "" when it fails; `mac`: `wifi_get_mac_str()`;
  `device_name`: `settings_get_device_name()` into 65 bytes, "" when it
  fails; `wifi_connected`: `wifi_is_connected()`; `role`:
  `web_role_name()`; `free_heap`: `esp_get_free_heap_size()`.
- Only when `esp_wifi_sta_get_ap_info()` succeeds: `wifi_ssid`,
  `wifi_bssid` (lower-case hex with colons), `wifi_rssi`, `wifi_channel`
  (`primary`), `wifi_phy`: `11n` if the 11n flag is set, else `11g`, else
  `11b`, else `?`.
- `firmware_version`: the app description's version; `firmware_build`:
  `firmware_id()`; `uptime_s`: `esp_timer_get_time() / 1000000`.
- `reset_reason`: `unknown`, `poweron`, `external`, `software`, `panic`,
  `int_wdt`, `task_wdt`, `other_wdt`, `deepsleep`, `brownout`, `sdio` for
  `ESP_RST_UNKNOWN` … `ESP_RST_SDIO` in ESP-IDF order; any other value
  `unknown` (decided).

### GET /api/wifi/scan [black box]

- `web_audio_pause(req, "Wi-Fi scan", &paused)`; not `ESP_OK`: return.
- `wifi_scan()`. On success:
  `{"networks":[{"ssid":"HomeNet","rssi":-40,"channel":1},…],"success":true}`,
  every record in scan order (no limit, empty SSIDs too), then the list
  freed. On an error: `{"success":false,"error":"<esp_err_to_name>"}`.
- Then, when `paused`, `web_audio_resume(req, "Wi-Fi scan")`; not `ESP_OK`:
  return without a JSON answer (it has answered).
- `application/json`, 200 in both cases.

### POST /api/wifi/config [black box]

- Body: `web_recv_json()` into 512 bytes (at most 511); not `ESP_OK`:
  `400 Bad Request` "Invalid body".
- `ssid` a JSON string, else `{"success":false,"error":"Invalid SSID"}`
  (also for a body that is not JSON). `password` a string, else "".
- `settings_set_wifi_credentials(ssid, password)`; an error:
  `{"success":false,"error":"<esp_err_to_name>"}`. Success:
  `{"success":true}`, then 500 ms, then `esp_restart()`.

### POST /api/device/name [black box]

- Body into 256 bytes (at most 255); not `ESP_OK`: 400 "Invalid body".
- `name` a JSON string, else `{"success":false,"error":"Invalid name"}`.
- `settings_set_device_name(name)`; success: `wifi_set_hostname(name)` and
  `{"success":true}`; error: `{"success":false,"error":"<esp_err_to_name>"}`.
  The page says "Restart to apply" [pages].

### POST /api/system/restart [black box]

`Restarting\n` (no type set), then 200 ms, then `esp_restart()`.

### POST /api/ota/update [black box]

- `web_role_source_stop()` (its error is logged, decided), then
  `ota_start_from_http(req)`.
- Success: `Firmware update complete, rebooting now!\n`, 500 ms,
  `esp_restart()`.
- Error: `ESP_LOGE` `OTA failed (<err>)`, `web_role_source_start()`, then
  `500 Internal Server Error` with `esp_err_to_name(err)` as the body;
  the handler returns `ESP_FAIL`.

### Speed test [black box, pages]

- `ping`: `text/plain`, `Cache-Control: no-store`, `ok`.
- `download?bytes=<n>`: `application/octet-stream`, `Cache-Control:
  no-store`, `n` bytes in chunks of at most 2048. `n` missing, not a
  number, or below 1: 1 MiB (1048576); above 16 MiB: 16 MiB (16777216)
  [black box: 1048576 and 999999999 → those; the page adds `&_=<time>`].
  The content of the bytes does not matter (decided: zeros).
- `upload`: reads and drops the whole body; `text/plain`,
  `received=<bytes read>`.

### Captive portal [black box, board]

- The four URIs above: `302 Found`, `Location: http://192.168.4.1/`, no
  body (phones open the set-up page from them).
- Any other URI (the 404 handler): when the station is connected
  (`wifi_is_connected()`), `404 Not Found`, `text/plain`, `Not Found`;
  else (the set-up access point) the same 302 redirect.

## Helpers (web_server_internal.h)

### web_serve_page(req, start, end)

As "Pages" above, for any embedded page; `ESP_OK`, or the send error
(decided).

### web_recv_json(req, buf, cap) [black box]

- `content_len` 0 or `>= cap`: `ESP_ERR_INVALID_SIZE`, nothing read, no
  response.
- Reads exactly `content_len` bytes. `HTTPD_SOCK_ERR_TIMEOUT`: tries again,
  at most 3 timeouts in all (decided; the current code waits for ever).
  Another error, or the peer closing early: `ESP_FAIL`, no response.
- Success: `buf` NUL-terminated, `ESP_OK`. No response is sent in any case.

### web_send_json(req, r) [black box]

`Content-Type: application/json`, the unformatted JSON of `r`, then
`cJSON_Delete(r)`. `r` NULL: `{}`.

### web_admin_ok(req) [black box]

- Empty `CONFIG_AIRPLAY_WEB_ADMIN_PASSWORD`: true, nothing read.
- Else true only for `Authorization: Basic <base64 of admin:<password>>`
  (user exactly `admin`, password exact). Anything else (no header,
  another scheme, bad base64, other user or password): `401 Unauthorized`,
  `WWW-Authenticate: Basic realm="AirPlay ESP32"`, body
  `Authentication required\n`, false.
- The compare takes the same time for any password of the same length
  (decided).

### web_log_memory(where) (decided)

One `ESP_LOGI` line: `MEM <where> internalFree=<KiB> internalLargest=<KiB>
psramFree=<KiB> psramLargest=<KiB>` (`heap_caps_*` with
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` and `MALLOC_CAP_SPIRAM`).

### web_audio_pause(req, what, &paused) [black box]

- Audio engine not initialised (`audio_receiver_is_initialized()` false):
  `paused = false`, `ESP_OK`, nothing else.
- Else: `web_role_source_stop()`; then wait until `web_role_source_idle()`
  is true, checking every 50 ms, at most 30 s. Still not idle: `ESP_LOGE`
  `<what>: audio source did not stop; restarting` (decided wording),
  `503 Service
  Unavailable`, `text/plain`, `AirPlay clients are still stopping;
  recovering service`, 250 ms, `esp_restart()`, `ESP_FAIL`.
- Then `audio_receiver_release_for_wifi_scan()`; an error: `ESP_LOGE`,
  `web_role_source_start()`, `500` with `<what>: audio memory not
  released`, `ESP_FAIL` (decided).
- Success: `paused = true`, `ESP_OK`.

### web_audio_resume(req, what) [black box]

`audio_receiver_init()`, then `web_role_source_start()`. Either failing:
`ESP_LOGE` `<what>: audio restore failed (<err>); restarting`, `500` with
`Audio restore failed; restarting`, 250 ms, `esp_restart()`, `ESP_FAIL`
(decided wording).

## Answers to the first questions (decided)

1. A handler that has already answered (401, a failed `web_audio_pause` /
   `web_audio_resume`, the 404 handler) returns `ESP_OK`.
2. After `400 Invalid body` the handler returns `ESP_FAIL` (the socket is
   closed; the body may be half read).
3. `web_recv_json()` and the upload after the third receive timeout:
   `ESP_FAIL`.
4. The upload uses the same rule of at most 3 timeouts.
5. `web_audio_resume()`: when `audio_receiver_init()` fails,
   `web_role_source_start()` is not called (the ESP restarts).
6. `bytes=` that is not a plain decimal number (`12abc`, `1.5`, `+5`,
   ` 5`): 1 MiB.
7. JSON bodies are parsed with `cJSON_Parse()`, as now: data after the
   first complete JSON value is ignored (the current code calls
   `cJSON_Parse()` too [black box]).
8. Out of memory while building or printing a JSON answer: the scan
   answers the error JSON with `ESP_ERR_NO_MEM`; a failed print answers
   `500` "Out of memory".
9. A failed `httpd_register_uri_handler()` for a shared handler is logged
   and the start goes on.
10. A second `web_server_start()` after a failed first one is allowed; the
    "started" state is set only on success.
11. `Authorization`: the scheme `Basic` in any case, one or more spaces,
    trailing white space ignored; only canonical padded base64.
12. SSIDs go into the JSON as their bytes are (cJSON escapes control
    characters; other bytes are not checked).

## Tests on a computer

With fakes of the HTTP server (registered handlers, requests with query,
headers and body, responses recorded), of the firmware functions and the
three pages: every example and rule above; the admin check with and
without a password and with broken headers; bodies of 0, 255, 256, 511,
512 bytes; a body shorter than `content_len`; receive timeouts; download
sizes; a page of 10000 bytes; a scan with 0 and 40 networks and an error;
a source that does not stop. None may crash or read past a buffer.

## Open points

None.
