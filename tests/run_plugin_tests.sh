#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 jaka
# End-to-end tests: drive the real 7-Zip (Linux build) with the plugin in
# its Formats/ folder.   Usage: tests/run_plugin_tests.sh <dir-with-7z-and-Formats> <workdir>
set -u
Z="$(cd "$1" && pwd)/7z"
rm -rf "$2"; mkdir -p "$2"; W=$(cd "$2" && pwd)   # absolute: tests cd around
fail=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fail=1; }

S=$W/src; mkdir -p "$S/Sub Dir/deeper" "$S/emptydir"
echo hello > "$S/README.TXT"
echo "long name" > "$S/A Long File Name.txt"
printf 'unicode' > "$S/Žabica čšž.txt"
head -c 150000 /dev/urandom > "$S/Sub Dir/deeper/blob.bin"
echo lower > "$S/Sub Dir/lower.txt"
touch -d "2001-02-03 04:05:06" "$S/README.TXT"

# --- create with size + label
( cd "$S" && "$Z" a -tf12 -msize=720k -mlabel=mydisk "$W/a.ima" > "$W/create.log" 2>&1 ) && ok "create 720k" || { bad "create"; cat "$W/create.log"; }
[ "$(stat -c %s "$W/a.ima")" = 737280 ] && ok "image is 720 KiB" || bad "size $(stat -c %s "$W/a.ima")"
fsck.fat -n "$W/a.ima" > "$W/fsck.log" 2>&1 && ok "fsck.fat clean" || { bad "fsck"; cat "$W/fsck.log"; }

"$Z" l -slt "$W/a.ima" > "$W/l.txt" 2>&1
grep -q "^Type = f12" "$W/l.txt" && ok "opened by f12 plugin (.ima)" || { bad "type"; head -20 "$W/l.txt"; }
grep -q "Label = MYDISK" "$W/l.txt" && ok "label upper-cased + listed" || bad "label"
grep -q "Path = Žabica čšž.txt" "$W/l.txt" && ok "unicode LFN" || bad "unicode"
grep -A12 "Path = README.TXT" "$W/l.txt" | grep -q "Modified = 2001-02-03 04:05:06" && ok "mtime round-trip" || { bad "mtime"; grep -A12 "Path = README.TXT" "$W/l.txt"; }

rm -rf "$W/x"; "$Z" x -o"$W/x" "$W/a.ima" > /dev/null 2>&1 && diff -r "$S" "$W/x" && ok "extract via plugin == source" || bad "extract diff"
"$Z" t "$W/a.ima" 2>&1 | grep -q "Everything is Ok" && ok "test (7z t)" || bad "test"
rm -rf "$W/xb"; "$Z" x -tFAT -o"$W/xb" "$W/a.ima" > /dev/null 2>&1 && diff -r "$S" "$W/xb" && ok "built-in FAT reader agrees" || bad "builtin diff"

# --- update: add a file, existing content and size kept
echo extra > "$W/extra.txt"
"$Z" a "$W/a.ima" "$W/extra.txt" > "$W/upd.log" 2>&1 && ok "add to existing image" || { bad "update"; cat "$W/upd.log"; }
[ "$(stat -c %s "$W/a.ima")" = 737280 ] && ok "update keeps 720 KiB size" || bad "size after update"
cp "$W/extra.txt" "$S/"
rm -rf "$W/x"; "$Z" x -o"$W/x" "$W/a.ima" > /dev/null 2>&1 && diff -r "$S" "$W/x" && ok "content after update" || bad "content after update"
"$Z" l -slt "$W/a.ima" | grep -q "Label = MYDISK" && ok "label kept on update" || bad "label lost"
fsck.fat -n "$W/a.ima" > /dev/null 2>&1 && ok "fsck after update" || bad "fsck after update"

# --- delete + rename
"$Z" d "$W/a.ima" README.TXT > /dev/null 2>&1 && ! "$Z" l "$W/a.ima" | grep -q " README.TXT" && ok "delete" || bad "delete"
"$Z" rn "$W/a.ima" "A Long File Name.txt" "renamed file.txt" > /dev/null 2>&1 && "$Z" l "$W/a.ima" | grep -q "renamed file.txt" && ok "rename" || bad "rename"
fsck.fat -n "$W/a.ima" > /dev/null 2>&1 && ok "fsck after delete/rename" || bad "fsck after d/rn"

