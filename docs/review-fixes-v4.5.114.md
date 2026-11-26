# Review fixes in v4.5.114

The fixes from the review of v4.5.113 (REW mode, protocol 15) and of the C
side of Room EQ, and what was found while making them. No protocol change:
the speakers stay on protocol 15, and every fix is Gateway-only, page-only or
a speaker-side input check.

## Listed in the review

| # | Where | What was wrong | Fix | Tested |
|---|---|---|---|---|
| 1 | `main/gw_calib.c` (Check levels) | The pink noise holds peaks at 4× RMS. Above −12 dBFS RMS (a speaker limit raised to −3) it clipped at the speaker (17 % of samples at −3), so the microphone's RMS and peak were wrong. | The noise plays at −12 dBFS RMS at most (`GW_CAL_NOISE_MAX_DB10`). RMS and peak are scaled up by the difference, `clipped` comes from the real recorded peak, and the JSON has `estimated` and `played_rms_db`. /roomeq shows "(estimated from −12.0 dBFS RMS)". The log shows both levels. | Builds (S3, P4). The hardware test is in the list below. |
| 2 | `web/rew.js` (REW import) | An RBJ shelf was moved to the speaker's corner frequency by the analog relation. That was wrong at high frequencies: 2.6 dB off for HS −12 dB at 10 kHz. | The shift is now made in the prewarped domain at the page's 48 kHz. | `node tools/test_rew_parse.js`: LS and HS at ±3/6/12 dB from 40 Hz to 10 kHz, and PK at ±3/12 dB with Q 0.5–10, all within 0.01 dB of RBJ. |
| 3 | `main/CMakeLists.txt` | The comment said "protocol 14". | It says 15 now. | — |
| 4 | `web/speakers.html`, `web/rew.js` | Without `CONFIG_AIRPLAY_SURROUND_PLAYER_STREAMS` there is no `/rew.js`, and the "Set up with REW" button did nothing. | The button (and the sentence around it) is hidden until rew.js shows it. /roomeq only loads the script and needs nothing from it. | — |
| 5 | `web/rew.js` `parseRew()` | A decimal comma (`180,5 Hz`) was refused. | A comma between two digits becomes a point. Other commas stay. | Node test: comma, point, and a note line with a comma. |
| 6 | `main/web_gateway.c`, `speaker/main/web_speaker.c` | 9 JSON numbers were cast straight to `uint16_t`/`int16_t`/`uint8_t`/`int32_t`. Out of range, that is undefined in C. | `web_json_int()` in `web_helpers.c` checks for a finite value in range. Otherwise the request gets the existing range error. | Builds (S3, P4, speaker). |
| 7 | `docs/calibration.md`, `web/rew.js` | They said REW's output could be L, R or L+R. L+R plays 6 dB louder and clips from about −6 dBFS. | Both now say L or R. | — |

## Found while working

| # | Where | What was wrong | Fix | Tested |
|---|---|---|---|---|
| 8 | `main/gw_player.c` `gw_player_seek()` | The page's position (a JSON double) was cast to `long long` after only a `< 0` check. NaN or a huge value is undefined behaviour. | A non-finite position is refused, and anything above 24 h is clamped there. `do_seek()` already clamps a position past the end of the track. | Builds (S3, P4). |
| 9 | 6 comments (`gw_rew.h`, `rew.js`, `roomeq.html`, `surround_proto.h`, `sound_engine.h`, `audio_eq.h`) | They pointed at `ROADMAP_REW_AND_ALIGNMENT.md` and `docs/rew-alignment-design.md`, which are not in the repository. | They point at the matching sections of `docs/calibration.md`. | — |

## Checked, no change

- **Sweep end:** the fade-out of the Room EQ sweep reaches above about
  19 kHz. The page deconvolves with the sweep exactly as it was played, so
  only the SNR there is a little lower.
- **REW mode:**
  - the lock order is gw_rew, then gw_speakers;
  - the send from the timer callback does not block;
  - the mode is sent again on HELLO;
  - it is reset when the link is lost;
  - STATUS every 2 s against the page's poll every 1.5 s is fine.
- **gw_calib.c:**
  - the recording length is the play time + 1 s, rounded up. The start
    refuses more than `GW_CAL_RECORD_MAX_S`;
  - `s_sweep_readers` keeps the recording while /api/calibration/sweep
    sends it, and `gw_calib_release_recording()` (player, radio) waits for
    it;
  - the levels run releases its own recording.
- **Protocol:**
  - every payload has a `static_assert` against `SURROUND_MAX_PAYLOAD`;
  - both sides take a message only at its exact length, so an older peer's
    message is ignored, not misread.
- **Speaker:**
  - CAL_PLAY clamps the burst count, and `roomcal_ways()` caps the repeats
    per driver;
  - a Room EQ signal's gain is capped at 1.0;
  - the REW overlay switches through the same fade out / silence / fade in
    as a layout change, and a stop installs it at once;
  - REW mode counts clipped samples and does not limit them;
  - the EQ config check rejects inf and out-of-range floats.
- **Pages:**
  - every speaker name put into innerHTML goes through `esc()` / `e()`;
  - the fetch errors end in a message or a retry, not a stuck "busy".

## Left for Kristian

- **REW mode level:** it sums L+R without halving, by design. Halving it
  would make L+R safe but L or R alone 6 dB quieter than music. The docs
  now say to use L or R.
- **Encrypting the stream key:** SET_ENCRYPTION still goes unencrypted on
  the port 7100 control link. This was put off on 2026-11-26.

## Hardware test (v4.5.114)

1. A REW session: select a speaker, EQ off/on, invert tweeter, import a
   Generic file with PK + HS, Apply, stop.
2. Room EQ: a sweep, and Check levels at the default level (−18).
3. Raise a speaker's limit to −3, then Check levels at −12, −6 and −3. The
   microphone RMS should rise by about 6 and 9 dB from −12, with no
   "clipped" and "(estimated from −12.0 dBFS RMS)" shown. A sweep at the
   same level should give a microphone peak within 1–2 dB.
