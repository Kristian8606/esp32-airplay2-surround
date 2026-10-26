![ESP32 AirPlay 2 Surround — home cinema](images/airplay-surround-dark.png)

# esp32-airplay2-surround

**Wireless 5.1 / 7.1 surround from Apple TV, built from ESP32-S3 boards.**
Each board is a full AirPlay 2 receiver that decodes the surround stream and
plays **one speaker position** of it: Front Left, Centre, Subwoofer, Rear
Right and so on. Put one board in every speaker, give each its position, and
together they form a surround system that Apple TV keeps in sync over PTP,
the same way it drives its own speakers. With ordinary stereo sources every
board simply plays stereo.

> **Status: experimental.** Tested with an Apple TV: Apple Music in Dolby
> Atmos arrives as 7.1, HBO as 5.1, YouTube as stereo. See
> [Limitations](#limitations).

## The idea

Apple TV can send surround sound over AirPlay 2, but only to receivers that
say they accept it. When it does, every receiver in the group gets the
**whole** multichannel stream, not only its own part, and all receivers play
each sample at the same moment because they share one PTP clock.

This project uses exactly that. One ESP32-S3 board cannot drive eight
speakers, and it does not need to: it only has to receive the full stream and
play a single channel of it. So:

- **Every board announces 5.1 and 7.1 AAC.** Apple TV then sends the
  multichannel stream to all of them.
- **Every board decodes only its own channel.** The bits of all channels are
  read, but only the element holding the selected channel is inverse
  quantised and synthesised, which keeps the CPU load at about a third of one
  core instead of more than 100 %.
- **Every board plays that channel on both outputs** of its DAC or amplifier
  (L = R), so either output can drive the speaker, or both can feed an active
  subwoofer or a pair of drivers in one cabinet.
- **The boards never talk to each other.** Timing comes from Apple TV's PTP
  clock, the same one AirPlay 2 uses for every multi-room group, so adding a
  speaker is just adding another board to the AirPlay group.

The speaker position is chosen in the board's web page and can be changed
while music plays. Boards are cheap and independent, so you can start with a
centre speaker and a subwoofer and add surrounds later.

## How it works

1. **Formats.** In `GET /info` the receiver advertises, in
   `supportedFormats`, stereo **ALAC 44.1 kHz/16-bit** and **48 kHz/24-bit**,
   stereo **AAC-LC 44.1 / 48 kHz**, and in multichannel builds **AAC-LC
   48 kHz 5.1 and 7.1**. The sender picks one per stream.
2. **Decoding.** All AAC is decoded with **Fraunhofer FDK AAC** to signed
   32-bit PCM and converted straight to float. For 5.1/7.1 a small FDK patch
   synthesises only the selected speaker and outputs just that channel.
   ALAC is decoded with Espressif's ALAC decoder, 24-bit samples also going
   straight to float.
3. **Output.** The selected speaker goes to **both I2S slots** (L = R). EQ,
   loudness and the output filters work as usual, so a board can drive a
   subwoofer through a low-pass filter.
4. **Rate.** I2S follows the stream: 44.1 or 48 kHz for stereo, 48 kHz for
   surround, 44.1 kHz for realtime ALAC.
5. **Sync.** Playback is timed from the sender's PTP clock with a live-tuned
   I2S clock, so each board plays every sample at its scheduled time.

The rest is the base receiver: AirPlay 2 pairing, buffered (type 103) and
realtime (type 96) streams, and a web UI with EQ, loudness, latency and OTA.

### Input formats

| Transport | Codec | Rate | Input channels | Frames/packet |
|---|---|---|---|---|
| Realtime UDP (96) | ALAC 16-bit | 44.1 kHz | 2 | 352 |
| Buffered TCP (103) | ALAC 16-bit | 44.1 kHz | 2 | 352 |
| Buffered TCP (103) | ALAC 24-bit | 48 kHz | 2 | 352 |
| Buffered TCP (103) | AAC-LC | 44.1 / 48 kHz | 2 | 1024 |
| Buffered TCP (103), multichannel build | AAC-LC | 48 kHz | 6 / 8 | 1024 |

Stereo builds advertise and play all four buffered stereo formats; surround is
advertised only when multichannel support is enabled.

**Precision.** ALAC 24-bit and AAC's 32-bit decoder output go into the float
pipeline without a 16-bit step; I2S uses 32-bit samples. The FDK PCM limiter
is off (the 32-bit output requires it); DRC is unchanged. AirPlay labels AAC
as "F24": that describes the stream profile, not a lossless source depth.
FDK uses noise-substitution concealment, which adds no extra frame of delay,
so every decoded block stays aligned to its packet's RTP time.

**Format changes.** SETUP's `audioFormat` fills in what the sender leaves out
(rate, depth, block size, channels); conflicting fields are rejected. After
that the format of each buffered packet, given by its SSRC, decides: the
sender can switch codec, channels, depth or rate mid-stream without a new
SETUP, for example between AAC and lossless ALAC.

- A change of codec or channels at the same rate keeps the queued audio.
- A change of rate waits until the new audio is due, then retires the
  old-rate audio and restarts I2S at the new rate. The PTP/RTP time map is
  rebased at that packet so the timeline stays continuous.
- A packet that changes the format is authenticated before anything in the
  pipeline changes.
- This is not a gapless resampler: a rate change has a short gap. Only AAC
  blocks are muted at a start or a timestamp jump, not ALAC.

## Choosing the speaker

**In the browser** (recommended): open `http://<board-ip>/eq`, card
**"Surround speaker (5.1 / 7.1)"**. The choice applies from the next decoded
frame, without a restart, and is saved. The card also shows what is playing
right now.

**At build time** (default before anything is chosen in the browser):
`idf.py menuconfig` → **Multichannel Experiment** → *Default speaker this
receiver plays*.

| Speaker | Where | Notes |
|---|---|---|
| Front Left / Right (L / R) | ~30° left/right of the screen | main pair |
| Centre (C) | under/over the screen | most of the dialogue |
| Subwoofer (LFE) | anywhere low | only the low-frequency effects channel, not the bass of the other speakers |
| Side Left / Right (Ls / Rs) | beside the listener, ~90–110° | the surround pair of 5.1 |
| Rear Left / Right (Lrs / Rrs) | behind the listener, ~135–150° | 7.1 only; with 5.1 they play Side L/R |
| Mono mix | — | all channels except LFE; full decode, most CPU |

The speaker is found by **name**, from the channel labels of each stream, so
one setting works for 5.1 and 7.1.

**7.1 naming caveat.** Apple TV sends 7.1 as AAC channel configuration 7
(centre, three pairs, LFE). The names above follow Apple's 7.1 order (first
pair L/R, second pair sides, backs rear). The ISO reading of configuration 7
calls the first pair "front centre" and the second L/R instead. If a 7.1
speaker test plays Front where you expect Side, that is why; please open an
issue.

