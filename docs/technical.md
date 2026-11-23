# Technical details

[← Back to the README](../README.md)

## Audio formats

The Gateway advertises these formats and the Apple TV chooses one per
stream; the speakers decode whatever the Gateway passes on:

| Transport | Codec | Rate | Channels | Frames / packet |
|---|---|---|---|---|
| Realtime UDP (type 96) | ALAC 16-bit | 44.1 kHz | 2 | 352 |
| Buffered TCP (type 103) | ALAC 16-bit | 44.1 kHz | 2 | 352 |
| Buffered TCP (type 103) | ALAC 24-bit (lossless) | 48 kHz | 2 | 352 |
| Buffered TCP (type 103) | AAC-LC | 44.1 / 48 kHz | 2 | 1024 |
| Buffered TCP (type 103) | AAC-LC | 48 kHz | **6 (5.1) / 8 (7.1)** | 1024 |

- **The Apple TV decides the format** from the formats the Gateway announces
  and, for each track, from what the content offers. An "Atmos" label in
  Apple Music does not guarantee a 7.1 stream; some tracks arrive as stereo.
  YouTube sends stereo only.
- **Format changes mid-stream.** Every audio packet carries its format in the
  RTP SSRC. A speaker switches codec, channels, bit depth or rate per packet,
  without a new SETUP. A codec or channel change at the same rate keeps the
  queued audio; a rate change (44.1 ↔ 48 kHz) restarts I2S at the right
  moment with a short gap, and the timeline stays continuous. AirPlay labels
  AAC "F24"; that is the stream profile, not the source's depth.

## Channels

5.1 and 7.1 arrive as AAC-LC with channel configuration 6 or 7. A speaker
reads the channel labels of every stream and finds its role **by name**, so
one setting works for 5.1 and 7.1 and survives format changes.

| Stream | Channel order (AAC elements) |
|---|---|
| 5.1 (config 6) | C · L R · Ls Rs · LFE |
| 7.1 (config 7) | C · L R · Ls Rs (side) · Lrs Rrs (rear) · LFE |

With 5.1 the Rear positions play Side L / R, so a 7.1 set of speakers also
works with 5.1 content.

> [!NOTE]
> **7.1 naming.** The names above follow Apple's 7.1 order (first pair
> front, second pair side, back pair rear). The ISO reading of configuration 7
> names the first pairs differently. A listening test with a 7.1 channel
> test signal is in progress; if Front and Side turn out swapped, a setting
> will be added and this README updated.

## Stereo sources

With a stereo stream every speaker decodes both channels and takes what its
role needs: **Front L / R** play left / right, **Subwoofer** and **Mono mix**
play (L + R) / 2, **Centre, Side and Rear** stay silent. 5.1 and 7.1 streams
are never touched by this.

## Decoding and signal path (on each speaker)

1. **AAC** is decoded with **Fraunhofer FDK AAC**, patched to synthesise
   only the selected channel: the bits of all channels are read, but only the
   element that holds the selected speaker is inverse quantised, filtered and
   transformed. Output is signed 32-bit, straight to float, without a 16-bit
   step. **ALAC** uses Espressif's decoder; 24-bit samples also go straight to
   float.
2. **DSP** in 32-bit float: stereo routing, EQ / crossover, loudness,
   volume (the Apple TV's volume, then the speaker's level trim).
3. **I2S** with 32-bit samples at the stream's rate: 44.1 or 48 kHz for
   stereo, 48 kHz for surround. The selected speaker goes to **both slots**
   (L = R), or in bi-amp mode its woofer and tweeter ways to their outputs.

## Synchronisation

The Apple TV schedules audio on its **PTP clock**. Audio packets carry RTP
sample positions, and an **anchor** associates an RTP position with an Apple
PTP time. The Gateway supplies that clock to every speaker; each speaker
uses its local representation to decide when a particular sample must play.

