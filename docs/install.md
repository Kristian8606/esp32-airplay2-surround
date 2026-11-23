# Installation

[← Back to the README](../README.md)

## 1. Install ESP-IDF

The firmware is built with **ESP-IDF v6.1**. Follow Espressif's
[installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html),
then open a terminal with the ESP-IDF environment loaded
(`source ~/esp-idf/export.sh`, or the path where you installed it).

## 2. Get the code

The Gateway is the **`main` branch** of the surround repository (the
earlier design, where every board is its own AirPlay receiver, is the
`standalone` branch):

```bash
git clone https://github.com/Kristian8606/esp32-airplay2-surround.git
cd esp32-airplay2-surround
```

## 3. Two projects, one code

| Folder | Builds | Firmware file |
|---|---|---|
| the repository folder | the **Gateway** (ESP32-S3 or ESP32-P4) | `build/airplay-gateway_<version>.bin` |
| `speaker/` | the **speakers** | `speaker/build/airplay-speaker_<version>.bin` |

| Code | Where |
|---|---|
| the Gateway's own (proxy, relays, speaker registry, speakers page); its AirPlay receiver (mDNS, pairing, RTSP) is `ap2_core` from [esp-airplay2-core](https://github.com/Kristian8606/esp-airplay2-core) | `main/` |
| the speaker's own (audio engine, link to the Gateway, EQ page) and FDK AAC | `speaker/main/`, `speaker/components/` |
| shared (web server, clock, sync protocol) | `common/`, `web/`, and the components in `components/` |
| menuconfig options of both | `components/airplay_options/` |

Both use the shared settings in `sdkconfig.defaults`; the Gateway adds
`sdkconfig.defaults.gateway`, and ESP-IDF adds `sdkconfig.defaults.esp32s3`
or `sdkconfig.defaults.esp32p4` for the chip. Each is an ordinary ESP-IDF
project: go into its folder and run `idf.py` as usual. The speaker is its
own project with its own `build/` and settings: it is always built for the
ESP32-S3, whatever chip the Gateway was set to. No `set-target` is needed.
The ESP32-C6
firmware of the P4 board is in `coprocessor/c6/`; the Gateway carries a
built copy and installs it on the C6 by itself.

## 4. Configure (optional)

Run `idf.py menuconfig` in the folder of the firmware you configure (the
repository folder for the Gateway, `speaker/` for the speakers). Both show
the same menus; some options matter for one firmware only:

| Menu | What you can set | For |
|---|---|---|
| **Surround Build Role** | the role (set by the folder: leave it); the Gateway board (it sets the default pins); a fixed Gateway address for networks that filter mDNS; the sync test pulse; the calibration microphone (INMP441 or PDM) and its pins | both |
| **AirPlay Receiver** | setup Wi-Fi name and password; HomeKit, setup code, manufacturer, artwork; playback buttons (for AirPlay and the USB player) and AirPlay model (the icon senders show) | Wi-Fi both, the rest Gateway |
| **Multichannel Experiment** | 5.1 / 7.1 on or off, default speaker position | both |
| **I2S Playout** | BCLK / LRCK / DATA / MCLK GPIOs, output latency of your DAC, wired latency test; web admin password | speakers (password both) |
| **Amplifier Power Control** | amplifier enable GPIO and level | speakers |
| **RGB Audio/VU LED** | LED on/off and GPIO | both |
| **USB Player (experimental)** | player streams (speakers), the Gateway's player and its USB drive; all on by default | both |
| **AirPlay Logs and Diagnostics** | log detail | both |

Every speaker runs the same firmware: its role is set later on the Gateway's
page.

### 16 MB flash

By default both firmwares are built for **8 MB** flash (`partitions.csv`).
A board with **16 MB** (N16R8) uses all of it with `partitions_16mb.csv`, two settings next to each other in menuconfig (ESP-IDF
does not tie the partition table to the flash size):

