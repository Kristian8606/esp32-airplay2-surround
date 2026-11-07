# RTSP server of the Gateway: behaviour

It says what the Gateway's AirPlay 2 control server must
do, as seen from the senders (Apple TV, iPhone, iPad, Mac) and from the rest
of the firmware. Sources, marked where a rule rests on one of them:

- **[log]** logs of real sessions (2026-11-04, Apple TV and iPhone, every
  request and reply, and what each request made the Gateway do);
- **[probe]** a test sender run against a running Gateway (2026-11-06):
  transient pairing, the encrypted channel, the requests below, a second
  sender;
- **[other receiver]** what another open AirPlay 2 receiver does;
- **[public notes]** public notes on the AirPlay 2 protocol;
- **[ESP-IDF]** the ESP-IDF v6 sources and documentation.

Rules marked "decided" were chosen for this Gateway where no source shows
the behaviour.

## Purpose

The Gateway is the AirPlay 2 receiver the senders see. It plays nothing
itself: it accepts the session, takes the audio, and passes every playback
decision to the audio engine interface (`sound_engine.h`), which the
Gateway implements by forwarding to its speakers. This server is the
control side: the RTSP connection on TCP port 7000, pairing, the encrypted
control channel, the event channel back to the sender, and the
translation of each request into calls of the interfaces below.

**One sender plays at a time.** Others may connect, ask `/info` and pair.

## Placement

