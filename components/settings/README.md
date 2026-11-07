# settings

The saved settings of both firmwares in NVS (namespace `airplay`): the
device name, up to 8 Wi-Fi networks, the AirPlay volume and the speaker's
audio settings. An ESP-IDF component under the **Apache License 2.0**
([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)).

The keys and the 796-byte network list keep the layout of earlier firmware,
so an update keeps what a board has saved, and a rollback still reads it.

- `include/settings.h`: the functions the firmwares call
- `src/settings_core.c`: the network list layout, the default name and the
  volume rounding, without ESP-IDF
- `src/settings.c`: NVS, the MAC and the lock
- `tests/`: host tests against a fake NVS (`make -C tests`)

Behaviour: [`docs/settings-spec.md`](../../docs/settings-spec.md).
