# bench_usb_msc — USB read speed (phase 0 of ROADMAP_USB_PLAYER.md)

A small test app for **any spare ESP32-S3 board** (N8R8 or N16R8). It is not
part of the Gateway firmware and changes nothing there.

It mounts a USB flash drive (FAT32) on the S3's native USB port as USB host,
lists the root folder, then reads the largest file in the root folder:

- sequentially with `fread` buffers of 4, 16, 64 and 256 KiB (up to 8 MiB each);
- 200 random 4 KiB reads (seek + read), the cost of a seek.

All results are `BENCH` log lines on UART0.

## Wiring

- The native USB port (GPIO19/20) gets the flash drive. It needs 5 V on VBUS:
  a powered USB hub between the board and the drive is the simplest way (it
  also tests the hub support); otherwise the board must feed its 5 V to that
  port (on many DevKitC boards a solder bridge, see the board's schematic).
- Flash and logs over the board's UART port.

## Run

```
cd tools/bench_usb_msc
idf.py set-target esp32s3
idf.py -p <UART port> flash monitor
```
