#!/bin/sh
# Cross-compile fat12.dll on Linux with MinGW-w64.  x86 DLL: Windows 2000 and later; x64 DLL: XP x64 and later.
#   apt install g++-mingw-w64-x86-64   (64-bit 7-Zip)
#   apt install g++-mingw-w64-i686     (32-bit 7-Zip, optional)
set -e
cd "$(dirname "$0")"
build() {  # $1 = compiler, $2 = output dir, $3 = minimum Windows version (PE header)
  command -v "$1" >/dev/null || { echo "skip $2: $1 not installed"; return 0; }
  mkdir -p "dist/$2" "build/$2"
  "${1%g++}windres" -O coff -o "build/$2/fat12.res.o" src/fat12.rc
  "$1" -std=c++17 -O2 -Wall -Wextra -Wno-non-virtual-dtor \
      -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0500 \
      -shared -static -static-libgcc -static-libstdc++ -s \
      -Wl,--enable-stdcall-fixup -Wl,--major-os-version,5,--minor-os-version,$3 \
      -Wl,--major-subsystem-version,5,--minor-subsystem-version,$3 \
      -o "dist/$2/fat12.dll" src/plugin.cpp src/fat12.cpp src/xp_compat.cpp src/fat12.def "build/$2/fat12.res.o" -loleaut32 -luser32
  echo "built dist/$2/fat12.dll"
}
# Windows XP: x86 = 5.1, x64 = 5.2
build x86_64-w64-mingw32-g++ x64 2
build i686-w64-mingw32-g++ x86 0   # 5.0 = Windows 2000