# --- resize existing image
"$Z" a -msize=1.44m "$W/a.ima" "$W/extra.txt" > /dev/null 2>&1 && [ "$(stat -c %s "$W/a.ima")" = 1474560 ] && ok "resize 720k -> 1.44m" || bad "resize"

# --- size spellings / default
for spec in "1,44" "1.44" "1.2m" "1200" "360k" "2.88" "dmf"; do
  rm -f "$W/s.ima"
  ( cd "$S" && "$Z" a -tf12 "-msize=$spec" "$W/s.ima" README.TXT > /dev/null 2>&1 ) && fsck.fat -n "$W/s.ima" > /dev/null 2>&1 \
    && ok "size=$spec -> $(( $(stat -c %s "$W/s.ima") / 1024 )) KiB" || bad "size=$spec"
done
rm -f "$W/d.img"; ( cd "$S" && "$Z" a -tf12 "$W/d.img" README.TXT > /dev/null 2>&1 ) && [ "$(stat -c %s "$W/d.img")" = 1474560 ] && ok "default size 1.44 MB" || bad "default size"
"$Z" a -tf12 -msize=bogus "$W/e.ima" "$W/extra.txt" > "$W/e.log" 2>&1; grep -q "cannot parse size 'bogus'" "$W/e.log" && ok "bad size rejected with message" || { bad "bad size"; cat "$W/e.log"; }

# --- .img: built-in FAT handler opens it first; -tf12 updates it
"$Z" a -tf12 "$W/d.img" "$W/extra.txt" > /dev/null 2>&1 && "$Z" l "$W/d.img" | grep -q extra.txt && ok ".img update with -tf12" || bad ".img update"

# --- errors
head -c 800000 /dev/urandom > "$W/big.bin"
"$Z" a -tf12 -msize=720k "$W/full.ima" "$W/big.bin" > "$W/full.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q "does not fit\|disk full" "$W/full.log" && ok "disk full reported (rc=$rc)" || { bad "disk full"; cat "$W/full.log"; }

# --- images from mkfs.fat open in the plugin
mkfs.fat -C -n FROMMKFS "$W/m.ima" 1440 > /dev/null 2>&1
"$Z" a "$W/m.ima" "$W/extra.txt" > /dev/null 2>&1 && "$Z" l -slt "$W/m.ima" | grep -q "Label = FROMMKFS" && fsck.fat -n "$W/m.ima" > /dev/null 2>&1 \
  && ok "update mkfs.fat image keeps label" || bad "mkfs.fat update"

# --- update of a damaged image is refused and leaves the image untouched
mkdir -p "$W/dmg"; head -c 5000 /dev/urandom > "$W/dmg/big.bin"
"$Z" a -tf12 "$W/dmg/d.ima" "$W/dmg/big.bin" > /dev/null 2>&1 || bad "damaged: create"
# big.bin starts at cluster 2: mark that cluster free in both FATs (chain broken)
python3 -c "
import sys; p=sys.argv[1]; d=bytearray(open(p,'rb').read())
for fat in (512, 512 + 9*512): d[fat+3] = 0; d[fat+4] &= 0xF0
open(p,'wb').write(d)" "$W/dmg/d.ima"
before=$(md5sum < "$W/dmg/d.ima")
"$Z" a "$W/dmg/d.ima" "$W/extra.txt" > "$W/dmg.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q "damaged file in the image: big.bin" "$W/dmg.log" && ok "update of damaged image refused (rc=$rc)" || { bad "damaged update not refused"; cat "$W/dmg.log"; }
[ "$(md5sum < "$W/dmg/d.ima")" = "$before" ] && ok "damaged image left unchanged" || bad "damaged image was modified"
[ "$(ls "$W/dmg")" = "$(printf 'big.bin\nd.ima')" ] && ok "no temporary files left behind" || bad "leftovers: $(ls "$W/dmg")"
"$Z" d "$W/dmg/d.ima" big.bin > /dev/null 2>&1 && "$Z" a "$W/dmg/d.ima" "$W/extra.txt" > /dev/null 2>&1 && fsck.fat -n "$W/dmg/d.ima" > /dev/null 2>&1 \
  && ok "deleting the damaged file makes updates work again" || bad "recovery after delete"

