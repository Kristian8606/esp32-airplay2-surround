#!/bin/sh
# make_driver.sh <root with the sources> <output> [libsodium prefix]
set -e
HERE=$(dirname "$0"); S=$1; OUT=$2; P=${3:-/usr/local}
REPO="$HERE/../.."
GEN=$(mktemp -d); trap 'rm -rf "$GEN"' EXIT
printf '#define CONFIG_AIRPLAY_MODEL_ID "AudioAccessory6,1"\n#define CONFIG_AIRPLAY_MULTICHANNEL 1\n' > "$GEN/sdkconfig.h"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$GEN" -I"$HERE/stubs" -I"$HERE/../hap_host/stubs" -I"$REPO/common" -I"$REPO/common/audio" -I"$REPO/common/network" -I"$REPO/main" -I"$REPO/speaker/main/audio" -I"$P/include" \
  "$HERE/driver.c" "$S/common/network/socket_utils.c" "$S/common/rtsp_events.c" "$S/main/mdns_airplay.c" "$S/speaker/main/audio/audio_crypto.c" \
  -L"$P/lib" -lsodium -o "$OUT"