The code goes into the ESP-IDF component `ap2_core` (Apache-2.0), next to
the pairing, plist and FairPlay code it uses: sources in `src/rtsp/`, the
public headers `rtsp_server.h` and `rtsp_remote.h` in `include/`. The
other headers in `deps/` belong to the firmware (audio engine, clock,
amplifier, events, settings, identity) or to `ap2_core` itself
(`ap2_hap.h`, `ap2_plist.h`, `ap2_tlv8.h`, `ap2_fairplay.h`); the firmware
gives their include paths and links them. Code that needs no ESP-IDF
(request parsing, the request rules, the volume and format tables) is kept
apart so that it builds and runs on a computer. `ap2_core` already depends
on libsodium (`espressif/libsodium` ^1.0.21 on the board; on a computer the
system's libsodium, e.g. Homebrew 1.0.22, linked with `-lsodium`).

## Interface to the firmware

The server offers:

| Function | Behaviour |
|---|---|
| `esp_err_t rtsp_server_start(void)` | Listens on TCP 7000 and serves senders until stopped. Called at start-up and again after a stop. A second start while running returns `ESP_OK` and does nothing; a start while a stop is still under way returns `ESP_ERR_INVALID_STATE`. The firmware calls `ap2_hap_identity_init()` before the first start; the server does not. That call also starts libsodium (`sodium_init()`), so the server does not call `sodium_init()` either; the host tests call it once in their own start-up. |
| `void rtsp_server_stop(void)` | Closes the listener and every connection, ends a running session as a TEARDOWN would, and waits until `rtsp_server_is_idle()` is true, at most 2 s (seen: about 0.1 s) [log]. The web page's firmware update checks `rtsp_server_is_idle()` right after the call, without waiting, and refuses the update when it is false; the room calibration polls. Never called from the server's own tasks. |
| `bool rtsp_server_is_idle(void)` | True only when nothing of the server runs any more (no task, no socket, no session). Callers poll it after a stop before they take the audio engine or the radio for something else (firmware update, room calibration). |
| `esp_err_t rtsp_remote_enqueue(uint8_t command, uint32_t *id)` | Queues one remote command for the sender (below). Checked in this order: `ESP_ERR_INVALID_ARG` for a number over 7, `ESP_ERR_NOT_FOUND` without an encrypted event connection of the session holder, `ESP_ERR_INVALID_STATE` while 8 commands are queued and not sent yet. `*id` numbers the command (after 2³²−1 it starts again at 1). It only queues; the sending happens on the event connection's own task. |
| `void rtsp_remote_get_status(rtsp_remote_status_t *status)` | Whether the session holder has an event connection, the last command's id, number and result, and the sender's response code. |
| `const char *rtsp_remote_result_name(rtsp_remote_result_t result)` | A fixed lower-case name per result. |

Remote command numbers: 0 play, 1 pause, 2 play/pause toggle, 3 stop, 4 next,
5 previous, 6 volume down, 7 volume up (the web page sends them by these
names). Results: idle, queued, sent, acknowledged, rejected, timeout, failed,
disconnected.

The server uses:

- **Audio engine** (`sound_engine.h`): set stream type, format,
  encryption key, start realtime or buffered reception, stop, volume (Q15
  gain), the flush calls, pause, playing on/off, the anchor time, the
  sender's control address, the playout latency, whether the engine is idle.
- **Sender clock** (`apple_clock.h`): select the sender's PTP clock when a
  session starts, release it when it ends; read the clock to map an anchor.
- **Amplifier** (`amp_control.h`): a session connected / disconnected.
- **Events** (`rtsp_events.h`): announce session and playback changes to the
  rest of the firmware (LED, web page).
- **Settings** (`settings.h`): the device name, the saved volume (read at
  session start, written to flash when the session ends; see "Flash"
  below).
- **AirPlay 2 core** (`ap2_core`): pairing (setup, verify, transient), the
  channel keys, binary plist reading and writing, the FairPlay setup reply,
  the device identity (pairing id, public key).

## Connections and messages

- TCP 7000 over IPv4 only: every sender in the logs came over IPv4, and the
  Gateway has no IPv6 address (the network code creates none, so none is
  announced). Several connections at once (a sender often opens a second
  one), up to 8; a ninth is closed at once (decided). Each connection is
  served on its own; a connection ends when the sender closes it or on a
  fatal error.
- Requests are RTSP/1.0 style: a request line (method, path, version), headers,
  an empty line, then `Content-Length` bytes of body. Methods are compared
  exactly (upper case); header names are case-insensitive. Any version token
  is accepted (`HTTP/1.1` too); the reply is always `RTSP/1.0` (decided).
- Every reply: `RTSP/1.0 <code> <text>`, `CSeq` copied from the request (a
  request without `CSeq` gets a reply without it), `Server:
  AirTunes/377.40.00`, then `Content-Type` and `Content-Length` when there
  is a body.
- `OPTIONS` answers 200 with `Public: SETUP, RECORD, PAUSE, FLUSH,
  FLUSHBUFFERED, TEARDOWN, OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER,
  SETPEERS, SETPEERSX, SETRATEANCHORTIME, LOUDNESSNORMALIZATION`.
- An unknown method, or `GET`/`POST` to a path not listed below: `501 Not
  Implemented`, `Content-Type: text/plain`, body `Not Implemented` (the other
  receiver answers 501 to an unknown path too).
- Requests on one connection are answered in order. The largest body seen
  is 7639 bytes (`/command`); no artwork (`image/*`) was seen. Bodies up to
  64 KiB are accepted; a larger one is read and dropped and answered `413
  Request Entity Too Large`, and the connection stays (decided).

### Reply codes

| Code | When |
|---|---|
| 200 | Everything below unless another code is named. |
| 400 Bad Request | A request whose data is missing or wrong (named per request below); nothing changes. |
| 400 Bad Request + `Connection: close`, then the connection closes | A head that cannot be read: a bad request line (also one without a version), a header line without `:`, folded header lines, a bad or repeated `Content-Length`, a head over 4 KiB or over 32 headers, control bytes. The rest of the stream cannot be trusted (decided). |
| 413 Request Entity Too Large | A body over 64 KiB (above). |
| 455 Method Not Valid in This State | A `SETUP` with a stream on a connection that has not sent the session `SETUP`; nothing changes. |
| 470 Connection Authorization Required | A pairing step the AirPlay 2 core fails (below) [other receiver]; a `SETUP` on a connection that is not encrypted (decided); nothing changes. |
| 500 Internal Server Error | The Gateway itself fails: no memory, the event port cannot be reserved or opened, no free UDP port pair, the engine does not start reception, no audio key, the reply cannot be built, the engine refuses a deferred flush. |
| 501 Not Implemented | Above. |

## Encryption

- Before pairing: plain text.
- After a successful pair-verify on a connection, that connection switches
  to the encrypted HAP channel in both directions, starting with the next
  request: frames of a 2-byte little-endian length (at most 1024), that many
  bytes sealed with ChaCha20-Poly1305 and a 16-byte tag, the length as
  associated data, a 64-bit counter per direction as nonce (four zero bytes
  and the counter, little-endian). The keys come from the pairing's shared
  secret (for transient pairing, the SRP session key) by HKDF-SHA512 with
  salt `Control-Salt`: info `Control-Write-Encryption-Key` for sender to
  Gateway, `Control-Read-Encryption-Key` for Gateway to sender [probe]. A
  frame that does not open closes the connection. [public notes]
- Every message the Gateway sends, on either channel, goes out as one
  frame when it is 1024 bytes or less, and otherwise in frames of 1024
  bytes with the rest in the last; never the head and the body in frames
  of their own. The previous firmware sent each event message in one
  frame; a remote command sent as two frames (head, then body) got no
  reply from an Apple TV [log] (whether one frame changes that is not yet
  seen).
- The AirPlay 2 core already derives exactly these two keys when pairing
  ends and keeps both counters (from 0): use `ap2_hap_channel_key()`,
  `ap2_hap_channel_counter()` and `ap2_hap_channel_count()` (after each
  frame) for the control channel; do not derive them again.
- Seal and open frames with libsodium (`sodium.h`):
  `crypto_aead_chacha20poly1305_ietf_encrypt_detached()` and
  `crypto_aead_chacha20poly1305_ietf_decrypt_detached()`, the library the
  core uses for the same cipher. No own implementation of ChaCha20 or
  Poly1305 (decided).
- The event channel uses the same framing with salt `Events-Salt`: info
  `Events-Write-Encryption-Key` for Gateway to sender,
  `Events-Read-Encryption-Key` for sender to Gateway [probe] (the opposite
  naming of the control channel). The core does not keep these: derive them
  with `ap2_hkdf_sha512()` from `ap2_hap_pairing_secret()`, with the event
  connection's own counters from 0.
- Only the TLV8 pair-verify is served. The older form without TLV8
  (`ap2_hap_verify_raw_*`) is not used: it leaves the channel in the clear,
  and a sender using it would go on with AirPlay 1 requests (`ANNOUNCE`)
  that this server does not offer.
- The audio key is the 32-byte `shk` of the stream's SETUP; it goes to the
  audio engine as ChaCha20-Poly1305. Before that, at the session SETUP, the
  engine gets the 32-byte audio key the AirPlay 2 core derives from the
  pairing (`ap2_hap_audio_key()`); every log shows a 32-byte key there.

## A session

The order seen with both senders:

1. `GET /info` (plain).
2. `POST /pair-setup` (transient pairing, two exchanges) or
   `POST /pair-verify` (two exchanges). From here the connection is
   encrypted.
3. `POST /fp-setup` twice.
4. `SETUP` without streams: the session. If no other sender holds the
   session, this sender takes it: the Gateway selects the sender's clock,
   opens the event port, sets the saved volume on the engine, tells the
   amplifier a session is connected.
5. `GET /info`, `RECORD`, `SETPEERS`, `POST /command`, ...
6. `SETUP` with one stream: realtime (type 96) or buffered (type 103).
7. `SETRATEANCHORTIME` with rate 1: play from the anchor. Rate 0: pause.
8. `SET_PARAMETER` (volume, metadata, progress), `POST /feedback` every 2 s,
   `FLUSHBUFFERED` / `FLUSH` at skips and track changes, `POST /audioMode`,
   `LOUDNESSNORMALIZATION`.
9. `TEARDOWN` with the stream: the stream ends; a new `SETUP` with a stream
   may follow on the same session (format change, e.g. ALAC to AAC 7.1).
10. `TEARDOWN` without streams, or the connection closes: the session ends.

States: **idle** (no session), **session** (lock held, no stream),
**stream** (one stream set up; paused or playing).

The sender that holds the session is the one whose connection took it.
When that connection closes, the session ends as with `TEARDOWN`.

**Requests from a connection that does not hold the session** (decided):
`SET_PARAMETER`, `SETRATEANCHORTIME`, `FLUSHBUFFERED`, `FLUSH`, `PAUSE`,
`RECORD` and `TEARDOWN` with streams answer 200 and change nothing;
`/feedback` answers 200 without a body; `GET_PARAMETER volume` answers the
current volume. Only the holder drives the engine.

**A second sender** [probe, other receiver, log]: its `SETUP` without
streams is answered as usual and its own event port listens at once, as
for the holder; the first sender keeps the session. A real iPhone connects
to that event port right after the reply while an Apple TV holds the
session, and gives up when the port does not accept [log]; the other
receiver also opens it before the reply. Its `RECORD` and `TEARDOWN`
change nothing (200). Its first `SETUP` with a
stream takes the session: the first sender's stream stops and its
connection is closed, then the second sender's session starts (clock, event
port, volume, amplifier) and its stream is set up. AirPlay 2 receivers
always allow this take-over. After an ignored `TEARDOWN` the Gateway closed
that second connection [probe].

