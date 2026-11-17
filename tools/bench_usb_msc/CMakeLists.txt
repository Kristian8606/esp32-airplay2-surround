# USB player, phase 0: USB flash drive read speed on an ESP32-S3 (USB OTG,
# full speed). A test app of its own, not part of the Gateway firmware.
# See README.md here and docs/player-design.md.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bench_usb_msc)
