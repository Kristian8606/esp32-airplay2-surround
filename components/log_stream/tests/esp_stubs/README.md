Stand-ins for the ESP-IDF headers that `src/log_stream.c` uses, so that it
builds on a computer. Declarations follow the ESP-IDF v6.0.2 API reference;
they are not the real headers. `sdkconfig.h` sets the options spec/spec.md
says both firmwares use, including
`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT`.

- `make esp-check` only compiles `src/log_stream.c` against them.
- `make test` also links it with `../esp_fake.c`, a fake web server, logging
  and FreeRTOS (one thread, simulated time), and runs `../test_esp.c`. As
  the spec describes ESP-IDF v6.0.2, the fake does not call the WebSocket
  handler for the handshake; it calls the URI's post-handshake callback and
  closes the connection if that fails. It reports any web server call other
  than `httpd_queue_work` made from the module's own task.

The real build is `idf.py build` in a firmware that uses this component.
