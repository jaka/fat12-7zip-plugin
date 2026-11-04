#!/bin/bash
# Round-trip tests for the FAT12 core.  Verifies images we write with
# fsck.fat (dosfstools) and 7-Zip's built-in FAT reader, and reads images
# made by mkfs.fat.   Usage: tests/run_core_tests.sh <fat12tool> <workdir>
set -u
TOOL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
rm -rf "$2"; mkdir -p "$2"; W=$(cd "$2" && pwd)
fail=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fail=1; }

# ---- source tree with awkward names
S=$W/src; mkdir -p "$S"
echo "hello" > "$S/README.TXT"
echo "lower" > "$S/lower.txt"
echo "mixed" > "$S/MixedCase.Txt"
echo "long"  > "$S/A long file name with spaces.text"
printf 'x' > "$S/čšž-ünïcode.txt"
: > "$S/empty.dat"
mkdir -p "$S/dir one/sub dir" "$S/emptydir" "$S/many"
head -c 100000 /dev/urandom > "$S/dir one/sub dir/random.bin"
head -c 512 /dev/urandom > "$S/dir one/exact512.bin"
for i in $(seq 1 60); do echo "file $i" > "$S/many/longfilename_number_$i.txt"; done
for n in .hidden "trailing dot." "multi.dot.name.tar.gz"; do echo "$n" > "$S/$n"; done
# Windows strips trailing dots: expected name differs
mkdir -p "$W/expect"; cp -a "$S/." "$W/expect/"; mv "$W/expect/trailing dot." "$W/expect/trailing dot"

for size in 1440 720 1200 2880 360 160 1680; do
  img=$W/t$size.img
  if [ $size = 160 ] || [ $size = 360 ] || [ $size = 1680 ]; then
    # too small for everything: only small files
    S2=$W/small$size; rm -rf "$S2"; mkdir -p "$S2/sub"; echo hi > "$S2/Hello World.txt"; echo x > "$S2/sub/a.b"
    $TOOL create "$img" $size "$S2" SMALL || { bad "$size create"; continue; }
    exp=$S2
  else
    $TOOL create "$img" $size "$S" "TESTDISK" || { bad "$size create"; continue; }
    exp=$W/expect
  fi
  [ "$(stat -c %s "$img")" = $((size*1024)) ] && ok "$size: image size" || bad "$size: image size"
  fsck.fat -n -v "$img" > "$W/fsck$size.txt" 2>&1 && ! grep -qiE "differ|invalid|bad|orphan|free cluster summary" "$W/fsck$size.txt" \
    && ok "$size: fsck.fat clean" || { bad "$size: fsck.fat"; tail -20 "$W/fsck$size.txt"; }
  rm -rf "$W/x7"; 7z x -o"$W/x7" "$img" > "$W/7z$size.txt" 2>&1 && ok "$size: 7-Zip built-in FAT extracts" || { bad "$size: 7z x"; tail "$W/7z$size.txt"; }
  diff -r "$exp" "$W/x7" > /dev/null && ok "$size: 7-Zip output matches source" || { bad "$size: 7z diff"; diff -r "$exp" "$W/x7" | head; }
  rm -rf "$W/xo"; $TOOL extract "$img" "$W/xo" && diff -r "$exp" "$W/xo" > /dev/null && ok "$size: own reader round-trip" || { bad "$size: own reader"; diff -r "$exp" "$W/xo" | head; }
done

# label + timestamps visible to 7-Zip
7z l -slt "$W/t1440.img" > "$W/slt.txt"
grep -q "Path = A long file name with spaces.text" "$W/slt.txt" && ok "LFN visible in 7z" || bad "LFN in 7z"
mt=$(date -r "$S/README.TXT" +"%Y-%m-%d %H:%M"); grep -A12 "Path = README.TXT" "$W/slt.txt" | grep -q "Modified = $mt" && ok "mtime preserved" || { bad "mtime"; grep -A12 "Path = README.TXT" "$W/slt.txt"; }
$TOOL list "$W/t1440.img" | head -1 | grep -q "label=TESTDISK" && ok "volume label" || bad "label"
$TOOL list "$W/t1440.img" | grep -q "LOWER.TXT | lower.txt" && ok "lowercase 8.3 via NT case flags" || bad "case flags"
$TOOL list "$W/t1440.img" | grep -q "LONGFI~1.TXT" && ok "numeric-tail short names" || bad "short names"

# disk full / root full errors
head -c 2000000 /dev/urandom > "$W/big.bin"; mkdir -p "$W/bigsrc"; cp "$W/big.bin" "$W/bigsrc/"
$TOOL create "$W/full.img" 1440 "$W/bigsrc" 2>&1 | grep -q "disk full" && ok "disk full detected" || bad "disk full"
mkdir -p "$W/rootfull"; for i in $(seq 1 230); do : > "$W/rootfull/F$i"; done
$TOOL create "$W/rf.img" 1440 "$W/rootfull" 2>&1 | grep -q "root directory full" && ok "root full detected" || bad "root full"

# images from mkfs.fat
for size in 1440 720 2880; do
  mkfs.fat -C -n MKFSDISK "$W/m$size.img" $size > /dev/null 2>&1
  out=$($TOOL list "$W/m$size.img") && echo "$out" | grep -q "label=MKFSDISK" && ok "reads mkfs.fat $size image" || bad "mkfs.fat $size: $out"
done
mkfs.fat -C -F 16 "$W/f16.img" 20000 > /dev/null 2>&1
$TOOL list "$W/f16.img" 2>&1 | grep -q "not FAT12" && ok "rejects FAT16" || bad "FAT16 not rejected"

# corrupted image must not crash
cp "$W/t1440.img" "$W/corrupt.img"
python3 -c "
import sys,random
p=sys.argv[1]; b=bytearray(open(p,'rb').read()); random.seed(1)
for i in range(3000): b[random.randrange(512,33*512)]=random.randrange(256)
open(p,'wb').write(b)" "$W/corrupt.img"
$TOOL list "$W/corrupt.img" > /dev/null 2>&1; rc=$?; [ $rc -le 1 ] && ok "corrupt image handled (rc=$rc)" || bad "corrupt image crash rc=$rc"
$TOOL extract "$W/corrupt.img" "$W/xc" > /dev/null 2>&1; rc=$?; [ $rc -le 1 ] && ok "corrupt extract handled (rc=$rc)" || bad "corrupt extract crash rc=$rc"

[ $fail = 0 ] && echo "ALL CORE TESTS PASSED" || echo "SOME CORE TESTS FAILED"
exit $fail
