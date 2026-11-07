# helpers: session events, sockets and captive DNS

## Purpose

Three small parts used by two ESP32-S3 firmwares (ESP-IDF v6.0.2), a
Gateway and a speaker:

- **rtsp_events**: tells listeners when an AirPlay session starts, plays,
  pauses and ends.
- **socket_utils**: opens bound IPv4 UDP sockets and listening TCP
  sockets.
- **dns_server**: the captive DNS of the setup access point.

The interfaces are in `deps/rtsp_events.h`, `deps/socket_utils.h` and
`deps/dns_server.h`. Names, types and signatures stay as they are; other
code already calls them. The public headers go to `include/` with these
three names.

Sources: **[callers]** the code that calls these functions in the
firmwares; **[ESP-IDF]** ESP-IDF v6.0.2 and its lwIP; **[RFC 1035]**,
**[RFC 6891]** (EDNS). Rules marked "decided" were chosen for this
firmware where no source shows the behaviour.

## Placement

An ESP-IDF component `helpers` (Apache-2.0): sources in `src/`, the three
public headers in `include/` (its only `INCLUDE_DIRS`). It needs only
ESP-IDF components (`lwip`, `log`, `freertos`, `esp_common`). Code that
needs no ESP-IDF (the DNS message rules, the listener list) is kept apart
so that host tests on a computer can run it; the socket functions can be
tested on a computer with POSIX sockets.

## Environment

