#!/bin/sh
# Builds the C6 firmware and puts it in prebuilt/ as airplay-gw-c6_<VERSION>.bin,
# the file the P4 Gateway build embeds. Run it after a change here (and a new
# VERSION), with ESP-IDF in the environment (get_idf), then commit prebuilt/.
# The target (esp32c6) comes from sdkconfig.defaults; reconfigure takes a
# changed VERSION into the build.
set -e
cd "$(dirname "$0")"
idf.py reconfigure build
version=$(tr -d ' \r\n' < VERSION)
rm -f prebuilt/airplay-gw-c6_*.bin
mkdir -p prebuilt
cp build/airplay-gw-c6.bin "prebuilt/airplay-gw-c6_${version}.bin"
echo "prebuilt/airplay-gw-c6_${version}.bin"
