#!/bin/sh
# make_driver.sh <dir with the hap sources> <output> [libs prefix]
# SANITIZE overrides the sanitizers (ASan hangs at start on some macOS).
set -e
HERE=$(dirname "$0"); SRC=$1; OUT=$2; P=${3:-/usr/local}
GEN=$(mktemp -d); trap 'rm -rf "$GEN"' EXIT
: > "$GEN/sdkconfig.h"
gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=${SANITIZE:-address,undefined} \
  -I"$GEN" -I"$HERE/stubs" -I"$SRC" -I"$HERE/../../include" -I"$P/include" \
  "$HERE/driver.c" "$SRC"/hap.c "$SRC"/hap_crypto.c "$SRC"/hap_pair_setup.c \
  "$SRC"/hap_pair_verify.c "$SRC"/srp.c "$SRC"/tlv8.c \
  -L"$P/lib" -lsodium -lmbedcrypto -o "$OUT"