## Requests

All replies are 200 with no body unless said otherwise.

### `GET /info`

Plain or encrypted, with or without a body (a body names a `qualifier`; it
does not change the reply). Reply: binary plist
(`application/x-apple-binary-plist`), as a running Gateway answers:

| Key | Value |
|---|---|
| `deviceid` | the station MAC, `AA:BB:CC:DD:EE:FF` upper case |
| `features` | 496155697629696 (0x1C340401C4A00: bits 9, 11, 14, 18, 19, 20, 30, 38, 40, 41, 46, 47, 48) |
| `model` | `AudioAccessory6,1` |
| `protovers` | `1.1` |
| `srcvers` | `377.40.00` |
| `vv` | 2 |
| `statusFlags` | 4, always: the running Gateway answered 4 also during another sender's session [probe] |
| `pk` | the 32-byte Ed25519 public key of the device identity |
| `pi` | the pairing id (text) |
| `name` | the device name from the settings |
| `audioFormats` | one dict: `type` 96, `audioInputFormats` and `audioOutputFormats` 16777216 (0x1000000) |
| `audioLatencies` | two dicts, `type` 96 and 103, each `audioType` 100, `inputLatencyMicros` 0, `outputLatencyMicros` 0 |
| `initialVolume` | the saved volume in AirPlay units (e.g. -15.0); -24.0 when it cannot be read (the other receiver's default) |
| `supportedFormats` | `audioStream` 262144 (0x40000), `bufferStream` 1649282383872 (0x18000E40000) |

### `POST /pair-setup`, `POST /pair-verify`

TLV8 bodies handed to the AirPlay 2 core; the `State` of the request picks
the step (pair-setup 1, 3, 5; pair-verify 1, 3). Its replies go back as
`application/octet-stream`.

- A client the core refuses with a well-formed message (a wrong proof or
  signature): the core returns `ESP_OK` with its `{State, Error}` reply,
  sent with 200 like any other reply [other receiver: the same].
- Any error the core returns, and a body without a known `State` (empty
  too): `470 Connection Authorization Required`, no body; the connection
  stays [other receiver].
- No memory for the pairing session: 500.

After the last pair-verify step the connection is encrypted (above). A
transient pair-setup also ends in an encrypted connection. Encryption
starts with the reply after the step that makes `ap2_hap_established()`
true. The core sets both channel keys before that and fails the step if
it cannot (470, above); `ap2_hap_channel_key()` returns the session's own
key and is never NULL, so nothing more is checked. The pairing
steps do not touch flash (only `ap2_hap_identity_init()` does, and the
firmware calls it at start-up).

### `POST /fp-setup`

The FairPlay setup reply from the AirPlay 2 core (142 or 32 bytes,
`application/octet-stream`). A message it does not know: one warning line,
reply one byte `0x00`.

### `SETUP` (no `streams` in the body)

Binary plist. Starts the session if no other sender holds it. Only on an
encrypted connection (else 470). A body that does not start with
`bplist00` answers 400. `ap2_bplist_stream_count()` returning false (no
`streams`, or a plist it cannot read) counts as this SETUP: nothing here
depends on what the plist holds, so a damaged one does no harm.

- Reads `timingProtocol` (PTP) and the sender's address (from the socket);
  selects the sender's clock (`apple_clock_select(sender IP, false)`). Any
  other or no `timingProtocol`: one log line, then the same (decided).
- Opens the event port (TCP) for this session.
- Gives the audio engine the encryption key (above) and the saved volume
  (-24.0 when it cannot be read).
- Session connected for the amplifier; a session event.
- Reply: binary plist `{eventPort: <port>, timingPort: 0}` (76 bytes) [log,
  probe]. (another open receiver also returns `timingPeerInfo`; this Gateway does
  not.)
- A sender sending it again on the same connection, holder or not: the
  same `eventPort`, which keeps listening; nothing new opens or starts
  (decided).
- If another sender holds the session: see "A second sender" above.

### `SETUP` (with `streams`)

One stream dict: `type`, `audioFormat`, `shk`, `controlPort` (realtime),
`spf` (frames per packet), `ct` (compression type), `sr`.

Only after the session `SETUP` on the same connection (else 455). 400 and
nothing changes when: the number of streams is not 1; `type` is not 96 or
103; `audioFormat` is missing or not in the table below; `shk` is not 32
bytes; a realtime stream has no `controlPort` from 1 to 65535.

`audioFormat` is one bit of the AirPlay audio format table; the Gateway
offers these (`supportedFormats`: `audioStream` bit 18, `bufferStream` bits
18, 21, 22, 23, 39, 40):

| Bit | `audioFormat` | Stream | Format to the engine (codec, rate, channels, bits, frames) |
|---|---|---|---|
| 18 | 0x40000 | 96 or 103 | ALAC, 44100, 2, 16, 352 [log] |
| 21 | 0x200000 | 103 | ALAC, 48000, 2, 24, 352 |
| 22 | 0x400000 | 103 | AAC, 44100, 2, 24, 1024 |
| 23 | 0x800000 | 103 | AAC, 48000, 2, 24, 1024 [log] |
| 39 | 0x8000000000 | 103 | AAC, 48000, 2, 24, 1024 (5.1) |
| 40 | 0x10000000000 | 103 | AAC, 48000, 2, 24, 1024 (7.1) [log] |

The channel count given to the engine is 2 also for 5.1 and 7.1 (the
decoder finds the channels in the stream) [log]. A format not in the table:
the SETUP answers 400 and nothing changes (decided here; not seen). The
engine gets: stream type, format, key, then

- The sender's clock is selected again with realtime = (type is 96).
- **type 96 (realtime)**: start realtime reception on a free UDP data and
  control port pair, playout latency 11025 samples (0.25 s at 44.1 kHz), the
  sender's control address (its IP and `controlPort`), playing on. Reply:
  `streams` with `type` 96, `dataPort`, `controlPort`, `audioBufferSize` 0.
- **type 103 (buffered)**: start buffered reception on a TCP port, playout
  latency 0, the sender's control address with port 0 (the engine got this
  call for type 103 too [log]; port 0: there is no control port). Reply:
  `streams` with `type` 103, `dataPort`, `controlPort` 0, `audioBufferSize`
  6291456 (6 MiB).

