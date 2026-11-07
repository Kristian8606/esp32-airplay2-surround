<p align="center">
  <b>Enjoying this project?</b> It is built and tested in my free time. A small
  donation helps buy test hardware and keeps the development going. Thank you! ❤️
  <br><br>
  <a href="https://www.paypal.com/donate/?business=VGY3ZM78NAL78&no_recurring=0&currency_code=EUR"><img src="https://img.shields.io/badge/Donate-PayPal-ff5e5b?style=for-the-badge&logo=paypal&logoColor=white" alt="Donate with PayPal"></a>
</p>

<p align="center">
  <a href="images/airplay-surround-dark.png"><img src="images/airplay-surround-dark.png" alt="ESP32 AirPlay 2 Surround 7.1" width="100%"></a>
</p>

# ESP32 AirPlay 2 Surround Gateway

**Wireless 5.1 / 7.1 surround sound from an Apple TV, built from low-cost
ESP32-S3 boards: one Gateway, and one board per speaker.**

The Apple TV sees **one AirPlay 2 speaker: the Gateway**. The Gateway takes
the complete surround stream and passes it on, unchanged, to up to **eight
speaker boards** over your Wi-Fi. Every speaker decodes **only its own
channel** (Front Left, Centre, Subwoofer, Rear Right, …) and plays it through
any amplifier and speaker you like. All speakers share one clock taken from
the Wi-Fi access point itself, so they play each sample at the same moment.
No cables between the speakers, no AV receiver, one web page for the whole
system.

- 🎬 **5.1 and 7.1 AAC** (Apple Music Dolby Atmos, films and series from the
  Apple TV apps).
- 🎵 **Stereo** too: ALAC 44.1 kHz/16-bit and **lossless 48 kHz/24-bit**, AAC
  44.1 / 48 kHz, from any AirPlay 2 sender. A stereo song plays on the front
  pair and the subwoofer, as on an AV receiver.
- 📡 **One AirPlay target.** You pick the Gateway, not eight speakers. Pairing,
  RTSP and the Apple TV's clock are handled once, by the Gateway.
- ⏱️ **Microsecond clock alignment** between speakers through the access
  point's beacon timer (TSF). No extra hardware, no cables.
- 🎤 **Room calibration** with an optional INMP441 microphone on the
  Gateway: distance, delay and level of every speaker measured from your
  seat, applied with one click.
- 🎛️ **One page for all speakers**: role, level and delay of every speaker,
  and a live view of what each one plays. Each speaker keeps its own
  2 × 24-band parametric EQ / crossover, loudness and latency settings.
- 🔀 **Format changes on the fly**: the Apple TV can switch between stereo and
  surround from track to track; the speakers follow without reconnecting.
- ⚡ About **a third of one CPU core** per speaker for 7.1, thanks to a patched
  Fraunhofer FDK AAC decoder that synthesises just one channel.
- 🔗 **Speakers belong to the system**: a speaker is not an AirPlay
  receiver of its own and never shows up on an iPhone; it just follows the
  Gateway. (A board that is its own AirPlay receiver is the `standalone`
  branch.)