**Two-way measurement on the Gateway.** `apple_clock` owns UDP **319/320**
and runs the transport and clock core in one task. RTSP SETUP selects the
sender's IP address, and the sender's Announce names the grandmaster. Only
the grandmaster gives the time: the clock is measured at the device that
announces itself as that grandmaster (its port identity is the grandmaster
identity, seen within 5 s), and there is no time until one does. When the
sender is its own master, that is the sender. A sender that only relays a
HomePod master would add its
own error: an iPhone two steps from the master gave a line 5 ms off for 12 s
and 7 steps in ten minutes. Before SETUP the first Announce can prepare the
clock. Only the grandmaster the sender names is used; other HomePods cannot
replace it. The line is never published before an Announce has identified
the grandmaster.

Each complete exchange provides four timestamps:

| Timestamp | Event | Clock |
|---|---|---|
| `t1` | Apple sent Sync | Apple PTP |
| `t2` | Gateway received Sync | Gateway `esp_timer` |
| `t3` | Gateway sends Delay_Req | Gateway `esp_timer` |
| `t4` | Apple received Delay_Req | Apple PTP |

For the Apple timestamp encoding observed with these senders, corrections
are **added**: `t1 = preciseOrigin + Sync correction + Follow_Up correction`,
and `t4 = receiveTimestamp + Delay_Resp correction`. The receive and send
marks on the Gateway are software timestamps, not radio hardware timestamps.
The module matches peer, source/port identity, domain and sequence; only a
response addressed to its own requesting identity completes the exchange.

```
a = t1 - t2                         Apple - local - downlink delay
b = t4 - t3                         Apple - local + uplink delay
RTT = b - a                         downlink + uplink delay
offset = (a + b) / 2                 estimated Apple - local
```

Delay_Req follows a fresh Sync / Follow_Up pair when allowed by the sender's
requested interval, with a minimum interval of **125 ms** (at most 8/s).
There is one request in flight and a **1 s** timeout. Other HomePods are not
polled as extra peers.

**One start rule, gradual rate learning.** The same rules hold at a cold
boot, a stream after a pause, a new master or a step. The line is ready
whenever the fit holds a second whose best exchange has **RTT <= 10 ms**:
about one second after the first answer, from the best of the 5-8
exchanges of that second, on the remembered clock rate or zero rate after a
cold boot. The same published line then learns the rate and corrects phase
smoothly. Not lower than 10 ms: a stream's first ~30 s can keep the best
of each second near 4.6 ms, and in a host simulation of a cold boot
straight into that a 4 ms limit left no time, so no sound, for 30-38 s.

The rate memory holds four grandmaster identities in RAM. TEARDOWN leaves
the selected clock running between tracks. A ready line can therefore
already exist at the next SETUP. A second with RTT X can be up to X/2 off
from path asymmetry; in the simulation the start was 0.1-0.6 ms off (once
2.2 ms), the same for every speaker, and taken back by the calm correction.
Fast readiness is not a promise of immediate sub-0.1 ms accuracy or zero
time to audible output.

**Filtering and a continuous line.** Each roughly one-second bucket keeps
its exchange with the lowest RTT. Every second enters a weighted
least-squares line over up to **10 minutes**: its weight falls with how far
its RTT is above the **shortest RTT among the seconds in the line** (a
second at it counts fully, one several ms above it hardly at all), and a
second more than 40 ms above it is not used. While the Gateway relays audio
most seconds are a few ms above the minimum; a hard limit of minimum + 1 ms
took 0-4 of 10 seconds there. The minimum is taken from the line, not from
the last 60 s: on the board a pause before a stream let the good seconds
age out of 60 s, the stream's first 4.6 ms seconds counted fully and the
line swung by 1.8 ms. Negative RTT and RTT over 1 s are invalid. The rate is initially pulled towards the remembered
prior. Seconds are closed and fitted after each received batch, never
between receiving one packet and time-stamping the next.