The EQ page's old *Source mode* (stereo / mono / left / right) does not matter
for a surround stream: both inputs carry the same speaker, so every mode gives
the same signal.

## Hardware

- **ESP32-S3** with **8 MB flash** and **8 MB octal PSRAM** (the buffered
  stream alone keeps a 6 MB compressed buffer in PSRAM).
- An I2S DAC / amplifier. Defaults: BCLK GPIO 5, LRCK/WS GPIO 6, DATA GPIO 4,
  MCLK unused; change them in `menuconfig` → **I2S Playout**.
- Optional: WS2812 VU LED (GPIO 38 by default), amplifier power-control GPIO,
  playback buttons.

## Build and flash

Built with **ESP-IDF v6.0.2**.

```bash
idf.py set-target esp32s3
idf.py menuconfig    # optional: I2S pins, default speaker, logs
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

First start: join the Wi-Fi access point **ESP32-AirPlay-Setup** and open
`http://192.168.4.1/` to enter your network. Afterwards the board shows up as
an AirPlay 2 speaker; add all of them to the Apple TV's audio output.

To build the plain stereo receiver from this tree, turn off
**Multichannel Experiment → Receive 5.1/7.1 AAC and play one channel**, and
remove the two "Multichannel experiment" lines at the end of
`sdkconfig.defaults` (they move Wi-Fi and LwIP to Core 1).

### Surround Gateway build (work in progress)

The same tree builds a second firmware: the **Gateway**, the single AirPlay
receiver the Apple TV talks to. It keeps mDNS, pairing, RTSP and PTP, but has
no decoder and no I2S output: `main/surround/gw_proxy.c` implements the
`audio_receiver.h` API that RTSP calls, and `gw_relay.c` accepts the buffered
audio stream. For now both only log (`FWD …` and `RELAY …` lines); forwarding
to the speakers comes next. The role is chosen in **Surround Build Role**;
`sdkconfig.defaults.gateway` selects it. Keep each role in its own build
directory:

```bash
idf.py -B build_gateway -D SDKCONFIG=build_gateway/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.gateway" build
```

```bash
idf.py -B build_gateway -p /dev/cu.usbmodem* flash monitor
```

The speaker build is the default and unchanged (`idf.py build` above).

### Where the work runs

| Core 0 | Core 1 |
|---|---|
| FDK AAC decoding, RTSP, PTP | Wi-Fi driver, LwIP, AAC TCP reader, I2S playout, status / LED / log tasks |

The decode task has a 20 KiB stack in PSRAM (internal RAM only as a fallback;
the log says which); FDK's deepest path uses ~11 KiB. An internal-RAM stack
was measured and is no faster. The AAC signed32 scratch buffer (64 KiB) lives in PSRAM and can
fall back to the regular heap.

## Limitations

- **Experimental**, tested with one Apple TV. An iPhone sends stereo to this
  receiver; multichannel was only seen from the Apple TV.
- **Rear channels are often nearly silent in music**; they come alive with
  films and Atmos mixes.
- The **7.1 naming** (Apple vs ISO order) has not been confirmed with a channel
  test signal yet (see above).
