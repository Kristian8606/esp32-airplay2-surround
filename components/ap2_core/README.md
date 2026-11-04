# ap2_core

The AirPlay 2 protocol core of
[esp32-airplay2-surround](https://github.com/Kristian8606/esp32-airplay2-surround),
an ESP-IDF component under the **Apache License 2.0** ([`LICENSE`](LICENSE),
[`NOTICE`](NOTICE)).

It takes over the protocol parts of the firmware one at a time: first
pairing (HAP) and plist, later RTSP. Where its code comes from is recorded in
[`PROVENANCE.md`](PROVENANCE.md).

The rest of the repository is under a different license; see the
repository's README.

## Layout

- `include/`: the public API
- `src/`: the implementation
- `tests/`: tests that run on a computer