# --- update of an image whose folders are damaged is refused (hidden files would be lost)
mkdir -p "$W/inc/src/sub"; echo keep > "$W/inc/src/keep.txt"; echo hidden > "$W/inc/src/sub/hidden.txt"
( cd "$W/inc/src" && "$Z" a -tf12 "$W/inc/i.ima" keep.txt sub > /dev/null 2>&1 ) || bad "incomplete: create"
# point the root entry of folder SUB at cluster 0: its contents can no longer be listed
python3 -c "
import sys, struct; p=sys.argv[1]; d=bytearray(open(p,'rb').read())
bps, = struct.unpack_from('<H', d, 11); res, = struct.unpack_from('<H', d, 14)
nf = d[16]; spf, = struct.unpack_from('<H', d, 22); root = (res + nf*spf) * bps
for o in range(root, root + 224*32, 32):
    if d[o:o+11] == b'SUB        ': d[o+26:o+28] = b'\\0\\0'
open(p,'wb').write(d)" "$W/inc/i.ima"
"$Z" l "$W/inc/i.ima" > "$W/inc.log" 2>&1
grep -q "not all files are listed" "$W/inc.log" && ok "listing warns that files are missing" || { bad "no incomplete warning"; cat "$W/inc.log"; }
before=$(md5sum < "$W/inc/i.ima")
"$Z" a "$W/inc/i.ima" "$W/extra.txt" > "$W/inc.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q "not all files are listed" "$W/inc.log" && ok "add to incompletely listed image refused (rc=$rc)" || { bad "incomplete add not refused"; cat "$W/inc.log"; }
"$Z" d "$W/inc/i.ima" keep.txt > "$W/inc.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q "not all files are listed" "$W/inc.log" && ok "delete from incompletely listed image refused (rc=$rc)" || { bad "incomplete delete not refused"; cat "$W/inc.log"; }
[ "$(md5sum < "$W/inc/i.ima")" = "$before" ] && ok "incompletely listed image left unchanged" || bad "incompletely listed image was modified"
[ "$(ls "$W/inc")" = "$(printf 'i.ima\nsrc')" ] && ok "no temporary files left behind" || bad "leftovers: $(ls "$W/inc")"
rm -rf "$W/inc/x"; "$Z" x -o"$W/inc/x" "$W/inc/i.ima" > /dev/null 2>&1; cmp -s "$W/inc/x/keep.txt" "$W/inc/src/keep.txt" && ok "listed files still extract" || bad "extract from incompletely listed image"

# --- an image with its unused end cut off is listed completely and can be updated
( cd "$S" && "$Z" a -tf12 "$W/inc/t.ima" README.TXT > /dev/null 2>&1 ) && truncate -s 40960 "$W/inc/t.ima"
"$Z" a "$W/inc/t.ima" "$W/extra.txt" > "$W/inc.log" 2>&1 && [ "$(stat -c %s "$W/inc/t.ima")" = 1474560 ] && fsck.fat -n "$W/inc/t.ima" > /dev/null 2>&1 \
  && ok "truncated image can still be updated (restored to full size)" || { bad "truncated image update"; cat "$W/inc.log"; }

# --- options: lfn=off (8.3 names only) and boot=<file> (boot code from a file or image)
mkdir -p "$W/opt"; echo x > "$W/opt/A Long Name.txt"
( cd "$W/opt" && "$Z" a -tf12 -msize=720k -mlfn=off ../lfn.ima "A Long Name.txt" > /dev/null 2>&1 )
"$Z" l "$W/lfn.ima" 2>/dev/null | grep -q " ALONGN~1.TXT$" && fsck.fat -n "$W/lfn.ima" > /dev/null 2>&1 \
  && ok "lfn=off writes 8.3 names" || bad "lfn=off"
mkfs.fat -C "$W/bootsrc.ima" 1440 > /dev/null 2>&1
"$Z" a -tf12 -msize=720k "-mboot=$W/bootsrc.ima" "$W/boot.ima" "$W/extra.txt" > /dev/null 2>&1
[ "$(head -c 510 "$W/bootsrc.ima" | tail -c +63 | md5sum)" = "$(head -c 510 "$W/boot.ima" | tail -c +63 | md5sum)" ] \
  && fsck.fat -n "$W/boot.ima" > /dev/null 2>&1 && ok "boot= copies the boot code" || bad "boot="
"$Z" a -tf12 "-mboot=$W/extra.txt" "$W/badboot.ima" "$W/extra.txt" > "$W/boot.log" 2>&1
grep -q "boot sector file is shorter than 512 bytes" "$W/boot.log" && [ ! -e "$W/badboot.ima" ] \
  && ok "bad boot file rejected" || { bad "bad boot file"; cat "$W/boot.log"; }

[ $fail = 0 ] && echo "ALL PLUGIN TESTS PASSED" || echo "SOME PLUGIN TESTS FAILED"
exit $fail