- **CPU**: decoding one speaker of 7.1 takes ~31–34 % of Core 0 (measured on a
  back speaker; centre and LFE should be lower). Reading the bits of every
  channel cannot be skipped; the other channels are not inverse quantised or
  synthesised. Mono mix decodes everything and may not keep up.
- **Realtime ALAC** plays at 44.1 kHz through the rate switch, but it has not
  been tested much in multichannel builds yet.
- Each switch between 44.1 and 48 kHz has a short silence while I2S is
  reconfigured.
- Some video apps take 20–30 s between the start of the AirPlay session and
  the first audio (seen with HBO); that is the sender, not a fault here.
- Wi-Fi and LwIP pinned to Core 1 (`sdkconfig.defaults`) apply to the whole
  branch, multichannel on or off.

## Logs

Useful lines in the serial or web log (`http://<board-ip>/logs`):

| Line | Meaning |
|---|---|
| `W ... NOW PLAYING \| AAC-LC \| 48000 Hz \| 24-bit (F24) \| 7.1 (8ch) \| Front Right (R)` | actual surround selection, logged when the decoder applies it |
| `W ... NOW PLAYING \| ALAC \| 48000 Hz \| 24-bit \| stereo L/R` | stereo format after successful PCM publication |
| `SETUP audioFormat=0x10000000000 -> AAC-LC 48000 Hz / F24 / 7.1` | the initial format the sender chose; F24 is AAC profile metadata |
| `AUDIO FORMAT ... -> ALAC 48000 Hz / 24-bit / stereo; SSRC ...` | actual input change, old/new SSRC and sequence |
| `AUDIO PIPELINE AAC/48000 Hz/F24/8 ch/1024 frames -> ALAC/48000 Hz/S24/2 ch/352 frames` | old/new codec, rate, AirPlay profile or ALAC integer depth, channels and block size |
| `I2S RATE 44100 -> 48000 Hz` | the physical I2S clock has changed successfully |
| `AUDIO RATE ANCHOR 44100 -> 48000 Hz, rebased at RTP=...` | timing continuity at the rate-change boundary |
| `MC: decoded channel N = back #0 -> Rear Left (Lrs)` | channel layout of the stream |
| `MC CPU … avg …% of one core` and `MC CPU stages …` | decode time per frame, split into parse / synthesis / output |
| `audio_status: … pcm=…ms` | decoded audio queued for I2S; should stay near 850 ms |

For RTSP-level debugging enable **AirPlay Logs and Diagnostics → Log every
RTSP request and response** (verbose; turn it off afterwards).

This single checkbox (`CONFIG_AIRPLAY_LOG_RTSP_REQUESTS`) controls all
`RTSP REQ` / `RTSP RSP` lines, including `/feedback`, `/command`, metadata and
progress requests. It is disabled by default. `NOW PLAYING` warnings remain
visible with both this option and **Runtime event logs** off; they are emitted
on stream start, input format changes or applied surround speaker changes,
not for every packet. The periodic status also includes codec and rate, with
AAC's AirPlay profile as `24-bit (F24)`, or ALAC's actual 16/24-bit depth.
Internal decoder/DSP/I2S details are omitted from playback and status lines.
AAC's profile does not establish the source recording's bit depth or make
the lossy codec lossless.
Each playback warning is framed by `================================================`
on separate warning-level log lines, so the complete format and speaker remain
easy to find in both serial and web logs.

For hardware regression testing, try realtime screen/system audio, buffered
44.1/48 kHz stereo AAC, lossless 48 kHz ALAC, and 5.1/7.1 speaker selection.
Then change tracks/apps and AAC/lossless output without disconnecting AirPlay,
including 44.1 → 48 → 44.1 kHz, pause/resume and seek. Check both `AUDIO FORMAT`
and `I2S RATE` logs; a new SETUP is not required for an SSRC format change.

## Licenses

- **Fraunhofer FDK AAC** (`components/fdk_aac/fdk-aac`) is under the
  Fraunhofer FDK license, [`NOTICE`](components/fdk_aac/fdk-aac/NOTICE). The
  copy here is a **Third-Party Modified Version of the Fraunhofer FDK AAC
  Codec Library for Android**; the changes and their date are listed in
  [`components/fdk_aac/MODIFICATIONS.md`](components/fdk_aac/MODIFICATIONS.md).
  The license allows redistribution in source and binary form without
  copyright fees, but **grants no patent rights**: AAC patent licenses are
  available through Via Licensing or the patent owners.
- The **rest of this repository** has no license file yet, so no license is
  granted for it beyond viewing it on GitHub.
- Other components (ESP-IDF, `esp_audio_codec`, libsodium, mDNS, cJSON,
  led_strip) come through the ESP-IDF component manager under their own
  licenses.

## Credits

- Based on [airplay-esp32](https://github.com/Kristian8606/airplay-esp32),
  the stereo AirPlay 2 receiver for the ESP32-S3.
- [fdk-aac](https://github.com/mstorsjo/fdk-aac) for the standalone build of
  the Fraunhofer FDK AAC codec.

AirPlay, Apple TV and HomePod are trademarks of Apple Inc. This project is not
affiliated with or endorsed by Apple.
