# Provenance

How the code in `ap2_core` was made and checked.

## Use of AI

The code was written with Claude (Anthropic) in Claude Code, under the
author's direction.

## RTSP server

`src/rtsp/` was written in a separate Claude Code session that had only a
behaviour specification (`docs/rtsp-spec.md` in the repository) and the
headers of the interfaces it calls. The specification was written from
logs and black-box tests of the receiver, from public protocol
documentation and from the behaviour of an open-source AirPlay 2 receiver;
it describes behaviour only and contains no code. Questions from that
session were answered by changing the specification. Measured against the history of the code it
replaces, 5% of its lines of 20 or more characters also appear there
(includes, calls of the firmware's interfaces, socket set-up, method
names and function signatures).

## mDNS announcement

`src/mdns/` was written the same way, in a separate session with only a
behaviour specification (`docs/mdns-airplay-spec.md`) and the headers it
calls. The specification was written from the records a running Gateway
announces, black-box tests of the receiver, the call sites, the public API
of the Espressif `mdns` component and RFC 1035/6763. Measured against the
history of the code it replaces, 10% of its lines of 20 or more characters
also appear there (includes, the function name, log tag, `mdns_init()` and
the TXT keys with their identity macros).

## Tests

The host tests in `tests/` run the pairing and plist code on a computer
against independent Python clients and parsers, with malformed input under
the sanitizers; `tests/fairplay_host` checks the FairPlay replies against
the published tables; `tests/core_host` in the repository does the same for audio
decryption. `tests/rtsp_host` runs the RTSP server's request rules, framing
and remote commands against fakes of the firmware (4534 checks).
`tests/mdns_host` checks the mDNS calls, names and TXT records against
fakes, with cut and invalid names (198 checks).

On the boards (2026-11-04), with an Apple TV and an iPhone: pairing, 7.1 and
stereo, pause, skip and volume, every RTSP reply 200 OK.

The new RTSP server on the boards (2026-11-06, v4.5.56), with an Apple TV,
an iPhone and three speakers: buffered 7.1 and stereo, realtime ALAC,
format changes, volume, and a second sender taking the session in both
directions.

## Reviewed and changed by the author
