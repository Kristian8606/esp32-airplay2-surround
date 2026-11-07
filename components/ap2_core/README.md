# ap2_core

The AirPlay 2 protocol core of
[esp32-airplay2-surround](https://github.com/Kristian8606/esp32-airplay2-surround),
an ESP-IDF component under the **Apache License 2.0** ([`LICENSE`](LICENSE),
[`NOTICE`](NOTICE)).

It takes over the protocol parts of the firmware one at a time: pairing
(HAP), plist, audio decryption, the FairPlay setup, the RTSP server and
the mDNS announcement. The behaviour of the last two is described in the
repository's `docs/rtsp-spec.md` and `docs/mdns-airplay-spec.md`. Where its code comes from is recorded in
[`PROVENANCE.md`](PROVENANCE.md).

The repository's README lists the licenses of all its parts.

## Layout

- `include/`: the public API
- `src/`: the implementation

## FairPlay setup

Apple senders post two FairPlay messages to `/fp-setup` before they send
audio. Every message starts with a 12-byte header: `FPLY`, version `3`,
`1`, the message number, four zero bytes and the length of the rest.

| Message | From | Length | Content |
|---|---|---|---|
| 1 | sender | 16 | the mode, 0 to 3, at byte 14 |
| 2 | receiver | 142 | the fixed reply of that mode |
| 3 | sender | 164 | the key message |
| 4 | receiver | 32 | a fixed 12-byte header and the last 20 bytes of message 3 |

`ap2_fairplay_setup()` (`include/ap2_fairplay.h`, `src/fairplay/`) gives
messages 2 and 4. It does no FairPlay cryptography: with AirPlay 2 pairing
the audio key comes over the paired channel, and the sender only needs the
expected answers. In FairPlay itself, message 3 carries what decrypts an
AES key sent with FairPlay (`ekey`); this receiver does not use it.

The four replies and the header are fixed interop data, the same in the
open AirPlay receivers: first published in
[playfair](https://github.com/EstebanKubata/playfair) (2015), a
reverse-engineered FairPlay setup that precomputed them with fixed key
material. They are in
`src/fairplay/fairplay_tables.c`, outside the Apache License (see
[`NOTICE`](NOTICE)).