> [!NOTE]
> **The project is under active development.** A Gateway and two speakers
> play a 7.1 stream from an Apple TV, each its own channel, audibly in sync.
> Features, defaults and wiring may still change, and this README is updated
> with every change. See [Limitations](#limitations).

## Contents

- [Gallery](#gallery)
- [How it works](#how-it-works)
- [What you need](#what-you-need)
- [Assembly and wiring](#assembly-and-wiring)
- [Installation](#installation)
- [First start](#first-start)
- [Settings](#settings)
- [Technical details](#technical-details)
- [Measuring sync](#measuring-sync)
- [Limitations](#limitations)
- [Troubleshooting and logs](#troubleshooting-and-logs)
- [Licenses and credits](#licenses-and-credits)

## Gallery

Click an image to open it full size.

<table>
  <tr>
    <td align="center" width="50%">
      <a href="images/wiring-max98357a.png"><img src="images/wiring-max98357a.png" alt="ESP32-S3 + MAX98357A I2S amplifier + speaker" width="100%"></a>
      <br><sub><b>Simplest speaker:</b> ESP32-S3 + MAX98357A I2S amplifier + one speaker</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/wiring-pcm5102a-2way.jpg"><img src="images/wiring-pcm5102a-2way.jpg" alt="ESP32-S3 + PCM5102A DAC + 2-channel amplifier, woofer and tweeter" width="100%"></a>
      <br><sub><b>Active 2-way speaker:</b> ESP32-S3 + PCM5102A DAC + 2-channel amplifier, woofer + tweeter</sub>
    </td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <a href="images/airplay-surround-light.png"><img src="images/airplay-surround-light.png" alt="Surround setup around a TV" width="60%"></a>
      <br><sub>One board in every speaker, all fed wirelessly through the Gateway</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="images/speakers-page-room.png"><img src="images/speakers-page-room.png" alt="Gateway speakers page: room map" width="100%"></a>
      <br><sub><b>Gateway speakers page:</b> the room map shows which positions play</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/speakers-page-tiles.png"><img src="images/speakers-page-tiles.png" alt="Gateway speakers page: one tile per speaker" width="100%"></a>
      <br><sub>One tile per speaker: role, level, delay and live sync</sub>
    </td>
  </tr>
</table>

## How it works

The Apple TV sends surround sound over AirPlay 2 only to a receiver that says
it accepts it. The Gateway says so, receives the whole multichannel stream
and shares it with the speakers:

1. **The Gateway is the only AirPlay receiver.** It announces 5.1 and 7.1, so
   the Apple TV sends it the full multichannel stream. mDNS, HAP pairing,
   RTSP and the Apple TV's PTP clock all run there.
2. **It forwards, it does not decode.** Every instruction from the Apple TV
   (format, key, start, flush, pause, volume, timing) is passed to the
   speakers over a small control protocol, and every encrypted audio packet
   is copied byte for byte to each speaker. The Gateway reads the stream into
   a 4 MiB buffer and sends each speaker from it, so at a track change the
   old track is dropped from the buffer at once instead of being sent only
   to be thrown away.
3. **Every speaker decodes only its own channel.** One ESP32-S3 cannot decode
   all eight channels in real time, and it does not need to. A speaker is
   not an AirPlay receiver: it finds the Gateway by mDNS, decrypts the stream
   with the key the Gateway passed on and plays its channel
   on **both outputs** of its DAC or amplifier (L = R), so you can drive one
   speaker, two drivers (woofer + tweeter) or a subwoofer amplifier.
4. **One two-way Apple clock for all.** The Gateway measures both directions
   of the PTP path with Sync / Follow_Up and Delay_Req / Delay_Resp, then
   publishes one smooth clock line. Every board on the same access point
   shares its beacon timer (TSF). Eight times a second the Gateway tells the
   speakers "the Apple TV's time P is TSF T". Each speaker converts T to its
   own clock and plays every sample at the moment the Apple TV scheduled it.

You choose each speaker's role (Front Left, Centre, Subwoofer, …) on the
Gateway's page, even while music is playing. Start with two or three
speakers and add the rest later.

```
                       ┌─────────────────────────────┐
   Apple TV ──AirPlay──►  Gateway (ESP32-S3)          │
   (PTP clock)         │  pairing · RTSP · PTP        │
                       │  no decoder, no audio output │
                       └─────┬──────────────┬─────────┘
              control + time │              │ the same audio packets,
              (TCP, port 7100)              │ unchanged, to every speaker
         ┌──────────┬────────┴─┬────────────┴┬────────────┐
         ▼          ▼          ▼             ▼            ▼
       ESP32      ESP32      ESP32         ESP32        ESP32    (… up to 8)
      Front L    Front R    Centre       Subwoofer    Rear L/R
         │          │          │             │            │
        amp        amp        amp           amp          amp
         │          │          │             │            │
      speaker    speaker    speaker      subwoofer     speaker
```

### Sync, as measured

| What | Measured |
|---|---|
| TSF reading jitter per board | ~4.5 µs rms, averaged by a 64-point line fit |
| Clock difference speaker ↔ speaker | a few µs (from the fits; scope test pending) |
| Playout error of each speaker | within ±0.4 ms, corrected slowly by the I2S servo |
| Speaker ↔ speaker in practice | 0.1–0.3 ms, about 3–10 cm of sound travel |
| Transport delay of the time messages | 3–60 ms, does not matter by design |

The time messages carry a TSF timestamp rather than "now", so their travel
time over Wi-Fi is not added to the speaker's clock offset. TSF reading and
fit errors still matter. Speaker playout logs measure following the Gateway
line; they do not measure the acoustic offset to a HomePod.

In a captured **v4.5.33** cold start the Apple clock became ready **80 ms**
after the master was identified, before audio stream SETUP. The speaker
reported its first audio **172 ms after the anchor**, and **223 ms** on a
later start. These are observed starts, not guaranteed latency on every
network.

### What each speaker plays

| Source | Front L / R | Centre | Subwoofer | Side / Rear | Mono mix |
|---|---|---|---|---|---|
| **7.1** (Apple Music Dolby Atmos) | own channel | own channel | LFE channel | own channel | all except LFE |
| **5.1** (films, series) | own channel | own channel | LFE channel | Side L/R; Rear plays Side | all except LFE |
| **Stereo** (music, YouTube) | left / right | silent | mix of L and R | silent | mix of L and R |

With stereo the subwoofer gets the full-range mix; a low-pass on its EQ page
(or the subwoofer amplifier's own crossover) keeps the bass.

## What you need

### The Gateway

| Part | Notes |
|---|---|
| **One ESP32-S3 board**, the same type as the speakers (N8R8) | It has no audio output: no DAC, no amplifier, no wiring. A USB supply is all it needs. |
| **A place with a good signal** | It receives the stream from the Apple TV and sends it again to every speaker, so it carries the most Wi-Fi traffic. Near the access point is best. |
| *Optional:* **INMP441 I2S microphone** | For [room calibration](#room-calibration): VDD → 3V3, GND and L/R → GND, SCK → GPIO 5, WS → GPIO 6, SD → GPIO 4 (changeable in `menuconfig`). |

### Per speaker (one set for every position)

| Part | Notes |
|---|---|
| **ESP32-S3 board with 8 MB flash and 8 MB PSRAM** (N8R8), e.g. ESP32-S3-DevKitC-1 N8R8 | PSRAM is required: the stream buffer alone takes 6 MB. Boards without PSRAM (or with 2 MB) will not work. |
| **Audio output**, one of: | |
| &nbsp;&nbsp;• an **I2S amplifier** such as **MAX98357A** | Simplest: digital in, speaker out, about 3 W. Good for small satellites and surrounds. |
| &nbsp;&nbsp;• an **I2S DAC** such as **PCM5102A** + **any analogue amplifier** | Any size: mini (PAM8403, 2 × 3 W), medium or powerful (TPA3110 / TPA3116, TDA7498, a plate amplifier for a subwoofer…), **one- or two-channel**. |
| **Speaker(s)**, 4–8 Ω | One full-range driver, or a woofer + tweeter pair (see [Option B](#option-b--dac--any-amplifier)). |
| **Power supply** | 5 V USB (≥ 1 A) for the ESP32-S3; a separate supply for a larger amplifier (e.g. 12 V ≥ 2 A, as your amplifier requires). |
| **Enclosure** | 3D-printed, wooden or an existing speaker box. |
| Jumper wires or a small PCB | 5 wires for the I2S connection. |

### Number of boards

| System | Boards | Positions |
|---|---|---|
| 5.1 | 1 Gateway + 6 speakers | Front L, Front R, Centre, Subwoofer, Side L, Side R |
| 7.1 | 1 Gateway + 8 speakers | the 5.1 positions + Rear L, Rear R |
| Start small | 1 Gateway + 2–3 speakers | e.g. Front L + Front R (+ Centre) |

A channel without a speaker is simply not played. The **centre carries most
of the dialogue**, so build it early.

### Sender and network

- An **Apple TV** for 5.1 / 7.1 (surround is sent only by the Apple TV; an
  iPhone, iPad or Mac sends stereo, which the speakers play as well).
- A **2.4 GHz Wi-Fi** network with good coverage at every speaker (the
  ESP32-S3 has no 5 GHz radio).
- **All boards on the same access point.** The shared clock is that access
  point's beacon timer: each access point, and each node of a mesh system,
  has its own. A speaker on another access point logs a warning and stays
  silent.

## Assembly and wiring

The Gateway needs no wiring (only the optional calibration microphone, see
[What you need](#the-gateway)). Everything below is for the speakers.

> [!NOTE]
> The GPIOs below are the defaults and can be changed in `menuconfig`
> (see [Installation](#installation)). Wiring options may be extended in
> future versions; this section will be updated.

### Option A — I2S amplifier (MAX98357A)

<a href="images/wiring-max98357a.png"><img src="images/wiring-max98357a.png" alt="MAX98357A wiring" width="420" align="right"></a>

| ESP32-S3 | MAX98357A |
|---|---|
| GPIO 5 | BCLK |
| GPIO 6 | LRC (WS) |
| GPIO 4 | DIN |
| 5V | VIN |
| GND | GND |

- Speaker to **SPK+ / SPK−** (4–8 Ω).
- Leave **SD** and **GAIN** unconnected (default: (L+R)/2 output, 9 dB gain).
  Because both I2S slots carry the same channel, the result is that channel
  at full level.
- At full volume the amplifier draws up to ~1 A from 5 V: use a good USB
  supply and short power wires.

<br clear="right">

### Option B — DAC + any amplifier

<a href="images/wiring-pcm5102a-2way.jpg"><img src="images/wiring-pcm5102a-2way.jpg" alt="PCM5102A wiring with a 2-channel amplifier" width="420" align="right"></a>

| ESP32-S3 | PCM5102A |
|---|---|
| GPIO 5 | BCK |
| GPIO 6 | LCK (LRCK / WS) |
| GPIO 4 | DIN |
| 5V | VIN |
| GND | GND |
| — | SCK: not used (connect to GND, or close the module's SCK bridge) |

On the common purple PCM5102A module check the solder jumpers on the back:
**FLT = L, DEMP = L, XSMT = H** (otherwise the DAC stays muted), **FMT = L**
(I2S).

The DAC's **L OUT** and **R OUT** carry the same speaker channel, which lets
you use **any amplifier, small or powerful, one- or two-channel**:

| Amplifier | Connection | Setup on the speaker's EQ page |
|---|---|---|
| **One channel** (mono amp, a single side of a stereo amp, a subwoofer plate amp) | L OUT → amp input | nothing; for a subwoofer add a low-pass filter if the amp has none |
| **Two channels, one full-range speaker** | L OUT → amp, use one channel | nothing |
| **Two channels, active 2-way (woofer + tweeter)** | L OUT → CH1 → **woofer**, R OUT → CH2 → **tweeter** | a crossover: low-pass on the **Left output**, high-pass on the **Right output** |
| **Powered speaker / active subwoofer** | L/R OUT → its line input | nothing |

**Active 2-way crossover** (one example, adjust to your drivers):

1. Open `http://<speaker-ip>/eq` and tick **Enable EQ and preamp**.
2. **Left Output Filters** (woofer): two **LP** filters at the crossover
   frequency (e.g. 2500 Hz), **Q 0.707** each. Two stacked Butterworth
   filters make a 24 dB/oct Linkwitz-Riley slope.
3. **Right Output Filters** (tweeter): two **HP** filters at the same
   frequency, **Q 0.707** each.
4. Tweeters are usually louder: lower the **Right preamp** until both
   drivers sound balanced. Add peak or shelf filters to correct each
   driver's response.
5. **Apply & Save**: the change is live and stored, no restart needed.

> [!WARNING]
> Always set the tweeter's high-pass **before** playing loud. Full-range
> bass destroys a tweeter in seconds. The per-output **Mute** buttons on the
> EQ page help while setting up.

**Power:** the ESP32-S3 and the DAC run from USB 5 V; a larger amplifier gets
its own supply (e.g. 12 V ≥ 2 A). Connect the grounds of the DAC and the
amplifier (the signal ground), and keep the amplifier's power wires away
from the I2S wires.

<br clear="right">

### Optional extras (speakers)

| Function | Default GPIO | Notes |
|---|---|---|
| Amplifier enable / standby | 21 | switches the amplifier on while audio plays; active high or low |
| WS2812 RGB VU LED | 38 | the on-board LED of many DevKitC boards |
| Wired latency measurement (ADC loopback) | 7 | DAC output → coupling capacitor + bias divider → ADC pin |
| Sync test pulse (also on the Gateway) | 9 | for an oscilloscope, see [Measuring sync](#measuring-sync) |

## Installation

### 1. Install ESP-IDF

The firmware is built with **ESP-IDF v6.0.2**. Follow Espressif's
[installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html),
then open a terminal with the ESP-IDF environment loaded
(`source ~/esp-idf/export.sh`, or the path where you installed it).

### 2. Get the code

The Gateway is the **`main` branch** of the surround repository (the
earlier design, where every board is its own AirPlay receiver, is the
`standalone` branch):

```bash
git clone https://github.com/Kristian8606/esp32-airplay2-surround.git
cd esp32-airplay2-surround
```

### 3. Two projects, one code

| Folder | Builds | Firmware file |
|---|---|---|
| the repository folder | the **Gateway** | `build/airplay-gateway_<version>.bin` |
| `speaker/` | the **speakers** | `speaker/build/airplay-speaker_<version>.bin` |

| Code | Where |
|---|---|
| the Gateway's own (proxy, relays, speaker registry, speakers page); its AirPlay receiver (mDNS, pairing, RTSP) is in `components/ap2_core` | `main/` |
| the speaker's own (audio engine, link to the Gateway, EQ page) and FDK AAC | `speaker/main/`, `speaker/components/` |
| shared (web server, clock, sync protocol) | `common/`, `data/www/`, and the components in `components/` |
| menuconfig options of both | `components/airplay_options/` |

Both use the shared settings in `sdkconfig.defaults`; the Gateway adds
`sdkconfig.defaults.gateway`. Each is an ordinary ESP-IDF project: go into
its folder and run `idf.py` as usual.

### 4. Configure (optional)

Run `idf.py menuconfig` in the folder of the firmware you configure (the
repository folder for the Gateway, `speaker/` for the speakers). Both show
the same menus; some options matter for one firmware only:

| Menu | What you can set | For |
|---|---|---|
| **Surround Build Role** | the role (set by the folder: leave it); a fixed Gateway address for networks that filter mDNS; the sync test pulse | both |
| **AirPlay Receiver** | setup Wi-Fi name and password; playback buttons and AirPlay model (the icon senders show) | Wi-Fi both, the rest Gateway |
| **Multichannel Experiment** | 5.1 / 7.1 on or off, default speaker position | both |
| **I2S Playout** | BCLK / LRCK / DATA / MCLK GPIOs, output latency of your DAC, wired latency test; web admin password | speakers (password both) |
| **Amplifier Power Control** | amplifier enable GPIO and level | speakers |
| **RGB Audio/VU LED** | LED on/off and GPIO | both |
| **AirPlay Logs and Diagnostics** | log detail | both |

Every speaker runs the same firmware: its role is set later on the Gateway's
page.

### 5. Build and flash

Connect a board over USB.

**Speakers** (repeat for every speaker):

```bash
cd speaker
idf.py -p /dev/cu.usbmodem101 build flash monitor
```

**Gateway**, from the repository folder:

```bash
idf.py -p /dev/cu.usbmodem101 build flash monitor
```

On macOS list the ports with `ls /dev/cu.*`; on Windows the port is `COMx`,
on Linux usually `/dev/ttyACM0`. Every board's log goes to its USB port
(USB Serial/JTAG, so `idf.py monitor` shows it) and to its `/logs` page.

### 6. Updates

Later versions are installed **over Wi-Fi**:

- **Gateway:** on its main page, **Firmware Update** → choose
  `build/airplay-gateway_<version>.bin` → **Upload Firmware**.
- **Speakers, all at once:** on the Gateway's **speakers page**, panel
  **Speaker firmware** → **Choose .bin** →
  `speaker/build/airplay-speaker_<version>.bin` → **Upload**. The Gateway
  keeps it in its otherwise unused `storage` partition (1.9 MB) and every
  speaker with another build installs it by itself: one at a time and right
  away, also a speaker that was switched off and comes back later. A speaker
  that is playing stops its stream for the update (the others play on),
  checks size and SHA-256 and restarts into the new firmware at once; in a
  buffered stream (music, films) it plays again from the next track, skip or
  seek, in a realtime stream at once. Nothing waits for the playback to end;
  only a running room calibration holds the update back. Each tile shows
  `will update`, `updating…` or `update failed` (after three tries). If a
  speaker restarts and does not come back with the new build within
  3 minutes, the update **stops** there (`update stopped`)
  instead of going on to the next speaker; check that speaker, then upload the
  firmware again to continue. A new speaker firmware that cannot reach the
  Gateway (crashes at boot, no link) goes back to the previous one at its next
  restart (app rollback). The rollback needs the bootloader of v4.5.11 or
  later: flash every speaker once over USB (`idf.py flash` in `speaker/`).
- **One speaker by hand:** its own main page, **Firmware Update**, as for the
  Gateway.

The web interface is part of the firmware and is updated with it.

> [!IMPORTANT]
> **Speakers older than v4.5.9 cannot update from the Gateway** and need this
> once by hand on their own page. From v4.5.9 on the Gateway also updates
> speakers whose control protocol differs from its own (`protocol version …,
> Gateway speaks …` in its log), so a new Gateway brings the speakers up to it.
> A speaker image larger than the `storage` partition is refused on upload.

## First start

1. **Connect every board to Wi-Fi.** A new board opens the access point
   **`ESP32-AirPlay-Setup`**. Join it with a phone or laptop; the setup page
   opens by itself (or go to `http://192.168.4.1/`). Choose your network,
   enter the password, **Connect to WiFi**.
2. **Name the boards.** Find each IP address in your router (or the serial
   log), open `http://<board-ip>/` and set its name (**AirPlay Device Name**
   on the Gateway, **Speaker Name** on a speaker). The
   Gateway's name is what the Apple TV shows; clear speaker names
   (*Front Left*, *Centre*, …) make the speakers page easy to read.
3. **Speakers connect by themselves.** A speaker finds the Gateway on the
   network and shows up on the Gateway's speakers page; there is nothing to
   switch on.
4. **Give every speaker its role.** Open the Gateway's page
   `http://<gateway-ip>/speakers` (or **Open Speakers** on its main page),
   pick a role, level and delay for each speaker, **Save**.
5. **Choose the Gateway on the Apple TV.** Open Control Center, select the
   **AirPlay / audio output** icon and tick **only the Gateway**. The speakers
   are not listed there; they follow the Gateway.
6. **Play** something in surround (Apple Music Dolby Atmos or a film). The
   speakers page shows what every speaker plays right now, e.g.
   *AAC · 48 kHz · 24-bit · 7.1 · plays: Front Left*.

> [!TIP]
> A speaker that joins or restarts during a film stays silent until the next
> **skip or seek** (the Apple TV then restarts the stream at a point every
> speaker can start from). During screen mirroring it joins at once.

### Speaker positions

| Position | Where to put it | Notes |
|---|---|---|
| Front Left / Right (L / R) | ~30° left / right of the screen | main pair; music and effects |
| Centre (C) | above or below the screen | most of the dialogue |
| Subwoofer (LFE) | anywhere low, often near a corner | the low-frequency effects channel; with stereo, the L+R mix |
| Side Left / Right (Ls / Rs) | beside the listener, ~90–110° | the surround pair of 5.1 |
| Rear Left / Right (Lrs / Rrs) | behind the listener, ~135–150° | 7.1 only; with a 5.1 stream they play Side L / R |
| Mono mix | anywhere | every channel except LFE in one speaker; uses the most CPU |

> [!IMPORTANT]
> With 5.1 / 7.1 the **Subwoofer** plays only the LFE channel (the extra bass
> effects of films), **not** the bass of the other speakers. There is no bass
> management, so the front speakers should reproduce reasonable bass on their
> own. With music the LFE channel is often silent; with films it is busy.

## Settings

Everything is configured in the browser.

### Gateway: main page (`http://<gateway-ip>/`)

| Card | What it does |
|---|---|
| **Surround Speakers** | opens the speakers page below |
| **Playback** | play/pause, next, previous and volume of the current AirPlay sender |
| **WiFi Network** | scan and change the network |
| **Device Settings** | the AirPlay name the Apple TV shows |
| **Firmware Update** | OTA update with a `.bin` file |
| **System** | IP, MAC, memory, firmware, uptime, reset reason, Wi-Fi signal and channel; restart |
| **System Logs / WiFi Speedtest** | links to the pages below |

The Gateway has no audio output, so it has no EQ or latency cards.

### Gateway: speakers page (`/speakers`)

<a href="images/speakers-page-room.png"><img src="images/speakers-page-room.png" alt="Speakers page with the room map and three speakers playing a stereo stream" width="100%"></a>

At the top, a **room map** seen from above: the screen at the top, you in
the middle and all eight positions around you. An empty position is dashed,
a position with a speaker is outlined, and a speaker that is playing glows.
Two speakers on the same position are marked orange. Click a position to
jump to its speaker. Above the map: what is playing now
(`AAC · 48 kHz · Stereo`) and how many positions are taken.

<a href="images/speakers-page-tiles.png"><img src="images/speakers-page-tiles.png" alt="Speaker tiles with role, level, delay and live status" width="100%"></a>

Below it, one tile per connected speaker, side by side:

| Field | What it does |
|---|---|
| **Role** | which channel it plays: Front Left … Rear Right, or Mono mix |
| **Level (dB)** | trim from −30 to 0 dB; turn the louder speakers down, the quietest stays at 0 dB |
| **Delay (ms)** | 0–50 ms; about **2.9 ms per metre** a speaker is closer to the listener than the farthest one |
| **Live status** | coloured tags: format, rate and bits (`AAC · 48 kHz · 24-bit`), layout (`7.1`, `5.1`, `Stereo`), the channel actually played, buffered or realtime stream, playout sync and I2S correction, buffer fill, TSF quality |
| **Links** | the speaker's own page and its **EQ** |

Every speaker keeps its settings in its own flash and applies a change at
once, also while playing. A warning shows when two speakers have the same
role.

### Speaker: main page (`http://<speaker-ip>/`)

| Card | What it does |
|---|---|
| **Surround Speaker** | a reminder that role, level and delay are set on the Gateway's page |
| **WiFi Network / Device Settings** | network, and the speaker's name shown on the Gateway's page |
| **Output Latency** | delay of your hardware after the ESP (DAC, DSP), so all speakers stay in sync; *Measure* runs a wired loopback test |
| **Firmware Update / System** | OTA update, board status, restart |
| **Audio EQ / System Logs / WiFi Speedtest** | links to the pages below |

### Speaker: Audio EQ page (`/eq`)

| Card | What it does |
|---|---|
| **Surround speaker (5.1 / 7.1)** | role of this speaker (the same setting as on the Gateway's page; a change here shows there too) |
| **Loudness** | ISO 226 loudness compensation: restores bass and treble at low volume |
| **Left / Right Output Filters** | two independent chains of up to **24 biquad filters** each: peak, low-pass, high-pass, band-pass, notch, low shelf, high shelf; a **preamp** and a **mute** button per output |
| **Apply & Save** | applies live and stores in flash |

### Room calibration

An optional **INMP441 microphone** on the Gateway (wiring in
[The Gateway](#the-gateway)) measures every speaker from your seat and
suggests its **delay** and **level**, in about 2 s per speaker:

1. Put the Gateway with its microphone at your listening seat, at ear
   height (a power bank will do). Keep the room quiet.
2. **Test mic** checks the microphone and shows the room noise; −60 dBFS
   or quieter is good. Music may keep playing meanwhile.
3. **Calibrate**: if the Apple TV is connected, it is disconnected for the
   measurement (the speakers can only chirp between streams); afterwards
   pick the Gateway again as its audio output. Each speaker in turn plays
   four short chirps (300 Hz–8 kHz, 20 ms; the subwoofer 30–150 Hz, 200 ms)
   through its own EQ, so a 2-way speaker's crossover protects the tweeter:
   at −30 dBFS first, then 6 dB louder (up to −12 dBFS) while the
   microphone does not hear them clearly, or a full-range speaker's level
   is not yet 10 dB above the room noise. You hear one to four rounds per
   speaker; the level they were played at is taken out, so a louder round
   does not make a speaker read louder.
4. The table shows every speaker's **distance**, suggested **delay** (the
   farthest gets 0 ms, the others 2.9 ms per metre they are closer) and
   **level** (the quietest gets 0 dB, the others are turned down to it).
   **Apply to speakers** saves them like a manual edit; **Discard** changes
   nothing. What was measured is applied; a speaker without a delay or a
   level keeps the one it has, and the message names it. Delays and levels
   compare speakers, so each needs at least two full-range speakers
   measured. A speaker left unchanged was set against another reference:
   measure again once it plays clearly.

| In the table | Meaning |
|---|---|
| **± cm** after a distance | how much the four chirps disagree; a few cm is normal |
| **± dB** after a level | the same for the level |
| *Level not repeatable (… dB over the chirps)* | the levels still differ by more than 2 dB at −12 dBFS, usually from room noise; measure again in quiet |
| *Too quiet for a level: … dB above the noise, 10 needed* | even at −12 dBFS the chirp is under 10 dB above the room noise; its delay is applied, its level stays. Turn its amplifier up or measure in quiet |
| *Not heard clearly* | the chirps did not stand out of the noise even at −12 dBFS, or fewer than three of the four agree on the travel time within 0.5 ms |
| *Clipped at … dBFS: move the microphone away from it* | the recording reached full scale, which makes the level read low; the run is not used and nothing louder is tried |
| *No sound at −12 dBFS: check its cable and amplifier* | at the loudest level the chirp's shape is not found and it is under 6 dB above the room noise: left unchanged |
| *No other level to compare with: level unchanged* | it is the only full-range speaker with a level |
| *Only … of 4 chirps placed in the recording* | the speaker's chirp times could not be placed on the recording (TSF or microphone timing); louder would not help, so it is not tried |
| *Hint: the chirp came back inverted* | + and − may be swapped (or a 2-way speaker's tweeter is wired so); only a hint, nothing is changed |
| *Subwoofer: level and delay by ear* | its low chirp gives a delay, but no level |

Two runs in a quiet room should agree within a few centimetres and about
0.5 dB; if they do not, measure again before applying.

How it measures:

- **Travel time without cables.** A speaker plays its chirps when it can
  and reports when each one left it, on the access point's TSF (its output
  latency included); the Gateway timestamps its recording on the same TSF.
  The difference is the sound's travel time.
- **The direct sound, not an echo.** The arrival is the first correlation
  peak within 12 dB of the strongest one: the direct sound always comes
  first, while a reflection off a wall can be louder.
- **One disturbed chirp is left out.** A chirp whose travel time is far
  from the others' (a door, a cough) does not count; three of the four
  must agree within 0.5 ms.
- **Level as the ear hears it:** the RMS of the chirp and the next 10 ms of
  the room (full-range speakers above 200 Hz), the median of the chirps,
  with the quiet room's noise in the same band taken out. It is used only
  10 dB or more above that noise.
- **Tested in a simulated room** (the real `gw_calib.c` on a computer, with
  noise, reflections, a 2-way crossover, a loose cable, a door slam and a
  clipping microphone): distances within 1 mm, trims within their 0.5 dB
  rounding, a reflection up to 12 dB louder than the direct sound ignored.
- **Every round is in the log** (`gw_calib`): level played, chirps found,
  travel time, level, dB above the noise, recording peak, match, and why
  the next round is louder or why it stops.

*No signal from the microphone: check its wiring* means the Gateway clocks
the microphone but reads a constant value: check VDD → 3V3, GND, L/R → GND
(otherwise it sends on the other channel), SD → GPIO 4, SCK → GPIO 5,
WS → GPIO 6. Without a microphone signal the speakers still play their
chirps one after another (so you hear that each one works), and the panel
says nothing was measured.

<a href="images/speakers-page-calibration.png"><img src="images/speakers-page-calibration.png" alt="Room calibration panel after a run without a microphone: every speaker played its chirps, nothing measured" width="100%"></a>

### Other pages (every board)

- **`/logs`**: live log of the board over WebSocket.
- **`/speedtest`**: ping, download and upload between the browser and the
  board, to check Wi-Fi at the speaker's (or the Gateway's) position.

**Security:** with a **Web UI admin password** set in `menuconfig`, firmware
updates, restart, Wi-Fi and name changes and the latency test require the
user `admin` and that password.

## Technical details

### Audio formats

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

### Channels

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

### Stereo sources

With a stereo stream every speaker decodes both channels and takes what its
role needs: **Front L / R** play left / right, **Subwoofer** and **Mono mix**
play (L + R) / 2, **Centre, Side and Rear** stay silent. 5.1 and 7.1 streams
are never touched by this.

### Decoding and signal path (on each speaker)

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
   (L = R).

### Synchronisation

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

The Gateway has one clock implementation, in
`common/network/apple_clock.c` and `apple_clock_core.c`. A speaker's
`common/network/ptp_clock.c` is only the external TIME state and conversion
interface for its audio engine; it has no PTP sockets or passive estimator.
The optional oscilloscope pulse on the Gateway reads the same published
Apple line. The algorithm is described in `apple_clock_core.h`.

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

### Relaying the stream

| Stream | How the Gateway passes it on |
|---|---|
| **Buffered** (type 103: music, films) | one TCP connection per speaker, a copy of the sender's. The Gateway reads the sender whenever its 4 MiB PSRAM buffer has room and sends each speaker from it; when the buffer is full (a speaker keeps old audio longest), the sender waits. Speakers keep up to ~90 s of compressed audio. Sending is paced at twice real time after a 2 s start: at full speed to every speaker at once it kept the Wi-Fi so busy that the sender's clock packets arrived late. At an immediate FLUSH (an iPhone's track change) each speaker's stream skips to the new track at its next frame boundary; read and sent at paced speed only to be dropped, the old track took 2-4 s per change. |
| **Realtime** (type 96: screen mirroring, system audio) | every UDP audio and sync packet is copied to each speaker's ports. A speaker that misses a packet asks the Gateway; several requests for the same packet reach the Apple TV once. |

A speaker that connects in the middle of a session gets the session's
settings replayed (format, key, volume, timing). With a buffered stream it
waits for the next flush (skip or seek), because it can only start decoding
at a point every speaker starts from; with a realtime stream it joins at
once.

### Performance and memory

| | Gateway | Speaker (one channel of 7.1) |
|---|---|---|
| CPU | relays only, no decoding (not yet measured) | ~31–34 % of one core (measured on a rear speaker) |
| Audio buffer | none (TCP backpressure) | 6 MB compressed stream buffer in PSRAM |
| Decode task stack | — | 20 KiB in PSRAM (deepest use ~11 KiB) |
| Free PSRAM during 7.1 | — | ~400 KiB |
| Firmware image | ~1.26 MB | ~1.79 MB (no AirPlay receiver code) |

| Board | Core 0 | Core 1 |
|---|---|---|
| Gateway | RTSP, PTP | Wi-Fi, LwIP, buffered and realtime relays |
| Speaker | AAC decoding | Wi-Fi, LwIP, stream reader, I2S playout, status / LED / log tasks |

The FDK changes (single-channel synthesis, 32-bit output, hardware bit
counting on the ESP32-S3, element skipping, smaller buffers, lazy limiter)
are listed in
[`speaker/components/fdk_aac/MODIFICATIONS.md`](speaker/components/fdk_aac/MODIFICATIONS.md).

### Under the hood

| Part | Where | What it does |
|---|---|---|
| AirPlay 2 core | `components/ap2_core` | pairing, plists, audio decryption, the FairPlay setup replies, the RTSP server (`rtsp` in the log) and the mDNS announcement (`mdns_airplay`), the last two Gateway only, under Apache-2.0 (the FairPlay tables excepted); both firmwares |
| Web server | `components/web_server` | the shared pages and APIs, captive portal (`web_server` in the log), under Apache-2.0; both firmwares |
| Settings | `components/settings` | device name, Wi-Fi networks, volume and the speaker settings in NVS, under Apache-2.0; both firmwares |
| Status LED | `components/led` | the AirPlay state and a level meter on the WS2812 LED (`led` in the log), under Apache-2.0; both firmwares |
| Helpers | `components/helpers` | session events, bound sockets, captive DNS (`dns_server` in the log), under Apache-2.0; both firmwares |
| Wi-Fi | `components/wifi` | station on a saved network, setup access point, host name (`wifi` in the log), under Apache-2.0; both firmwares |
| Live log, firmware update | `components/log_stream`, `components/ota` | the log on `/logs`; update from a `.bin` upload (`ota` in the log), under Apache-2.0; both firmwares |
| Web pages | `main/web_gateway.c`, `speaker/main/web_speaker.c` | each firmware's own pages and APIs on top of the shared web server |
| Gateway proxy | `main/gw_proxy.c` | implements the audio engine API on the Gateway and forwards each call (`FWD …` in the log) |
| Speaker registry | `main/gw_speakers.c` | control link on port 7100 (`_apsurround._tcp`), late joins, settings, status, TIME messages, which speaker updates next |
| Speaker firmware | `main/gw_fw.c`, `speaker/main/spk_update.c` | the image in the Gateway's `storage` partition (`gw_fw`); download, check and install on the speaker (`spk_update`) |
| Buffered relay | `main/gw_relay.c` | reads the TCP audio stream into a buffer and copies it to every speaker; drops the old track at a FLUSH (`RELAY …`) |
| Realtime relay | `main/gw_rt_relay.c` | copies UDP audio and sync packets, passes retransmit requests on (`RT RELAY …`) |
| Apple clock | `common/network/apple_clock.c`, `common/network/apple_clock_core.c` | Gateway-only two-way PTP transport, RTT filtering, rate estimation and continuous published line (`apple_clock …`) |
| Time distribution | `common/surround/tsf_fit.c`, `main/gw_speakers.c`, `speaker/main/spk_time.c` | TSF fits on both sides; maps the same Apple line into TIME and each speaker's local clock |
| Speaker clock state | `common/network/ptp_clock.c` | external TIME state, rate carry and conversions for the audio engine; no PTP receiver |
| Speaker link | `speaker/main/spk_link.c` | finds the Gateway, runs every forwarded call locally (`CMD …`) |
| Protocol | `common/surround/surround_proto.h` | message types and payloads, versioned |
| Audio engine | `speaker/main/audio` | FIFO, FDK AAC / ALAC decode, PCM store, I2S playout and servo |
| Room calibration | `main/gw_calib.c`, `main/gw_mic.c`, `common/surround/cal_chirp.c` | microphone capture, chirp search, suggestions (`gw_calib` in the log); the speaker side plays the chirps (`ROOM CAL …`) |

## Measuring sync

1. Enable **Surround Build Role → Sync test pulse** on the Gateway and on two
   speakers (GPIO 9 by default). Each board raises the pin for 10 ms at every
   whole second of the Apple TV's clock, as it sees it, and logs
   `PULSE … interrupt late avg … max …`.
2. Put the three pins on an oscilloscope, trigger on the Gateway, play
   anything. Gateway → speaker is a speaker's clock error; speaker →
   speaker is the difference the listener would hear before playback adds
   its own.
3. For the full chain give both speakers the role **Mono mix**, play a click
   track and compare the two DAC outputs:

   ```bash
   ffmpeg -f lavfi -i "aevalsrc='if(lt(mod(t,1),0.001),0.8*sin(2*PI*4000*t),0)':s=48000" -t 600 -ac 2 -c:a aac -b:a 256k clicks.m4a
   ```

## Limitations

- **Experimental.** Tested with one Apple TV, a Gateway and two speakers
  playing together (three connected): 7.1 and stereo AAC, skip, seek, pause,
  volume, role and level changes while playing, a second sender taking over.
- **Room calibration** is tested with an INMP441 and up to six speakers.
  An echo more than 12 dB louder than the direct sound (a speaker behind an
  obstacle) still misleads its timing, and the subwoofer's level is left
  to the ear. Its delay includes its own crossover's delay and the
  microphone's low cut (about 1 ms in the simulation), so check it by ear.
- **Not yet tested:** realtime (screen mirroring, system audio) through the
  Gateway, the stereo routing to front pair and subwoofer, oscilloscope
  measurements, more than three speakers, a weak Wi-Fi signal.
- **Late joins:** a speaker that connects during a film waits for the next
  skip or seek; until then it stays silent.
- **One access point.** All boards must be associated with the same access
  point (on a mesh, the same node).
- **Playout servo:** each speaker holds its own playout within ±0.4 ms; two
  speakers differ by 0.1–0.3 ms. That is inaudible as an echo but could be
  tightened for a perfectly fixed stereo centre.
- **No bass management**: with 5.1 / 7.1 the Subwoofer plays only LFE.
- The **7.1 Front / Side naming** is not yet confirmed (see
  [Channels](#channels)).
- **Mono mix** decodes all channels and may not keep up with 7.1.
- A switch between 44.1 and 48 kHz has a short silence while I2S is
  reconfigured.
- **Rear and side channels are often nearly silent in music**; they come
  alive with films and Atmos mixes.
- A sender can postpone audio after AirPlay setup. Distinguish that from
  clock acquisition using `apple_clock` readiness and the speaker's
  `PLAYOUT START` / timing-watchdog logs; a long wait is not automatically
  a sender delay.

## Troubleshooting and logs

| Problem | Check |
|---|---|
| A speaker does not appear on `/speakers` | Same Wi-Fi as the Gateway; its log says `connected to Gateway …`. If mDNS is filtered, set **Surround Build Role → Gateway IPv4 address** on the speaker. |
| A speaker still shows up as an AirPlay speaker on an iPhone | It runs an old firmware from before the Gateway design: update it. |
| Which build does a speaker run? | Its tile shows `fw v4.5.5 7c5d1e74`: the version plus a code that changes with every build. A speaker on a different build than the others is marked; the Gateway's own is under the page title. |
| A speaker connects and drops again | `protocol version …, Gateway speaks …` in the Gateway log: update both to the same firmware. |
| A speaker is silent after joining | Wait for the next skip or seek (`speaker … joins at this FLUSH`). Then its role on `/speakers`, the **Mute** buttons on its EQ page, the PCM5102A **XSMT** jumper, the BCLK / LRCK / DATA wiring. |
| `no common TSF, no sender time` on a speaker | It is on another access point (or mesh node) than the Gateway. |
| `TIMING WATCHDOG ... reason=no-sender-time` | Check Gateway `apple_clock` readiness, Announce and Delay_Req/answered counters. Audio can already be buffered while the clock is acquiring. |
| No sound at all after a Gateway update or restart | The Apple TV dropped the session: select the Gateway again as the audio output. |
| The Apple TV sends stereo only | The track or app may only offer stereo (YouTube, some "Atmos" tracks); the speakers page shows the layout. |
| Subwoofer silent with surround music | Normal: most music has no LFE. Try a film. |
| Dropouts | Wi-Fi signal at that speaker and at the Gateway (`/speedtest`, RSSI on the main page). |
| One speaker early or late | Its **Delay** on `/speakers`; its **Output Latency** (DSP, external processors). |

Every board streams its log to `http://<board-ip>/logs`. Useful lines:

| Line | Where | Meaning |
|---|---|---|
| `FWD set_format AAC 48000 Hz …` | Gateway | an Apple TV instruction forwarded to the speakers |
| `RELAY rx … \| buffered … KiB, flushed … pkts, paced … ms \| … RELAY tx Front:live …` | Gateway | audio in from the sender and out to each speaker; how much waits in the buffer, frames dropped by a FLUSH, how long sending was held back |
| `RT RELAY data … pkts -> N speakers \| NACK …` | Gateway | realtime audio and retransmit requests |
| `control connection from …` / `speaker … joins at this FLUSH` | Gateway | a speaker connected / started playing |
| `two-way clock owns UDP 319/320 ...` | Gateway | the standalone Apple clock owns the PTP transport |
| `master ... announces itself from <ip>` | Gateway | a device that is its own PTP master talks to the Gateway |
| `master ...: new clock ..., measured at the master <ip> (the sender itself / the sender relays it, N steps)` | Gateway | which device gives the time; `waiting for it to announce itself` until the master talks to the Gateway |
| `line ready epoch 1: N exchanges, 1 fit points ...` | Gateway | the line started from the first second whose best exchange is fast |
| `apple_clock: ready epoch ... floor RTT ... seconds +N -M ... steps ... jumps ... Delay_Req ... answered ...` | Gateway | clock line, RTT floor, used/unused seconds, steps of Apple's clock, line restarts, exchange counters |
| `TIME apple_clock ready=1 epoch=... rate=... (TSF)` | Gateway | the published Apple line mapped onto TSF and sent to the speakers |
| `TIME tsf fit 64 samples ... rms ... locked=1` | speaker | local TSF fit quality and external clock state |
| `PLAYOUT START ... first audio N ms after anchor` | speaker | measured startup wait; the status task may print the event later |
| `ROLE: Surround speaker (follows the Gateway)` | speaker | boot banner of the speaker firmware |
| `connected to Gateway …` | speaker | the control link is up |
| `CMD start_buffered: ESP_OK, audio port …` | speaker | a forwarded instruction run locally |
| `sender time from the Gateway: gm=…` | speaker | the speaker has the Apple TV's clock |
| `audio_status: AAC \| 48000 Hz \| … sync=+0.06ms \| i2s=-51ppm …` | speaker | playback, its sync error and I2S correction |
| `NOW PLAYING \| AAC-LC \| 48000 Hz \| 24-bit (F24) \| 7.1 (8ch) \| Front Right (R)` | speaker | the channel actually played |
| `stereo sources: silent (role Centre (C))` | speaker | what this role plays from a stereo song |
| `MC CPU … avg …% of one core` | speaker | decoding time per frame |

For deeper debugging enable **AirPlay Logs and Diagnostics → Log every RTSP
request and response** in `menuconfig` on the Gateway (verbose; turn it off
afterwards).

## Licenses and credits

- **This repository**, from version v4.5.64 on, is under the **Apache
  License 2.0**, Copyright 2026 Kristian Dimitrov: [`LICENSE`](LICENSE),
  [`NOTICE`](NOTICE). The components in `components/` carry their own copy
  of it and a `NOTICE`. The exceptions are below.
- **Older versions.** The project started from
  [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32).
  Commits and tags up to and including **v4.5.63** contain code from it and
  stay under its **Non-Commercial License**, Copyright (c) 2026 Remi
  Bouteiller (the `LICENSE` in those versions). The parts that came from it
  were written again from behaviour specifications, and the whole
  repository was compared line by line with that project's history before
  this change.
- **FairPlay setup replies** (`components/ap2_core/src/fairplay/fairplay_tables.c`)
  are fixed interop data from reverse engineering, not under the Apache
  License; see [`components/ap2_core/NOTICE`](components/ap2_core/NOTICE).
- **Fraunhofer FDK AAC** (`speaker/components/fdk_aac/fdk-aac`) is under the
  Fraunhofer FDK license, [`NOTICE`](speaker/components/fdk_aac/fdk-aac/NOTICE). The
  copy here is a **Third-Party Modified Version of the Fraunhofer FDK AAC
  Codec Library for Android**; the changes and their dates are listed in
  [`MODIFICATIONS.md`](speaker/components/fdk_aac/MODIFICATIONS.md). The license
  allows redistribution in source and binary form without copyright fees but
  **grants no patent rights**: AAC patent licenses are available through Via
  Licensing or the patent owners.
- Other components (ESP-IDF, `esp_audio_codec`, libsodium, mDNS, cJSON,
  led_strip) come through the ESP-IDF component manager under their own
  licenses.

Grows out of the `standalone` branch of
[esp32-airplay2-surround](https://github.com/Kristian8606/esp32-airplay2-surround),
where every board is its own AirPlay receiver playing one channel, and
[airplay-esp32](https://github.com/Kristian8606/airplay-esp32), the stereo
AirPlay 2 receiver for the ESP32-S3. AAC decoding through
[fdk-aac](https://github.com/mstorsjo/fdk-aac).

AirPlay, Apple TV and HomePod are trademarks of Apple Inc. This project is not
affiliated with or endorsed by Apple.

<p align="center">
  <br>
  If this project helped you build your surround system, please consider
  supporting it. 🙏
  <br><br>
  <a href="https://www.paypal.com/donate/?business=VGY3ZM78NAL78&no_recurring=0&currency_code=EUR"><img src="https://img.shields.io/badge/Donate-PayPal-ff5e5b?style=for-the-badge&logo=paypal&logoColor=white" alt="Donate with PayPal"></a>
</p>