The order the engine got them in [log]: stream type, format, key, start
reception, playout latency, the sender's control address, then (realtime)
playing on.

A stream SETUP while a stream is set up replaces it: playing off and stop
(as a stream `TEARDOWN`, but without an event), then the new one. If the
new stream cannot start (500), the session stays without a stream.

### `RECORD`, `SETPEERS`, `SETPEERSX`, `POST /command`, `POST /audioMode`, `LOUDNESSNORMALIZATION`

200, no body [log, probe]. `/command` bodies are plists with a `type`
(`updateMRSupportedCommands` and others); log the type. Nothing else
changes.

### `SET_PARAMETER`

- `text/parameters` with `volume: <v>` (AirPlay volume, -30.0 to 0.0, or
  -144.0 for mute): gain `((v + 30) / 30)²` as Q15 (32768 = 0 dB), 0 for -144
  or below -30. Seen: -15.0 → 8192, -13.125 → 10368, -18.75 → 4608,
  -24.3385 → 1166 [log]; -20.625 → 3200, -20.0 → 3640 [probe]. A value that
  is not a number: 400. Above 0: taken as 0. From -144 up to -30: gain 0,
  the value kept as it came; below -144: as -144 (decided).
- Every change goes to `settings_set_volume()` (it only keeps it in RAM).
  `settings_persist_volume()` writes it to flash once, when the session
  ends (`TEARDOWN` without streams, the holder's connection closing, a
  take-over, `rtsp_server_stop()`); not at `PAUSE` or a stream `TEARDOWN`
  (decided: the header asks for it only when playback stops, because a
  flash write stalls the caches). The saved volume is the `initialVolume` of
  `/info` and the volume set when a session starts.
