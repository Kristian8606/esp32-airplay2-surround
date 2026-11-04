#!/bin/sh
# make_driver.sh <dir with the plist sources> <output>
set -e
HERE=$(dirname "$0"); SRC=$1; OUT=$2
GEN=$(mktemp -d); trap 'rm -rf "$GEN"' EXIT
printf '#define CONFIG_AIRPLAY_MODEL_ID "AudioAccessory6,1"\n#define CONFIG_AIRPLAY_MULTICHANNEL 1\n' > "$GEN/sdkconfig.h"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$GEN" -I"$HERE/../hap_host/stubs" -I"$SRC" -I"$HERE/../../common" -I"$HERE/../../common/audio" \
  "$HERE/driver.c" "$SRC"/plist_xml.c "$SRC"/bplist_parser.c "$SRC"/bplist_builder.c "$SRC"/base64.c "$HERE/../../main/airplay_identity.c" -I"${SODIUM:-/usr/local}/include" -L"${SODIUM:-/usr/local}/lib" -lsodium -o "$OUT"
