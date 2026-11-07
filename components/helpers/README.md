# helpers

Small parts that both firmwares use. An ESP-IDF component under the
**Apache License 2.0** ([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)).

- `include/rtsp_events.h`: up to 8 listeners told when a sender connects,
  plays, pauses and disconnects, in the task that reports it
- `include/socket_utils.h`: bound IPv4 UDP sockets and listening TCP sockets
- `include/dns_server.h`: the captive DNS of the setup access point; every
  name gets one IPv4 address
- `src/dns_message.c`: the DNS query and reply rules, without ESP-IDF

Behaviour: [`docs/helpers-spec.md`](../../docs/helpers-spec.md).
