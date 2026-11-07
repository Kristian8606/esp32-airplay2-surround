# led: the RGB status and level LED

## Purpose

Both firmwares (ESP32-S3, ESP-IDF v6.0.2) drive one WS2812 RGB LED: a
Gateway that receives AirPlay 2, and speakers that play one channel each.
The LED shows the AirPlay state (standby, connected or paused, playing)
and, while playing, a level meter of the audio the speaker sends to its
amplifier.

The interface is `deps/led.h`; the public header goes to `include/led.h`
with these two functions. Session events come from `deps/rtsp_events.h`
(the `helpers` component).

Sources: **[callers]** the code that calls these functions and emits the
events; **[menuconfig]** the firmware options below; **[ESP-IDF]** ESP-IDF
v6.0.2 and the Espressif `led_strip` component 3.0.3 (its public API);
**[board]** the LED of the running firmware. Rules marked "decided" were
chosen for this firmware where no source shows the behaviour.

## Placement

An ESP-IDF component `led` (Apache-2.0): sources in `src/`, `include/led.h`
its only public header. It needs `helpers` (`rtsp_events.h`), the managed
component `espressif__led_strip` (^3.0.3), `freertos`, `log`, `esp_timer`.
The rules that need no ESP-IDF (the state, the colours, the level meter)
are kept apart so that host tests on a computer can run them.

## Options [menuconfig]

All in `sdkconfig.h`; the same in both firmwares:

| Option | Values | Default |
|---|---|---|
| `CONFIG_ENABLE_RGB_AUDIO_LED` | y / n | y |
| `CONFIG_RGB_AUDIO_LED_GPIO` | 0–48, the WS2812 data pin | 38 |
| `CONFIG_RGB_AUDIO_LED_BRIGHTNESS` | 0–255, the brightest the LED gets | 96 |
| `CONFIG_RGB_AUDIO_LED_UPDATE_HZ` | 5–60, level meter updates per second | 30 |
| playing: one of `CONFIG_RGB_AUDIO_LED_PLAYING_VU`, `_PLAYING_STEADY`, `_PLAYING_OFF` | level meter, steady green, off | VU |
| paused: one of `CONFIG_RGB_AUDIO_LED_PAUSED_STEADY`, `_PAUSED_OFF` | steady blue, off | steady |
| standby: one of `CONFIG_RGB_AUDIO_LED_STANDBY_OFF`, `_STANDBY_STEADY` | off, steady dim green | off |

Without `CONFIG_ENABLE_RGB_AUDIO_LED` both functions do nothing: no task,
no LED driver, no work in the audio path. Only one option of each choice
is defined.

The options are defined by the firmware (its `airplay_options` component
holds the `Kconfig`); the `led` component ships no `Kconfig` (decided). A
check at build time (`#error`) that a choice has one option set and a
value is in range is fine, but only when `CONFIG_ENABLE_RGB_AUDIO_LED` is
set: without it the other options do not exist.

## Environment [callers]

- Gateway: `led_init()` once in start-up (also in the first-time setup
  mode); it has no audio output, so `led_audio_feed()` is never called
  there. Its RTSP server emits the events.
- Speaker: `led_init()` once in start-up; its link to the Gateway emits
  `RTSP_EVENT_CLIENT_CONNECTED` when the link is up,
  `RTSP_EVENT_DISCONNECTED` when it is lost, and `RTSP_EVENT_PLAYING` /
  `RTSP_EVENT_PAUSED` when the Gateway says so.
- `led_audio_feed()` is called by the speaker's audio output task (core 1,
  a high priority) after each I2S write that succeeded: 480 frames (10 ms
  at 48 kHz, 10.9 ms at 44.1 kHz) of two `int32_t` slots, left then
  right; the 16-bit sample is the top 16 bits of a slot (`slot >> 16`,
  arithmetic). It must return at once: no waiting on a lock another task
  may hold for long, no log, no LED driver call, no allocation. A short
  spinlock or atomics are fine (decided).
- Events arrive in the task that emits them (the RTSP or link task); the
  callback only records the state and wakes the LED task (decided).

## States

| Event | State |
|---|---|
| start | standby |
| `RTSP_EVENT_DISCONNECTED` | standby |
| `RTSP_EVENT_CLIENT_CONNECTED` | paused |
| `RTSP_EVENT_PAUSED` | paused |
| `RTSP_EVENT_PLAYING` | playing |

[board: a connected speaker shows the paused colour; start and a lost link
show standby.] The table holds in every state, also `CLIENT_CONNECTED`
while playing: on the Gateway it comes only at the start of a new session
(the old one has ended with `DISCONNECTED` first) [callers], and on the
speaker the Gateway sends `PLAYING` again when it plays (decided).
`PLAYING` while already playing changes nothing; playing starts again only
after another state, also when that state lasted shorter than one wake-up
of the task (decided). The LED shows a new state within 50 ms of the event
(decided).

## Colours [board]

`B` is `CONFIG_RGB_AUDIO_LED_BRIGHTNESS`. A steady colour is a base colour
scaled with integer division, `c * B / 255` for each component:

