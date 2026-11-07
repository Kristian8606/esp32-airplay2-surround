# web_server

The shared web server of both firmwares on port 80: the main page, the
live log and speed test pages, system info, Wi-Fi scan and set-up, device
name, restart, firmware update and the captive portal of the set-up access
point. Each firmware adds its own pages and APIs through
`web_server_internal.h`. An ESP-IDF component under the **Apache License
2.0** ([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)).

- `include/web_server.h`: `web_server_start()`
- `include/web_server_internal.h`: the helpers for each firmware's pages and
  the `web_role_*` functions each firmware implements
- `src/web_rules.c`: JSON answers, the Basic auth check, download sizes,
  reset reason and PHY names, without ESP-IDF
- `src/web_server.c`, `src/web_helpers.c`: the handlers and the helpers

Behaviour: [`docs/web-server-spec.md`](../../docs/web-server-spec.md).
