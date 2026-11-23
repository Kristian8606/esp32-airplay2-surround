<p align="center">
  <b>Enjoying this project?</b> It is built and tested in my free time. A small
  donation helps buy test hardware and keeps the development going. Thank you! ❤️
  <br><br>
  <a href="https://www.paypal.com/donate/?business=VGY3ZM78NAL78&no_recurring=0&currency_code=EUR"><img src="https://img.shields.io/badge/Donate-PayPal-ff5e5b?style=for-the-badge&logo=paypal&logoColor=white" alt="Donate with PayPal"></a>
</p>

<p align="center">
  <a href="images/stream.png"><img src="images/stream.png" alt="Wireless surround 7.1 / 5.1 / 2.1 from an ESP32 streamer: USB, radio and AirPlay to wireless speakers" width="100%"></a>
  <br><sub><i>The Gateway as a streamer. The screen on its front is coming: display support is planned.</i></sub>
</p>

# ESP32 AirPlay 2 Surround Gateway

**Wireless 5.1 / 7.1 surround sound from an Apple TV, built from low-cost
ESP32 boards: one Gateway (an ESP32-S3, or an ESP32-P4 for the most
features), and one ESP32-S3 per speaker.**

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
- 🏠 **Apple Home (HomeKit)** pairing, artwork, playback and volume controls
  for the whole surround system as one speaker.
- ⏱️ **Microsecond clock alignment** between speakers through the access
  point's beacon timer (TSF). No extra hardware, no cables.