- `text/parameters` with `progress: <start>/<current>/<end>`: accepted.
- `application/x-dmap-tagged` (track metadata), `image/*` (artwork):
  accepted, not used.

### `GET_PARAMETER`

`text/parameters` body `volume\r\n`: reply `text/parameters`
`volume: <v>\r\n`, the current AirPlay volume with two decimals
(`volume: -20.62`) [probe]. Any other name: 200, no body [probe]. The two
decimals are what `printf("%.2f")` gives for the value as stored (nearest;
-20.625 is a tie and gives -20.62).

### `PAUSE`

`sound_engine_pause()`, 200 [probe].

### `SETRATEANCHORTIME`

Binary plist: `rate`, `rtpTime`, `networkTimeSecs`, `networkTimeFrac`,
`networkTimeTimelineID`.

Senders also send `networkTimeFlags`.

- `rate` 1: the anchor (clock id, network time in ns = secs × 10⁹ + frac
  × 10⁹ / 2⁶⁴, RTP time) to the engine, then playing on.
- `rate` 0: `sound_engine_pause()` (the anchor fields are 0, or the body
  holds `rate` alone: 53 bytes) [log].
- `rate` came as an integer: the 53-byte body only fits an integer 0, and
  the 190-byte one an integer 1 [log]. A real is accepted as well; another
  open receiver plays when it is odd. Here: non-zero plays.
