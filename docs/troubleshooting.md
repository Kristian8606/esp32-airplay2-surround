# Limitations and troubleshooting

[← Back to the README](../README.md)

## Limitations

- **Experimental.** Tested with one Apple TV and an iPhone, a Gateway and
  five speakers playing together: 5.1, 7.1 and stereo AAC, skip, seek,
  pause, volume, role and level changes while playing, a second sender
  taking over, web radio.
- **Room calibration** is tested with an INMP441 and up to six speakers.
  An echo more than 12 dB louder than the direct sound (a speaker behind an
  obstacle) still misleads its timing, and the subwoofer's level is left
  to the ear. Its delay includes its own crossover's delay and the
  microphone's low cut (about 1 ms in the simulation), so check it by ear.
- **Realtime** (screen mirroring, system audio from a Mac) through the
  Gateway works, also in the REW measuring mode.
- **Not yet tested:** the stereo routing to front pair and subwoofer,
  oscilloscope measurements, more than five speakers, a weak Wi-Fi signal.
- **Late joins:** a speaker that connects during a film waits for the next
  skip or seek; until then it stays silent.
- **One access point.** All boards must be associated with the same access
  point (on a mesh, the same node).
- **Playout servo:** each speaker holds its own playout within ±0.4 ms; two
  speakers differ by 0.1–0.3 ms. That is inaudible as an echo but could be
  tightened for a perfectly fixed stereo centre.
- **No bass management**: with 5.1 / 7.1 the Subwoofer plays only LFE.
- The **7.1 Front / Side naming** is not yet confirmed (see
  [Channels](technical.md#channels)).
- **Mono mix** decodes all channels and may not keep up with 7.1.
- A switch between 44.1 and 48 kHz has a short silence while I2S is
  reconfigured.
- **Rear and side channels are often nearly silent in music**; they come
  alive with films and Atmos mixes.
- **USB player** (experimental): on an ESP32-S3 Gateway a file heavier
  than about 5 Mbit/s (WAV 5.1 / 7.1) stops to buffer every few seconds,
  and so does a dense 7.1 film mix at 24-bit (CPU; 16-bit plays); the
  ESP32-P4 is not yet measured with such files. FAT32 only, one drive, hubs
  not yet tested; no shuffle or repeat, and the queue is not kept over a
  restart.
- **ESP32-P4 Gateway**: tested on the FireBeetle 2 with the same speakers;
  another P4 board needs its pins and the C6 wiring set in `menuconfig`
  ([bring-up](p4-gateway-bringup.md)).
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
| The USB stick is not seen | The Library says *no drive*: the native USB port gets no 5 V (on many DevKitC boards a solder bridge, see [the USB stick](usb-player.md#usb-player-experimental)). *Not FAT32*: format it FAT32 with a Master Boot Record. |
| A song from the stick stops every few seconds | `buffering: The file comes too slowly` in the Gateway log: on an ESP32-S3 a WAV 5.1 / 7.1 or a 7.1 at 24-bit is too heavy; convert it to FLAC (16-bit for 7.1), or use an ESP32-P4 Gateway. |
| No log or no flashing over the Gateway's USB port | An ESP32-S3 Gateway's console is on its **UART** port; the native USB port is the USB player's. The ESP32-P4's console is on its USB-C. |

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
| `gw_usb: drive mounted at /usb: 8.1 GB, …` | Gateway | the USB stick is in and read |
| `gw_player: track 2 of 5: usb:/Music/…` / `gapless on into track 3` | Gateway | the player's queue |
| `PLAYER playing … (1.18x real time) \| read … decode … encode … send …` | Gateway | the player keeps up when it runs above 1x; where the time goes |
| `PCM CRC-32 of 10 s from 0.000 s …` | Gateway | a checksum of the decoded samples, to compare with the file (bit for bit) |
| `PLAYING buffered ALAC … one channel (player lane, both slots)` | speaker | the speaker plays its own channel from the Gateway's player |

For deeper debugging enable **AirPlay Logs and Diagnostics → Log every RTSP
request and response** in `menuconfig` on the Gateway (verbose; turn it off
afterwards).