- 🎤 **Room calibration** with an optional INMP441 microphone on the
  Gateway (or the ESP32-P4 board's own microphone): distance, delay and
  level of every speaker measured from your seat, applied with one click.
- 📈 **Room EQ** with the same microphone: each speaker's frequency
  response measured from your seat and corrected with its own EQ, with
  settings like REW's.
- 📻 **Web radio** on the Gateway: stations of your country, a search, your
  favourites or any stream address (MP3 or AAC over HTTP / HTTPS), played on
  all speakers like an AirPlay stream. AirPlay always takes over.
- 💾 **USB player** on the Gateway (experimental): FLAC,
  WAV, ALAC, AAC and MP3 from a USB drive, **bit for bit** to the speakers,
  5.1 / 7.1 files each channel on its own speaker, gapless, with tags,
  covers and m3u playlists ([USB player](docs/usb-player.md#usb-player-experimental)).
- 🎛️ **One page for all speakers**: role, level and delay of every speaker,
  and a live view of what each one plays. Each speaker keeps its own
  2 × 24-band parametric EQ, mono or **bi-amp** (woofer + tweeter through a
  Linkwitz-Riley crossover), loudness and latency settings.
- 🔀 **Format changes on the fly**: the Apple TV can switch between stereo and
  surround from track to track; the speakers follow without reconnecting.
- ⚡ About **a third of one CPU core** per speaker for 7.1, thanks to a patched
  Fraunhofer FDK AAC decoder that synthesises just one channel.
- 🔗 **Speakers belong to the system**: a speaker is not an AirPlay
  receiver of its own and never shows up on an iPhone; it just follows the
  Gateway. (A board that is its own AirPlay receiver is the `standalone`
  branch.)
- 🚀 **Two Gateway boards, one code**: an ESP32-S3, the same board as the
  speakers, or a **DFRobot FireBeetle 2 ESP32-P4**, which does the most:
  twice the relay buffer, a fast USB port for heavy 5.1 / 7.1 WAV files,
  quick cover thumbnails and a microphone on the board
  ([S3 or P4](#esp32-s3-or-esp32-p4)). The chip is picked with
  `idf.py set-target`.

> [!NOTE]
> **The project is under active development.** A Gateway and five speakers
> (front pair, centre, side pair) play 5.1 and 7.1 from an Apple TV and
> stereo from an iPhone, each speaker its own channel, audibly in sync.
> Features, defaults and wiring may still change, and this README is updated
> with every change. See [Limitations](docs/troubleshooting.md#limitations).

> [!IMPORTANT]
> **16 MB flash modules (ESP32-S3 N16R8).** The 8 MB flash of the N8R8
> boards is nearly full: the Gateway keeps a copy of the speaker firmware to
> update the speakers, and that copy has only about 110 KB of room left. A
> board with 16 MB can already use its full flash
> ([16 MB flash](docs/install.md#16-mb-flash)); new features will need it. **If you buy
> boards now, buy N16R8.** 8 MB boards build and update as before. The
> ESP32-P4 Gateway has 16 MB anyway.
>
> **Planned:**
> - 🔊 **USB sound card** support.
> - 📺 **DLNA**: play to the system from DLNA / UPnP apps, not only AirPlay.
> - 🖥️ **A display on the Gateway**: what plays, its cover, the source and
>   the controls, as in the picture at the top.

## Contents

- [Gallery](#gallery) (all pictures: [GALLERY.md](GALLERY.md))
- [Apple Home (HomeKit)](#apple-home-homekit)
- [How it works](#how-it-works)
- [What you need](#what-you-need)
- [Quick start](#quick-start)
- [First start](#first-start)
- [Licenses and credits](#licenses-and-credits)

**Guides** in [`docs/`](docs/):

| Guide | What is in it |
|---|---|
| [Assembly and wiring](docs/hardware.md) | the speaker's I2S amplifier or DAC, GPIOs, optional extras |
| [Installation](docs/install.md) | ESP-IDF, the code, configuration, 16 MB flash, boards, building, flashing and updates |
| [Settings](docs/web-pages.md) | every web page of the Gateway and of the speakers |
| [Room calibration and Room EQ](docs/calibration.md) | measuring the speakers from your seat; measuring sync with an oscilloscope |
| [USB player](docs/usb-player.md) | music from a USB drive (experimental) |
| [Technical details](docs/technical.md) | formats, channels, signal path, synchronisation, relaying, memory, the code |
| [Limitations and troubleshooting](docs/troubleshooting.md) | what is not done or not tested, problems and log lines |

## Gallery

**All pictures** with a short description, AirPlay with its artwork
included: [GALLERY.md](GALLERY.md).

<table>
  <tr>
    <td align="center" width="50%">
      <a href="images/airplay-surround-dark.png"><img src="images/airplay-surround-dark.png" alt="ESP32 AirPlay 2 Surround 7.1" width="100%"></a>
      <br><sub><b>7.1 around the listener:</b> one Gateway, one ESP32-S3 board in every speaker</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/airplay-surround-light.png"><img src="images/airplay-surround-light.png" alt="Surround setup around a TV" width="100%"></a>
      <br><sub>One board in every speaker, all fed wirelessly through the Gateway</sub>
    </td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <a href="images/speakers-page.png"><img src="images/speakers-page.png" alt="Gateway speakers page in the Warm theme: an album from the USB stick playing with its cover, and the room map with the front pair lit" width="100%"></a>
      <br><sub><b>Gateway speakers page:</b> what plays now, here an album from the USB stick bit for bit on the front pair, and the room map with the positions that play</sub>
    </td>
  </tr>
</table>

## Apple Home (HomeKit)

Add the Gateway as a speaker in **Apple Home** to see artwork for the current
music or video and control playback and volume. The surround system appears
as **one accessory**, with every speaker keeping its assigned channel.

HomeKit support, setup code, manufacturer and artwork advertising can be
configured under **AirPlay Receiver** in `idf.py menuconfig`.

<table>
  <tr>
    <td align="center" width="50%">
      <a href="images/homekit-gateway.jpg"><img src="images/homekit-gateway.jpg" alt="ESP32 Gateway in Apple Home with film artwork, playback and volume controls" width="300"></a>
      <br><sub>The Gateway in Apple Home</sub>
    </td>
    <td align="center" width="50%">
      <a href="images/homekit-player.jpg"><img src="images/homekit-player.jpg" alt="Apple Home speaker view showing artwork and playback controls" width="300"></a>
      <br><sub>Artwork and playback controls</sub>
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
   a 4 MiB buffer (8 MiB on the ESP32-P4) and sends each speaker from it, so
   at a track change the
   old track is dropped from the buffer at once instead of being sent only
   to be thrown away.
3. **Every speaker decodes only its own channel.** One ESP32-S3 cannot decode
   all eight channels in real time, and it does not need to. A speaker is
   not an AirPlay receiver: it finds the Gateway by mDNS, decrypts the stream
   with the key the Gateway passed on and plays its channel
   on **both outputs** of its DAC or amplifier (L = R), so you can drive one
   speaker or a subwoofer amplifier, or, in **bi-amp** mode, the woofer on one
   output and the tweeter on the other through its crossover.
4. **One two-way Apple clock for all.** The Gateway measures both directions
   of the PTP path with Sync / Follow_Up and Delay_Req / Delay_Resp, then
   publishes one smooth clock line. Every board on the same access point
   shares its beacon timer (TSF). Eight times a second the Gateway tells the
   speakers "the Apple TV's time P is TSF T". Each speaker converts T to its
   own clock and plays every sample at the moment the Apple TV scheduled it.
   (On the ESP32-P4 the Wi-Fi radio is the board's ESP32-C6: a firmware of
   this project on the C6, installed by the Gateway itself, marks the TSF
   with a pulse on a wire that the P4 timestamps in hardware.)

You choose each speaker's role (Front Left, Centre, Subwoofer, …) on the
Gateway's page, even while music is playing. Start with two or three
speakers and add the rest later.

```
                       ┌─────────────────────────────┐
   Apple TV ──AirPlay──►  Gateway (ESP32-S3 or -P4)   │
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

The subwoofer plays below its **Crossover** set on the speakers page (80,
100, 120 or 150 Hz, 120 Hz by default, or Off when its amplifier has its own),
the stereo mix and the LFE channel alike. Do not add a low-pass on its own EQ
page as well, or the bass is cut twice.

## What you need

### The Gateway

| Part | Notes |
|---|---|
| **One ESP32-S3 board**, the same type as the speakers (N8R8, or N16R8: see the note at the top), with a **UART port** besides its native USB port (DevKitC boards have both) | It has no audio output: no DAC, no amplifier, no wiring. A USB supply is all it needs. It is flashed and logs over its UART port; the native USB port takes the [USB player](docs/usb-player.md#usb-player-experimental)'s drive. |
| *Or:* **a DFRobot FireBeetle 2 ESP32-P4** (16 MB flash, 32 MB PSRAM, Wi-Fi through its ESP32-C6) | The Gateway with the most features (see below). Built with `idf.py set-target esp32p4` ([Boards](docs/install.md#boards), [bring-up](docs/p4-gateway-bringup.md)). The speakers stay ESP32-S3. |
| **A place with a good signal** | It receives the stream from the Apple TV and sends it again to every speaker, so it carries the most Wi-Fi traffic. Near the access point is best. |
| *Optional:* **a USB stick** | Music for the [USB player](docs/usb-player.md#usb-player-experimental): FAT32. On the S3 it goes on the native USB port, which needs 5 V from the board; on the P4 on its high-speed USB port. |
| *Optional:* **INMP441 I2S microphone** | For [room calibration](docs/calibration.md#room-calibration): VDD → 3V3, GND and L/R → GND, SCK → GPIO 5, WS → GPIO 6 (GPIO 52 on the P4), SD → GPIO 4 (changeable in `menuconfig`). The FireBeetle 2 ESP32-P4 has a PDM microphone on the board, used by default. |

#### ESP32-S3 or ESP32-P4

Both run the same firmware, built for their chip. The **ESP32-P4 supports
the most features**; the S3 does everything the speakers need, with the
limits below.

| | ESP32-S3 (N8R8 / N16R8) | ESP32-P4 (FireBeetle 2) |
|---|---|---|
| CPU, memory | 2 cores at 240 MHz, 8 MB PSRAM | 2 cores at 360 MHz, 32 MB PSRAM |
| Wi-Fi | its own, 2.4 GHz | the board's ESP32-C6, 2.4 GHz; the Gateway installs the C6's firmware itself |
| Relay buffer (AirPlay music and films) | 4 MiB | 8 MiB |
| USB port for the player | full speed, about **8.4 Mbit/s** measured: a **5.1 / 7.1 WAV** is too heavy and stops every few seconds to buffer; FLAC of the same file plays | **high speed** (USB 2.0), its own port: no bitrate warning |
| 7.1 at 24-bit from the USB player | a dense film mix is at the limit of the CPU and stops now and then; 16-bit plays | more CPU to spare |
| AirPlay artwork on the Now playing card | — (the picture would take up to 2 MB of the memory the relay needs; Apple Home still shows its own) | yes |
| Covers of USB music | sent to the page as they are in the file | turned into small thumbnails by the P4's JPEG hardware and kept on the stick (folder `.airplay-covers-v1`), so the page shows them quickly |
| Player start | 2 s | 1 s |
| Calibration microphone | INMP441 | INMP441, or the PDM microphone on the board |
| Console, flashing | its UART port (the native USB port is the player's) | the board's USB-C (USB-Serial/JTAG) |
| Flash | 8 MB, or 16 MB ([16 MB flash](docs/install.md#16-mb-flash)) | 16 MB, with 6 MB firmware slots |

The heavy-file limits of the S3 are measured; the P4's high-speed port has
not yet been measured with a heavy 7.1 WAV.

### Per speaker (one set for every position)

| Part | Notes |
|---|---|
| **ESP32-S3 board with 8 MB flash and 8 MB PSRAM** (N8R8), e.g. ESP32-S3-DevKitC-1 N8R8, or with 16 MB flash (N16R8, see the note at the top) | PSRAM is required: the stream buffer alone takes 6 MB. Boards without PSRAM (or with 2 MB) will not work. |
| **Audio output**, one of: | |
| &nbsp;&nbsp;• an **I2S amplifier** such as **MAX98357A** | Simplest: digital in, speaker out, about 3 W. Good for small satellites and surrounds. |
| &nbsp;&nbsp;• an **I2S DAC** such as **PCM5102A** + **any analogue amplifier** | Any size: mini (PAM8403, 2 × 3 W), medium or powerful (TPA3110 / TPA3116, TDA7498, a plate amplifier for a subwoofer…), **one- or two-channel**. |
| **Speaker(s)**, 4–8 Ω | One full-range driver, or a woofer + tweeter pair (see [Option B](docs/hardware.md#option-b--dac--any-amplifier)). |
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
  ESP32-S3 and the P4 board's ESP32-C6 have no 5 GHz radio).
- **All boards on the same access point.** The shared clock is that access
  point's beacon timer: each access point, and each node of a mesh system,
  has its own. A speaker on another access point logs a warning and stays
  silent.

## Quick start

The whole way from download to flashed boards;
[Installation](docs/install.md) explains each part.

**Once: ESP-IDF v6.1 and the code**

```bash
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf-v6.1
~/esp-idf-v6.1/install.sh esp32s3,esp32p4
git clone https://github.com/Kristian8606/esp32-airplay2-surround.git
```

**In every new terminal**

```bash
. ~/esp-idf-v6.1/export.sh
cd esp32-airplay2-surround
```

**The Gateway: pick its chip, build, flash.** The firmware goes to
`build/airplay-gateway_<version>.bin`.

| | ESP32-S3 Gateway (N8R8) | ESP32-P4 Gateway (FireBeetle 2) |
|---|---|---|
| Pick the chip | nothing to do: the S3 is the default | `idf.py set-target esp32p4` (once) |
| Connect | the **UART** port (`/dev/cu.usbserial-…` on a Mac) | the board's **USB-C** (`/dev/cu.usbmodem…`) |
| Build and flash | `idf.py -p PORT build flash monitor` | `idf.py -p PORT build flash monitor` |
| First start | as usual | if the C6 does not run this project's firmware yet, the Gateway installs it over the C6's link and restarts once by itself |

For an **N16R8** (16 MB) S3, set the flash size and the partition table in
`idf.py menuconfig` before the first build ([16 MB flash](docs/install.md#16-mb-flash)).

**The speakers** (always ESP32-S3), over each speaker's **USB** port:

```bash
cd speaker
idf.py -p PORT build flash monitor
```

Then [First start](#first-start): Wi-Fi, names, roles. Later versions go
over Wi-Fi ([Updates](docs/install.md#6-updates)); the speakers update from the Gateway.

> [!NOTE]
> `set-target` starts the settings again from the defaults. To go back from
> a P4 to an S3 build in the same folder, run `idf.py set-target esp32s3`.


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

## Licenses and credits

- **This repository** is under the **Apache License 2.0**, Copyright 2026
  Kristian Dimitrov: [`LICENSE`](LICENSE), [`NOTICE`](NOTICE). The project
  has been rewritten in full; the components in `components/` carry their
  own copy of the license and a `NOTICE`. The exceptions are below.
- **AirPlay 2 protocol core** (`ap2_core`, `apple_clock`) comes from
  [esp-airplay2-core](https://github.com/Kristian8606/esp-airplay2-core)
  (Apache License 2.0) through the ESP-IDF component manager. Its
  **FairPlay setup replies** (`src/fairplay/fairplay_tables.c`) are fixed
  interop data from reverse engineering, not under the Apache License; see
  the [`NOTICE` of ap2_core](https://github.com/Kristian8606/esp-airplay2-core/blob/v1.0.0/components/ap2_core/NOTICE).
- **Fraunhofer FDK AAC** (`speaker/components/fdk_aac/fdk-aac`) is under the
  Fraunhofer FDK license, [`NOTICE`](speaker/components/fdk_aac/fdk-aac/NOTICE). The
  copy here is a **Third-Party Modified Version of the Fraunhofer FDK AAC
  Codec Library for Android**; the changes and their dates are listed in
  [`MODIFICATIONS.md`](speaker/components/fdk_aac/MODIFICATIONS.md). The license
  allows redistribution in source and binary form without copyright fees but
  **grants no patent rights**: AAC patent licenses are available through Via
  Licensing or the patent owners.
- Other components (ESP-IDF, `esp_audio_codec`, libsodium, mDNS, cJSON,
  led_strip, `usb` / `usb_host_msc` for the USB player's drive, and
  `esp_hosted` for the ESP32-P4's Wi-Fi through its C6) come through the
  ESP-IDF component manager under their own licenses.

Related projects by the same author: the `standalone` branch of
[esp32-airplay2-surround](https://github.com/Kristian8606/esp32-airplay2-surround),
where every board is its own AirPlay receiver playing one channel, and
[airplay-esp32](https://github.com/Kristian8606/airplay-esp32), a stereo
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