The published line follows the estimated rate and removes phase error with
a **20 s time constant**, limiting the phase correction to **50 ppm** and
rate changes to **20 ppm/s**. Ordinary updates continue the previous line
at the update instant and change its future rate, so they do not step time.
If the estimate and the published line ever differ by more than **10 ms**,
the line starts again from the estimate as a new epoch.
A second more than 2 ms (plus half its RTT excess) from the estimate is
ignored. Five successive such seconds close to the minimum RTT, on the same
side and spanning at least 4 s, confirm a clock step and start a new epoch. A different
grandmaster also starts a new acquisition; a relay change carrying the same
grandmaster can preserve the line.

After **30 s without a valid two-way exchange**, the Gateway
withdraws time. Reacquisition retains the learned rate and remeasures phase
with a fresh RTT floor. There is **no one-way fallback**.

**Distributing the same line over TSF.**

1. Every board reads its own `esp_timer` together with the access point's
   **TSF**, then fits their relation over the last **64 readings** (about
   8 s at 125 ms intervals). This reduces reading jitter.
2. Every **125 ms** the Gateway sends **TIME**: Apple time P at TSF T, the
   rate relative to TSF, source/grandmaster identities, epoch and readiness.
   `sample_count` counts quality-checked two-way observations; the fitted
   seconds are counted separately. Realtime's four-observation qualification
   therefore does not wait for four sparse accepted seconds.
3. Each speaker maps T to its own local time, then converts the rate to its
   own clock. It uses the timestamped instant, rather than packet arrival,
   so TIME transport delay does not become a clock offset. All boards must
   use the **same BSSID**: another mesh node has a different TSF.
4. The audio engine maps the RTP anchor through this local clock, compensates
   for **output latency** and the speaker's configured **delay**, and plays
   the scheduled samples. Its existing I2S servo uses the line rate directly
   and steers the remaining phase back over about **10 s**, calculating and
   applying live clock corrections at most once per second. It logs
   `SERVO LINE: line ... ppm, sync ... ms -> I2S ... ppm`.

