# Stand-in ESP-IDF headers

Not ESP-IDF. Just enough declarations and types (field lists from the
ESP-IDF v6.0.2 API reference) to compile `src/wifi.c` on a computer for
`make -C tests stack`: `-fstack-usage` frame sizes, `-Wframe-larger-than`
and printf format checks of the log lines. Struct sizes are close to, not
equal to, ESP-IDF's; nothing here is linked or run.
