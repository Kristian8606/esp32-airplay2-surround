# log_stream

The live log of both firmwares: everything logged through ESP-IDF logging
goes to the browsers on the WebSocket `/ws/logs` (the `/logs` page), the
recent 128 KiB first; the UART log is unchanged. An ESP-IDF component under
the **Apache License 2.0** ([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)).

- `include/log_stream.h`: `log_stream_init()` early in start-up,
  `log_stream_register()` once the web server runs
- `src/ls_core.c`: history, browser positions and UTF-8 cleaning, without
  ESP-IDF
- `src/log_stream.c`: the logging hook and the WebSocket sending
- `tests/`: host tests of `ls_core.c` (`make -C tests`)