- ESP32-S3, dual core. Internal RAM is scarce; PSRAM is 8 MB.
  `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` and
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`: task stacks may be in
  PSRAM. A task with its stack in PSRAM must not write or erase flash
  (ESP-IDF asserts); none of these parts does.
- lwIP: IPv4 is used (IPv6 is compiled in but has no address);
  `CONFIG_LWIP_SO_REUSE=y`, `CONFIG_LWIP_SO_REUSE_RXTOALL=y`,
  `CONFIG_LWIP_SO_RCVBUF=y`; 32 sockets on the Gateway, 16 on the speaker
  [ESP-IDF, sdkconfig].

## rtsp_events

Who emits [callers]: the Gateway's RTSP server (from its connection tasks,
stacks in PSRAM) and the speaker's link to the Gateway (from its own
task). Who listens: the LED code, registered once at start-up. Events:
`RTSP_EVENT_CLIENT_CONNECTED` when a sender's session starts,
`RTSP_EVENT_PLAYING`, `RTSP_EVENT_PAUSED`, `RTSP_EVENT_DISCONNECTED` when
the session ends.

- `rtsp_events_register(callback, user_data)`:
  - `callback` NULL: -1.
  - The same `callback` with the same `user_data` already registered: 0,
    kept once (decided).
  - Room for 8 listeners (decided); a ninth: -1.
  - Otherwise 0; the listener is added after the others.
- `rtsp_events_emit(event)`: calls each registered callback once with
  `event` and its `user_data`, in registration order, in the caller's task,
  before it returns. With no listener it does nothing.
- Safe to call from several tasks on both cores at once: the list is
  protected while it is read or changed, and callbacks are called without
  that protection held, so a callback may itself emit or register
  (decided). A listener registered while an emit is running may or may not
  get that event.
- There is no unregister.
- No log output.

## socket_utils

Both functions open an IPv4 socket (`AF_INET`) bound to `INADDR_ANY` and
the given port; port 0 lets the system pick a free one. On success they
return the socket and, when `bound_port` is not NULL, write the port
actually bound (read back from the socket, also when a port was asked).
On any failure they close what they opened, leave `*bound_port` unchanged
and return -1. No log output: the callers log their own failures
[callers].

### `socket_utils_bind_udp(port, recv_timeout_sec, recvbuf_size, bound_port)`

- A UDP socket (`SOCK_DGRAM`).
- `recv_timeout_sec > 0`: `SO_RCVTIMEO` of that many seconds.
- `recvbuf_size > 0`: `SO_RCVBUF` of that many bytes.
- No `SO_REUSEADDR` (decided): a port another socket holds must fail,
  because callers bind exact ports and treat a different or failed port
  as an error [callers]; with `SO_REUSE_RXTOALL` a shared port would
  silently get both sockets' packets.
- An option that cannot be set is a failure (-1).
- Calls seen [callers]: `(0, 0, 0, &p)` twice in a row to find two free
  ports (both sockets open at once, so the two ports differ), then closed;
  `(data_port, 0, 128 * 1024, &b)`, `(control_port, 0, 64 * 1024, &b)`
  and `(port, 0, buffer, &b)` with exact ports.

### `socket_utils_bind_tcp_listener(port, backlog, nonblocking, bound_port)`

- A TCP socket (`SOCK_STREAM`) with `SO_REUSEADDR` set before binding
  (decided), so a fixed port can be listened on again right after its
  previous socket closed.
- `listen(backlog)`; `backlog` as given (seen 1 and 8) [callers].
- `nonblocking`: the listening socket is set `O_NONBLOCK` (`fcntl`).
- Calls seen [callers]: `(7100, 8, true, &b)` for the speakers' control
  port; `(0 or a requested port, 1, true, &b)` for audio streams.

## dns_server

The wifi module starts it with `192.168.4.1` (network byte order) when the
setup access point turns on and stops it when the access point turns off;
never two starts in a row, and both only from one task, the default event
loop task where its state machine runs [callers]. So start and stop need
no lock (decided). Phones that join the access point
look up names (for example `captive.apple.com`) and must get
`192.168.4.1`, so that they open the setup page by themselves.

### Start and stop

- `dns_server_start(redirect_ip)`: opens a UDP socket on port 53 of every
  local address (IPv4) and starts one task that answers queries. Returns
  `ESP_OK` when the socket is bound and the task runs;
  `ESP_ERR_INVALID_STATE` while it already runs (decided); `ESP_FAIL` when
  the socket cannot be opened or bound; `ESP_ERR_NO_MEM` when the task
  cannot be created. On failure nothing stays open or running.
- `dns_server_stop()`: ends the task and closes the socket, and returns
  only when both are done, so that a start right after it can bind port
  53 again (decided). It waits at most 2 s for the task; the task notices
  a stop within 200 ms (for example a receive timeout of 200 ms). Not
  running: returns at once.
- If the task has not ended after 2 s, which only a fault can cause:
  the stop logs an error and returns, and leaves the task and its socket
  as they are (it does not delete a task that may be inside a socket
  call). The server then counts as running: a later stop tries again and
  a start returns `ESP_ERR_INVALID_STATE` (decided).
- Task: stack 4 KiB in PSRAM (`xTaskCreateWithCaps` with
  `MALLOC_CAP_SPIRAM`), priority `tskIDLE_PRIORITY + 1` (decided). A
  receive buffer of 513 bytes, so that a datagram over 512 bytes shows
  as 513 bytes read and is dropped, and a reply buffer of 512 bytes
  (decided).
- `SO_RCVTIMEO` takes a `struct timeval`: both firmwares keep lwIP's
  `LWIP_SO_SNDRCVTIMEO_NONSTANDARD` at its default 0 [ESP-IDF v6.0.2].
- A receive error other than the timeout (for example while the access
  point goes down): the task waits 20 ms and receives again, without a
  log (decided).
- Log, tag `dns_server`: one line at start (`captive DNS on UDP 53,
  answers 192.168.4.1`) and one at stop. Nothing per query.

### Which queries get a reply [RFC 1035, decided]

A datagram gets a reply only when all of these hold; otherwise it is
dropped without a reply:

- At least 12 bytes (the header) and at most 512.
- QR = 0 (a query), OPCODE = 0 (standard query).
- QDCOUNT = 1.
- The question parses: labels of 1 to 63 bytes, a name of at most 255
  bytes in wire form, ending with a zero byte, no compression pointer
  (a byte with the two top bits set), then QTYPE and QCLASS, all inside
  the datagram. The root name (only the zero byte) has no labels and
  is accepted like any other name (decided).

The query's TC, AA, Z and RCODE bits are ignored (decided). ANCOUNT,
NSCOUNT and ARCOUNT may be anything and the bytes after the question are
ignored: iPhones send an EDNS `OPT` record in the additional
section [RFC 6891] (ARCOUNT = 1); the reply has no `OPT` record.

### The reply [RFC 1035, decided]

- Header: the query's ID; QR = 1, OPCODE = 0, AA = 1, TC = 0, RD copied
  from the query, RA = 0, Z = 0, RCODE = 0 (NOERROR); QDCOUNT = 1,
  NSCOUNT = 0, ARCOUNT = 0.
- The question, copied byte for byte from the query.
- QCLASS IN (1) and QTYPE A (1) or ANY (255): ANCOUNT = 1 and one answer:
  name `C0 0C` (a pointer to the question's name), TYPE A, CLASS IN, TTL
  60, RDLENGTH 4, the four bytes of `redirect_ip` as given (network byte
  order).
- Any other QTYPE or QCLASS (AAAA, HTTPS, ...): ANCOUNT = 0, NOERROR, so
  the phone falls back to the IPv4 address (decided).
- Sent back to the address and port the query came from.

### Examples

A query for `captive.apple.com`, type A, ID 0x1a2b, RD = 1, with an EDNS
`OPT` record (46 bytes):

```
1a2b010000010000000000010763617074697665056170706c6503636f6d000001000100002904d0000000000000
```

Its reply with `redirect_ip` 192.168.4.1 (51 bytes):

```
1a2b850000010001000000000763617074697665056170706c6503636f6d0000010001c00c000100010000003c0004c0a80401
```

A query for the same name, type AAAA (28), ID 0x1a2c, RD = 1, no
additional record (35 bytes):

```
1a2c010000010000000000000763617074697665056170706c6503636f6d00001c0001
```

Its reply (35 bytes):

```
1a2c850000010000000000000763617074697665056170706c6503636f6d00001c0001
```

## Tests on a computer

The tests check the examples above byte for byte, the listener rules of
`rtsp_events` (order, duplicates, the limit, NULL, emits from several
threads), the socket functions (port 0, an exact port, a port already
taken, the options set), and damaged DNS input: every truncation of the
example queries, QR = 1, other opcodes, QDCOUNT 0 and 2, a label of 64
bytes, a name over 255 bytes, a compression pointer, a datagram over 512
bytes, random bytes. None of these may crash or read past the datagram.

## Open points

None.