1. `idf.py menuconfig` → **Serial flasher config → Flash size** → **16 MB**.
2. **Partition Table → Custom partition CSV file** →
   `partitions_16mb.csv` (in `speaker/`: `../partitions_16mb.csv`).
3. `idf.py build flash` **over a cable, once** (the Gateway over its UART
   port): a partition table reaches the
   board only so, not with an update over Wi-Fi. Afterwards updates work
   over Wi-Fi as before.

| Partition | 8 MB | 16 MB |
|---|---|---|
| nvs (settings, Wi-Fi, HomeKit), otadata, phy | as is | the same place and size, so the settings stay |
| ota_0, ota_1 (the firmware, twice for updates) | 3 MB each | the same |
| storage (the speaker firmware the Gateway hands out) | 1.9 MB | 3 MB, as large as a speaker's own slot |
| userdata (for later features) | — | 6.9 MB |

The 16 MB table does not fit an 8 MB flash: ESP-IDF stops the build. A
16 MB flash with the 8 MB table builds and runs, with a warning that the
rest is unused. The Gateway matters most (it keeps the speaker firmware);
speakers can stay on the 8 MB table, their firmware fits their slot.

### Boards

The chip is chosen the usual ESP-IDF way, with `idf.py set-target`; the
flash size in menuconfig.

| Board | Chip | Flash |
|---|---|---|
| ESP32-S3 N8R8 (8 MB flash, 8 MB PSRAM), the smallest supported | `esp32s3`, the default | the default |
| ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM) | `esp32s3` | [16 MB flash](#16-mb-flash) |
| DFRobot FireBeetle 2 ESP32-P4 with its ESP32-C6, Gateway only | `esp32p4` | set by the target: 16 MB, `partitions_esp32p4.csv` |

```bash
idf.py set-target esp32p4
idf.py -p /dev/cu.usbserial-0001 build flash monitor
```

`set-target` applies `sdkconfig.defaults.<chip>` (for the P4: Wi-Fi through
the C6, the board's pins, an 8 MiB relay buffer; see
[bring-up](p4-gateway-bringup.md)). It starts the settings again from
the defaults: what you changed in menuconfig is kept only in `sdkconfig.old`.
Either chip builds into the same `build/` folder.

## 5. Build and flash

Connect a speaker over its USB port, the Gateway over its UART port.

**Speakers** (repeat for every speaker):

```bash
cd speaker
idf.py -p /dev/cu.usbmodem101 build flash monitor
```

**Gateway**, from the repository folder, over its **UART** port (the native
USB port is the [USB player](usb-player.md#usb-player-experimental)'s drive port):

```bash
idf.py -p /dev/cu.usbserial-0001 build flash monitor
```

For an **ESP32-P4** Gateway run `idf.py set-target esp32p4` once first
([Boards](#boards)); it is flashed and logs over its USB-C port.

On macOS list the ports with `ls /dev/cu.*` (a speaker's USB port is a
`usbmodem`, a UART port a `usbserial` or `wchusbserial`); on Windows the port
is `COMx`, on Linux `/dev/ttyACM0` (USB) or `/dev/ttyUSB0` (UART). A
speaker's log goes to its USB port, the Gateway's to its UART port, so
`idf.py monitor` shows it, and every board's to its `/logs` page.

## 6. Updates

Later versions are installed **over Wi-Fi**:

- **Gateway:** on its main page, **Firmware Update** → choose
  `build/airplay-gateway_<version>.bin` → **Upload Firmware**.
- **Speakers, all at once:** on the Gateway's **speakers page**, panel
  **Speaker firmware** → **Choose .bin** →
  `speaker/build/airplay-speaker_<version>.bin` → **Upload**. The Gateway
  keeps it in its otherwise unused `storage` partition (1.9 MB, 3 MB with
  [16 MB flash](#16-mb-flash), 3.9 MB on the ESP32-P4) and every
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
