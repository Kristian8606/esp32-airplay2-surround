# led

The one WS2812 RGB LED of both firmwares: the AirPlay state (standby,
connected or paused, playing) and, while a speaker plays, a level meter of
the audio it sends to I2S. An ESP-IDF component under the **Apache License
2.0** ([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)).

- `include/led.h`: `led_init()` once at start-up, `led_audio_feed()` from
  the audio output
- `src/led_core.c`: states, colours and the level meter, without ESP-IDF
- `src/led.c`: the task, the events and the `led_strip` driver

The options are in menuconfig, "RGB Audio/VU LED" (`airplay_options`).
Behaviour: [`docs/led-spec.md`](../../docs/led-spec.md).
