#!/bin/sh
# make_driver.sh <dir with the plist sources> <output>
# SANITIZE overrides the sanitizers (ASan hangs at start on some macOS).
set -e
HERE=$(dirname "$0"); SRC=$1; OUT=$2
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=${SANITIZE:-address,undefined} -fno-sanitize-recover=undefined \
  -I"$SRC" -I"$HERE/../../include" \
  "$HERE/driver.c" "$SRC"/plist_xml.c "$SRC"/bplist_parser.c "$SRC"/bplist_builder.c "$SRC"/base64.c -o "$OUT"
