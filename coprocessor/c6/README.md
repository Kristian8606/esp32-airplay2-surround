# C6 firmware of the P4 Gateway

Firmware of the ESP32-C6 next to an ESP32-P4 Gateway (FireBeetle 2 ESP32-P4):
the Wi-Fi radio, as the standard [ESP-Hosted](https://github.com/espressif/esp-hosted-mcu)
co-processor over SDIO. Project name `airplay-gw-c6`, version in `VERSION`.

On top of that it keeps the time for the Gateway: a pulse on the WAKEUP line
every 125 ms, the AP's TSF at each edge and receive timestamps of PTP packets,
all sent to the P4 over ESP-Hosted peer data (format in
`common/gw_c6/gw_c6_proto.h`).

```bash
idf.py set-target esp32c6      # first time only
idf.py build
```

The partition table is the one of the factory image of the board's C6, so
the P4 can update the app over the air (an OTA never changes the table or
the bootloader).
