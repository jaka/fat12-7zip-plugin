#!/bin/bash
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

[ $fail = 0 ] && echo "ALL PLUGIN TESTS PASSED" || echo "SOME PLUGIN TESTS FAILED"
exit $fail
