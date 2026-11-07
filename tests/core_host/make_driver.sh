#!/bin/sh
# make_driver.sh <root with the sources> <output> [libsodium prefix]
# SANITIZE overrides the sanitizers (ASan hangs at start on some macOS).
set -e
HERE=$(dirname "$0"); S=$1; OUT=$2; P=${3:-/usr/local}
REPO="$HERE/../.."
GEN=$(mktemp -d); trap 'rm -rf "$GEN"' EXIT
printf '#define CONFIG_AIRPLAY_MODEL_ID "AudioAccessory6,1"\n#define CONFIG_AIRPLAY_MULTICHANNEL 1\n' > "$GEN/sdkconfig.h"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=${SANITIZE:-address,undefined} -fno-sanitize-recover=undefined \
  -I"$GEN" -I"$HERE/stubs" -I"$REPO/components/ap2_core/tests/hap_host/stubs" -I"$REPO/common" -I"$REPO/common/audio" -I"$REPO/common/network" -I"$REPO/components/wifi/include" -I"$REPO/components/helpers/include" -I"$REPO/main" -I"$REPO/components/ap2_core/include" -I"$P/include" \
  "$HERE/driver.c" "$S/components/helpers/src/socket_utils.c" "$S/components/helpers/src/rtsp_events.c" "$S/main/mdns_airplay.c" "$S/components/ap2_core/src/audio_crypto.c" \
  -L"$P/lib" -lsodium -o "$OUT"