- `networkTimeTimelineID` and `networkTimeFrac` are unsigned 64-bit values
  and are often above 2⁶³ (timeline `c4168fb7ecf90008` [log]). They came as
  8-byte integers: with the sizes the other values need, the 190-byte body
  fits no other encoding [log]. `ap2_bplist_int()` returns them as
  `int64_t`: use those 64 bits as `uint64_t`. A field the core cannot read:
  400.
- 400 and nothing changes: no `rate`; `rate` non-zero and one of `rtpTime`,
  `networkTimeSecs`, `networkTimeFrac`, `networkTimeTimelineID` missing.

### `FLUSHBUFFERED`

Binary plist with `flushUntilSeq` and `flushUntilTS`, and optionally
`flushFromSeq` and `flushFromTS`.

- Without the "from" pair: immediate flush up to (seq, ts) [log, probe].
- With it: a deferred flush of the range from (seq, ts) to (seq, ts) [log].
- `flushFromSeq` decides: present, deferred; absent, immediate. A deferred
  flush without `flushFromTS` passes `from_ts` 0 and `timestamps_valid`
  false (decided; the logs always had all four).
- No `flushUntilSeq` or `flushUntilTS`: 400, nothing changes.
- The numbers go to the engine as they came, without a mask: seen values
  are over 23 bits (12884298), and the engine got them unchanged [log].