The Gateway has one clock implementation, the `apple_clock` component of
[esp-airplay2-core](https://github.com/Kristian8606/esp-airplay2-core). A speaker's
`common/network/ptp_clock.c` is only the external TIME state and conversion
interface for its audio engine; it has no PTP sockets or passive estimator.
The optional oscilloscope pulse on the Gateway reads the same published
Apple line. The algorithm is described in that component's `src/apple_clock_core.h`.

**Absolute accuracy.** Two-way timing cannot distinguish a permanent
uplink/downlink asymmetry from clock offset. The remaining error is half
the difference between those delays. `AIRPLAY_APPLE_CLOCK_ASYM_US` adds a
calibration offset (default 0; positive makes playback earlier). Measure
against a HomePod with a microphone before calibrating it. `ready=1`,
`locked=1` and a small internal speaker sync error do not establish the
acoustic accuracy to Apple by themselves.

**Wi-Fi dropouts.** During a stream a speaker carries the last line forward
for up to 30 s, and both sides wait 20 s (instead of 6 s) before giving up a
silent link. A buffered speaker can continue from its queued audio during a
short dropout. If the Gateway explicitly sends invalid TIME, the speaker
withdraws that external clock. After a lost stream a buffered late join
still waits for the next pause, seek or track FLUSH; this admission wait is
separate from Apple clock acquisition.

## Relaying the stream

| Stream | How the Gateway passes it on |
|---|---|
| **Buffered** (type 103: music, films) | one TCP connection per speaker, a copy of the sender's. The Gateway reads the sender whenever its 4 MiB PSRAM buffer (8 MiB on the ESP32-P4, which also tells the sender it can take 8 MiB instead of 6) has room and sends each speaker from it; when the buffer is full (a speaker keeps old audio longest), the sender waits. A speaker that takes no audio for a while (a Wi-Fi gap) is waited for and goes on without a gap; it is dropped from the stream only once its audio fills the buffer (less than 512 KiB free), and connected again 30 s later. Speakers keep up to ~90 s of compressed audio. Sending is paced at twice real time after a 2 s start: at full speed to every speaker at once it kept the Wi-Fi so busy that the sender's clock packets arrived late. At an immediate FLUSH (an iPhone's track change) each speaker's stream skips to the new track at its next frame boundary; read and sent at paced speed only to be dropped, the old track took 2-4 s per change. |
| **Realtime** (type 96: screen mirroring, system audio) | every UDP audio and sync packet is copied to each speaker's ports. A speaker that misses a packet asks the Gateway; several requests for the same packet reach the Apple TV once. |

A speaker that connects in the middle of a session gets the session's
settings replayed (format, key, volume, timing). With a buffered stream it
waits for the next flush (skip or seek), because it can only start decoding
at a point every speaker starts from; with a realtime stream it joins at
once.

## Performance and memory

| | Gateway | Speaker (one channel of 7.1) |
|---|---|---|
| CPU | relays AirPlay without decoding (not yet measured); decodes and encodes only for the web radio and the USB player | ~31–34 % of one core (measured on a rear speaker) |
| Audio buffer | 4 MiB relay buffer in PSRAM (8 MiB on the ESP32-P4) | 6 MB compressed stream buffer in PSRAM |
| Decode task stack | — | 20 KiB in PSRAM (deepest use ~11 KiB) |
| Free PSRAM during 7.1 | — | ~400 KiB |
| Firmware image | ~2.1 MB on the ESP32-S3, ~3.4 MB on the ESP32-P4 (web radio and USB player included) | ~1.86 MB (no AirPlay receiver code) |

| Board | Core 0 | Core 1 |
|---|---|---|
| Gateway | RTSP, PTP, web radio (decode, ALAC encode) | Wi-Fi, LwIP, buffered and realtime relays, speaker links |
| Speaker | AAC decoding | Wi-Fi, LwIP, stream reader, I2S playout, status / LED / log tasks |

The FDK changes (single-channel synthesis, 32-bit output, hardware bit
counting on the ESP32-S3, element skipping, smaller buffers, lazy limiter)
are listed in
[`speaker/components/fdk_aac/MODIFICATIONS.md`](../speaker/components/fdk_aac/MODIFICATIONS.md).

## Under the hood

| Part | Where | What it does |
|---|---|---|
| AirPlay 2 core | `ap2_core` ([esp-airplay2-core](https://github.com/Kristian8606/esp-airplay2-core)) | pairing, plists, audio decryption, the FairPlay setup replies, the RTSP server (`rtsp` in the log) and the mDNS announcement (`mdns_airplay`), the last two Gateway only, under Apache-2.0 (the FairPlay tables excepted); both firmwares |
| Web server | `components/web_server` | the shared pages and APIs, captive portal (`web_server` in the log), under Apache-2.0; both firmwares |
| Settings | `components/settings` | device name, Wi-Fi networks, volume and the speaker settings in NVS, under Apache-2.0; both firmwares |
| Status LED | `components/led` | the AirPlay state and a level meter on the WS2812 LED (`led` in the log), under Apache-2.0; both firmwares |
| Helpers | `components/helpers` | session events, bound sockets, captive DNS (`dns_server` in the log), under Apache-2.0; both firmwares |
| Wi-Fi | `components/wifi` | station on a saved network, setup access point, host name (`wifi` in the log), under Apache-2.0; both firmwares |
| Live log, firmware update | `components/log_stream`, `components/ota` | the log on `/logs`; update from a `.bin` upload (`ota` in the log), under Apache-2.0; both firmwares |
| Web pages | `main/web_gateway.c`, `speaker/main/web_speaker.c`, `web/` | each firmware's own pages and APIs on top of the shared web server; `web/theme.css` holds the Warm and Violet colours of every page |
| Gateway proxy | `main/gw_proxy.c` | implements the audio engine API on the Gateway and forwards each call (`FWD …` in the log) |
| Speaker registry | `main/gw_speakers.c` | control link on port 7100 (`_apsurround._tcp`), late joins, settings, status, TIME messages, which speaker updates next |
| Speaker firmware | `main/gw_fw.c`, `speaker/main/spk_update.c` | the image in the Gateway's `storage` partition (`gw_fw`); download, check and install on the speaker (`spk_update`) |
| Buffered relay | `main/gw_relay.c` | reads the TCP audio stream into a buffer and copies it to every speaker; drops the old track at a FLUSH (`RELAY …`) |
| Realtime relay | `main/gw_rt_relay.c` | copies UDP audio and sync packets, passes retransmit requests on (`RT RELAY …`) |
| Apple clock | `apple_clock` ([esp-airplay2-core](https://github.com/Kristian8606/esp-airplay2-core)) | Gateway-only two-way PTP transport, RTT filtering, rate estimation and continuous published line (`apple_clock …`) |
| Time distribution | `common/surround/tsf_fit.c`, `main/gw_speakers.c`, `speaker/main/spk_time.c` | TSF fits on both sides; maps the same Apple line into TIME and each speaker's local clock |
| Speaker clock state | `common/network/ptp_clock.c` | external TIME state, rate carry and conversions for the audio engine; no PTP receiver |
| Speaker link | `speaker/main/spk_link.c` | finds the Gateway, runs every forwarded call locally (`CMD …`) |
| Protocol | `common/surround/surround_proto.h` | message types and payloads, versioned |
| Audio engine | `speaker/main/audio` | FIFO, FDK AAC / ALAC decode, PCM store, I2S playout and servo |
| Room calibration and Room EQ | `main/gw_calib.c`, `main/gw_mic.c`, `common/surround/cal_chirp.c`, `web/roomeq.html` | microphone capture, chirp search and level / delay suggestions; the sweeps of Room EQ, whose response and filters the browser computes (`gw_calib` in the log); the speaker side plays the chirps and sweeps (`ROOM CAL …`) |
| Web radio | `main/gw_radio.c`, `web/radio.js` | reads a station, decodes MP3 / AAC, encodes ALAC and sends it to the buffered relay like an Apple sender (`gw_radio` in the log); the stations panel and its Now playing card |
| USB player | `main/gw_player*.c`, `main/gw_usb.c`, `web/player.js` | reads a file (USB drive or HTTP), decodes it on core 0, encodes one ALAC lane per speaker on core 1 and sends them to the buffered relay in lane mode (`gw_player`, `gw_usb` in the log); the Library and its Now playing card. The speakers take the lanes with **Player streams** (`common/surround/surround_player_formats.h`) |
| Speaker EQ | `speaker/main/audio/audio_eq.c`, `web/eq.html` | 24 biquads per output, mono or bi-amp with a Linkwitz-Riley crossover, delay and polarity per output |
| AirPlay artwork (ESP32-P4) | `main/gw_airplay_artwork.c`, `main/ap2_artwork.patch` | keeps the picture the sender sends and serves it to the Now playing card; the patch is applied at build time to a copy of `ap2_core`'s RTSP reader, like `main/ap2_formats.patch` |
| Local volume | `main/gw_local_volume.c` | one saved volume for the web radio and the USB player; the hardware buttons change it while one of them plays |
| USB covers (ESP32-P4) | `main/gw_cover_thumbnail.c`, `main/gw_cover_cache.c` | thumbnails with the P4's JPEG and PPA hardware, kept on the stick |
| ESP32-C6 (ESP32-P4) | `coprocessor/c6/`, `main/gw_c6_fw.c`, `main/gw_c6_time.c`, `common/gw_c6/` | Wi-Fi through ESP-Hosted; the C6 firmware that sends the TSF and the PTP receive times, installed from the Gateway, and the P4 side that timestamps its pulse |
