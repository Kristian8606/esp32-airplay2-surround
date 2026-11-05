#!/bin/sh
# make_driver.sh <output>: builds the FairPlay host test.
# SANITIZE overrides the sanitizers (ASan hangs at start on some macOS).
set -e
HERE=$(dirname "$0"); OUT=$1
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=${SANITIZE:-address,undefined} -fno-sanitize-recover=undefined \
  -I"$HERE/../../include" -I"$HERE/../../src/fairplay" \
  "$HERE/test_fairplay.c" "$HERE/../../src/fairplay/fairplay.c" "$HERE/../../src/fairplay/fairplay_tables.c" -o "$OUT"