### `FLUSH` (realtime)

`RTP-Info: rtptime=<n>`: flush the realtime stream up to that RTP time; the
current anchor timing stays. Without that header: 400, nothing changes.

### `POST /feedback`

200. While a stream is set up (playing or paused), the body is the binary
plist `{streams: [{type: 103, sr: 48000.0}]}` (85 bytes), the stream's type
and sample rate; without a stream, no body [probe]. another open receiver answers
the same plist, only while playing. The sender sends it every 2 s; it keeps
the session alive [public notes].

### `TEARDOWN`

- With `streams`: playing off (`sound_engine_set_playing(false)`), then
  `sound_engine_stop()`; the session stays, the connection stays open
  [log].
- Without: playing off, stop, end the session: release the session and the
  sender's clock, close the event port, clear the engine's stream type and
  key, session disconnected for the amplifier, a session event, save the
  volume. The reply carries `Connection: close` and the Gateway closes the
  connection after it [probe, other receiver].
- From a connection that does not hold the session: 200, nothing changes
  [probe]; without streams it is also answered with `Connection: close` and
  that connection closes [probe]; with streams it stays open (decided).

## Event channel

The sender connects to the event port from the session SETUP (seen about
30 ms after the reply) and encrypts it as above. The Gateway sends remote
commands on it; the sender never needs to send anything else [probe].

Decided:

- Every session `SETUP` (holder or not) opens its own event port, which
  listens before the reply and as long as that RTSP connection lives
  [other receiver, log]. A new connection to it replaces the old one (a
  command still going on the old one ends `disconnected`).
- Remote commands go only on the event connection of the sender that
  holds the session; `rtsp_remote_enqueue()` and
  `rtsp_remote_get_status()` look only at that one. After a take-over it
  is the new holder's event connection, which is already open.
- A request from the sender on it is ignored. A reply whose `CSeq` is not
  the command's is ignored. A frame that does not open, or a message that
  cannot be read, closes the event connection.

A command is an RTSP request from the Gateway to the sender [probe]:

```
POST /command RTSP/1.0
CSeq: <the command's id>
Content-Type: application/x-apple-binary-plist
Content-Length: <n>
```

with a binary plist body:

| Command | Body |
|---|---|
| 0 play, 1 pause, 2 toggle, 3 stop, 4 next, 5 previous | `{type: "sendMediaRemoteCommand", modernMediaRemoteCommand: <0 to 5>}` (109 bytes) |
| 6 volume down, 7 volume up | `{type: "sendMediaRemoteCommand", value: "dvlc", volume: <f>, params: {volume: <f>}}` (143 bytes) |

The bodies of 0, 1, 2, 3, 5, 6 and 7 were captured; 4 (next) was sent but
not printed, and follows the same numbering.

`f` is the new volume as a fraction, `(v + 30) / 30` of the AirPlay volume
`v`, one step of 1/16 down or up from the current one, kept within 0 to 1
(from -15.0, i.e. 0.5: up gives 0.5625, down 0.5) [probe]. The current one
is the session's volume.