| Mode | Base (R, G, B) | With B = 96 | With B = 255 | With B = 10 |
|---|---|---|---|---|
| playing, steady green | 0, 96, 16 | 0, 36, 6 | 0, 96, 16 | 0, 3, 0 |
| paused, steady blue | 0, 0, 96 | 0, 0, 36 | 0, 0, 96 | 0, 0, 3 |
| standby, steady dim green | 0, 40, 0 | 0, 15, 0 | 0, 40, 0 | 0, 1, 0 |

"Off" is the LED cleared. With B = 0 the LED is always off.

### Level meter (playing with `_PLAYING_VU`, decided)

Every `1 / CONFIG_RGB_AUDIO_LED_UPDATE_HZ` s while the state is playing:

1. **Measure.** From the frames fed since the last update: the mean of the
   squares of all left and right 16-bit samples, `ms`, and the level
   `L = 10 * log10(ms / 32768^2)` dB (−∞ for silence). Frames fed in other
   states are not counted. An update with no frames fed since the last one
   (and less than 500 ms without frames, rule 4) is skipped: `S` and the
   colour stay (decided).
2. **Hold.** The shown level `S` rises at once and falls at most 1.5 dB
   per update: `S = max(L, S_prev - 1.5)`. `S` starts at −∞ when playing
   starts.
3. **Colour.** `t = clamp((S + 50) / 50, 0, 1)`. Value
   `v = round(B * t^2)`; hue `h = round(240 * (1 - t))` degrees (240 blue
   when quiet, 120 green, 0 red at full level); saturation 255. Set with
   `led_strip_set_pixel_hsv()` (hue 0–360). `v = 0` clears the LED.
   `round` is C `lround()` (halves away from zero).
4. **No audio.** When no frame has been fed for 500 ms (or none since
   playing started), the LED shows the paused mode's colour (steady blue,
   or off) instead, and `S` starts again at −∞. This is what the Gateway,
   which is never fed, shows while playing.

The LED is written (`led_strip_refresh()`) only when its colour changes
(decided).

## The LED driver [ESP-IDF, board]

- `led_strip_new_rmt_device()`: GPIO `CONFIG_RGB_AUDIO_LED_GPIO`, 1 LED,
  `LED_MODEL_WS2812`, colour order GRB (the WS2812 order), RMT default
  clock at 10 MHz, default memory, no DMA.
- On an error: one `ESP_LOGE` line, and both functions do nothing from
  then on. The same for a later error of `led_strip_set_pixel*()`,
  `led_strip_refresh()` or `led_strip_clear()`: the first one is logged,
  then the task sleeps for good (decided).
- `rtsp_events_register()` returning −1: one `ESP_LOGE` line; the LED
  keeps the standby colour and the rest goes on (decided). The LED itself
  is set up, so the start line follows the error line.
- After set-up the LED shows the standby colour at once.

## Task (decided)

One task, `led`: stack 4 KiB in PSRAM (`xTaskCreatePinnedToCoreWithCaps`,
`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`), priority 1, core 0 (away from the
audio output on core 1) [board: the same as now]. It wakes on an event,
on each level meter update while playing in VU mode, and otherwise sleeps
(no polling while standby or paused). A task that cannot be created: one
`ESP_LOGE` line, LED cleared, both functions do nothing.

## Log (decided)

Tag `led`. One line at start, only after the set-up worked (a failed one
logs only its error):
`WS2812 LED on GPIO <n>, brightness <B>, level meter <hz> Hz` (the last
part only with `_PLAYING_VU`). Errors as above. Nothing per event or
update.

## Examples

Steady modes, B = 96: start → off (`_STANDBY_OFF`) or 0, 15, 0
(`_STANDBY_STEADY`); `CLIENT_CONNECTED` → 0, 0, 36; `PLAYING` with
`_PLAYING_STEADY` → 0, 36, 6; `PAUSED` → 0, 0, 36 (or off with
`_PAUSED_OFF`); `DISCONNECTED` → standby again.

Level meter, B = 96, a constant 16-bit sample `x` in both channels (so
`L = 20 * log10(|x| / 32768)`), after the hold has settled:

| x | L (dB) | t | v | h |
|---|---|---|---|---|
| 32767 | −0.0003 | 1.0 | 96 | 0 |
| 16384 | −6.02 | 0.8796 | 74 | 29 |
| 3277 | −20.0 | 0.6 | 35 | 96 |
| 328 | −39.99 | 0.2002 | 4 | 192 |
| 33 | −59.94 | 0 | 0 (off) | — |
| 0 | −∞ | 0 | 0 (off) | — |

From x = 16384 to x = 328: the shown level falls −7.52, −9.02, −10.52 dB
… one step per update, and reaches −39.99 dB on the 23rd update.

## Tests on a computer

With fakes of the LED driver, the clock and the task wake-ups: the colour
table for B = 96, 255, 10 and 0; the states for each event in each mode;
the level meter examples, the hold, the 500 ms no-audio rule, frames fed
outside playing; feeds of 0 frames, of one frame, of many frames at
once, and from another thread at the same time as updates and events.
None may crash, read outside the frames given, or call the LED driver from
`led_audio_feed()`.

## Open points

None.
