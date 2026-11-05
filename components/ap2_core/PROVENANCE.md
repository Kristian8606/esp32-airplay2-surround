# Provenance

How the code in `ap2_core` was made and checked.

## Use of AI

The code was written with Claude (Anthropic) in Claude Code, under the
author's direction.

## Tests

The host tests in `tests/` run the pairing and plist code on a computer
against independent Python clients and parsers, with malformed input under
the sanitizers; `tests/fairplay_host` checks the FairPlay replies against
the published tables; `tests/core_host` in the repository does the same for audio
decryption.

On the boards (2026-11-04), with an Apple TV and an iPhone: pairing, 7.1 and
stereo, pause, skip and volume, every RTSP reply 200 OK.

## Reviewed and changed by the author