- As soon as a volume command is sent, the Gateway sets that volume on the
  engine itself (−13.125 → Q15 10368) and keeps it as a `SET_PARAMETER`
  volume is kept: `settings_set_volume()`, written to flash when the
  session ends, answered by `GET_PARAMETER volume`. So the next step goes
  on from it: up, up from -15.0 sends 0.5625, then 0.625 [board,
  2026-11-07: an Apple TV shows the new volume on each command but never
  replies and sends no `SET_PARAMETER`; the sound did not change until the
  phone's own volume button made it send one] (decided). A later
  `SET_PARAMETER` volume from the sender wins as usual.

- The command's id counts up from 1 since start-up, across sessions; it is
  also the `CSeq` [probe].
- A reply `RTSP/1.0 200 …` with that `CSeq` makes the result acknowledged;
  another code, rejected. Neither changes the volume again.
- No reply within 3 s: timeout, logged at info level (senders do not
  reply [board]). The event connection stays open; a late reply to an
  earlier command is ignored (its `CSeq` is not the current one). An Apple TV closed its RTSP
  connection, and with it the session, 40 ms after the Gateway closed the
  event connection on a timeout [log] (decided). A send error: failed. The
  event connection or the session ending first: disconnected.
- The Gateway does not wait for a reply. Commands are queued in order, up
  to 8 not yet sent; each new one becomes the current command (the status
  shows it). The event connection's task sends every queued command in
  order, each as its own message with its own `CSeq`; each volume step is
  set before the next is built, so the steps go on from one to the next. A ninth unsent command: `ESP_ERR_INVALID_STATE`.
  When the event connection or the session ends, the unsent commands are
  dropped [board: waiting 3 s for a reply that never comes refused every
  second press of a button] (decided). `rtsp_remote_enqueue()` only queues.
  The web page answers 202 with the id; the page does not wait for the
  result.

## Timeouts

- The Gateway does not close idle connections: a plain connection with no
  request was still open after 130 s [probe].
- The session holder's connection: as long as it stays open; `/feedback`
  arrives every 2 s.
- A remote command without a reply in 3 s: timeout (above).

## Log

Tag `rtsp`. One line per session and stream change (format, ports,
latency), and a warning per refused or unknown request. Only with
`CONFIG_AIRPLAY_LOG_RTSP_REQUESTS` (off by default, not compiled when off):
one line per request (method, path, `CSeq`, body length) and per reply
(code, body length), and the first bytes of each body as text, except the
bodies of `SETUP`, `/pair-setup`, `/pair-verify` and `/fp-setup` (decided:
`/feedback` alone comes every 2 s).
Never keys, `shk` or pairing secrets.

## Memory and tasks

Internal RAM is scarce on the Gateway; buffers for bodies go to PSRAM. One
task listens; each connection and each event channel have their own task
with its stack in PSRAM; they end when their connection ends. Stacks: 16
KiB per connection, 8 KiB per event channel (PSRAM), 4 KiB for the listener
(internal RAM). Pairing fits: the core keeps the SRP numbers and buffers on
the heap, not the stack. The high-water marks are measured on the board
when the code is put into the firmware.

**Flash.** While flash is read or written, the cache is off and PSRAM
cannot be reached; ESP-IDF stops with an assert when a task whose stack is
in PSRAM starts a flash access (`spi_flash/cache_utils.c`) [ESP-IDF]. The
`settings_*` functions may read or write NVS, so every `settings_*` call
(reads too) runs with its stack in internal RAM, for example in a short
task created for it.

## Events and amplifier

- Session start: amplifier session connected; event `CLIENT_CONNECTED`.
- Playing on (SETRATEANCHORTIME rate 1, realtime stream SETUP): `PLAYING`.
- Pause (rate 0, `PAUSE`, stream `TEARDOWN`): `PAUSED`.
- Session end: amplifier session disconnected; `DISCONNECTED`;
  `apple_clock_release()`.

## Open points

None.
