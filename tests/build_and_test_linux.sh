#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 jaka
# Builds the core tool + the Linux variant of the plugin and runs all tests.
# Needs: g++, dosfstools (mkfs.fat/fsck.fat), 7-Zip for Linux (7zip package),
# and the 7-Zip source tree (for Common/MyWindows.*):
#   tests/build_and_test_linux.sh /path/to/7zip-src/CPP
set -eu
SRC7Z=${1:?path to 7-Zip source CPP directory}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${2:-$ROOT/build}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -o "$OUT/fat12tool" "$ROOT/tests/fat12tool.cpp" "$ROOT/src/fat12.cpp"
g++ -std=c++17 -O2 -g -Wall -Wextra -Wno-non-virtual-dtor -fPIC -shared -fvisibility=hidden \
    -I"$SRC7Z" -o "$OUT/fat12.so" "$ROOT/src/plugin.cpp" "$ROOT/src/fat12.cpp" "$SRC7Z/Common/MyWindows.cpp"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DFUZZ_STANDALONE \
    -o "$OUT/fuzz_fat12" "$ROOT/tests/fuzz_fat12.cpp" "$ROOT/src/fat12.cpp"
# no ASan here: the unit test measures heap use with its own operator new
g++ -std=c++17 -O2 -g -Wall -Wextra -fsanitize=undefined -fno-sanitize-recover=all \
    -o "$OUT/unit_fat12" "$ROOT/tests/unit_fat12.cpp" "$ROOT/src/fat12.cpp"
# the File Manager's update path, against an ASan/UBSan build of the plugin
# (vptr is off: plugin and test declare the COM interfaces separately)
HSAN="-std=c++17 -O1 -g -Wall -Wextra -Wno-non-virtual-dtor -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=all"
g++ $HSAN -fPIC -shared -fvisibility=hidden -I"$SRC7Z" \
    -o "$OUT/fat12_asan.so" "$ROOT/src/plugin.cpp" "$ROOT/src/fat12.cpp" "$SRC7Z/Common/MyWindows.cpp"
g++ $HSAN -I"$SRC7Z" -o "$OUT/handler_fat12" "$ROOT/tests/handler_fat12.cpp" "$ROOT/src/fat12.cpp" \
    "$SRC7Z/Common/MyWindows.cpp" -ldl
# private copy of 7-Zip with the plugin in Formats/
H="$OUT/host7z"; rm -rf "$H"; mkdir -p "$H/Formats"
cp /usr/lib/7zip/7z /usr/lib/7zip/7z.so "$H/"; cp "$OUT/fat12.so" "$H/Formats/"
set +e
echo "== unit tests"
"$OUT/unit_fat12" "$ROOT/tests"; u=$?
echo "== handler tests (File Manager update path)"
"$OUT/handler_fat12" "$OUT/fat12_asan.so"; h=$?
"$ROOT/tests/run_core_tests.sh" "$OUT/fat12tool" "$OUT/coretest"; a=$?
"$ROOT/tests/run_plugin_tests.sh" "$H" "$OUT/plugtest"; b=$?
echo "== fuzz regression inputs + short fuzz run"
"$OUT/fuzz_fat12" "$ROOT/tests/fuzz_regress" &&
  "$OUT/fuzz_fat12" -runs=2000 -crash="$OUT/fuzz-crash.img" "$ROOT/tests/fuzz_regress"; c=$?
exit $((u | h | a | b | c))
