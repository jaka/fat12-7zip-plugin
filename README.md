# FAT12 floppy image plugin for 7-Zip

[![CI](https://github.com/jaka/7zip-f12/actions/workflows/ci.yml/badge.svg)](https://github.com/jaka/7zip-f12/actions/workflows/ci.yml)

A 7-Zip format plugin (`fat12.dll`) that **lists, extracts, tests, creates and
updates** FAT12 floppy disk images. 7-Zip's built-in `FAT` handler can only read
these images.

- Sizes: 160K, 180K, 320K, 360K, 720K, 1.2M, 1.44M, 1.68M (DMF), 2.88M, plus
  computed layouts for non-standard sizes
- Long file names (VFAT LFN) with Windows-style `NAME~1.EXT` short names,
  lowercase 8.3 names via NT case flags, Unicode names
- Volume label, timestamps (modified / created / accessed), attributes
- Add, delete, rename and update files in existing images, which keep their
  size, label, serial number and boot sector
- Opens DOS 1.x disks that have no BPB, and handles damaged images without
  crashing (bad files are reported as data errors)

## Requirements

- x86 DLL: Windows 2000 or later; x64 DLL: Windows XP x64 or later
- 7-Zip whose bitness matches the DLL (32-bit 7-Zip needs the x86 DLL).
  Developed against the 7-Zip 26.00 plugin interface.

## Install

**With the installer** (Windows 2000 or later): run
`fat12-7zip-plugin-<version>-setup.exe`. It finds every installed 7-Zip,
checks whether each one is 32-bit or 64-bit, and copies the matching DLL into
its `Formats` folder. If 7-Zip isn't found, it asks for the folder. Uninstall
it from Add/Remove Programs. Silent: `setup.exe /S` (exit code 2 if no 7-Zip
was found), `uninstall.exe /S`. Build it with `make installer` (needs NSIS 3).

**By hand**:

1. Build `fat12.dll` (see below). It must match 7-Zip's bitness: x64 DLL for
   64-bit 7-Zip, x86 DLL for 32-bit 7-Zip.
2. Create a `Formats` folder next to `7z.exe`, for example
   `C:\Program Files\7-Zip\Formats\`, and copy `fat12.dll` into it.
3. Check with `7z i`. The format list should include
   `f12  ima img vfd flp` with `C` (can create) in the flags column.

## Usage

### Creating an image in the 7-Zip GUI

Choose *Add to archive…* and set **Archive format: f12**. When you click OK,
the plugin asks which floppy size to create:

```
 Choose the size of the new floppy image
  ( ) 160 KB   – 5.25" single-sided (DOS 1.0)
  ...
  (•) 1.44 MB  – 3.5" HD (most common)
  ( ) 1.68 MB  – 3.5" DMF
  ( ) 2.88 MB  – 3.5" ED
                                   [ OK ] [ Cancel ]
```

To skip the question, type the size into the **Parameters** box, for example
`size=1.2m` or `size=720k label=GAMES`.

New images get the `.ima` extension. The dialog's *Compression level*,
*CPU threads* and memory settings have no effect: floppy images are not
compressed. 7-Zip enables those settings for every plugin format and doesn't
let a plugin turn them off.

### Changing an image in the File Manager

Open the image (double-click it), then:

- **Add files:** drag them from Explorer into the File Manager window, or
  show two panels (F9), select files in the other panel and press F5 (Copy).
  The *Add* toolbar button does not work here: it only creates new archives
  from files in a normal folder, so inside an open archive it says
  "Operation is not supported" (for every format, not just floppy images).
- **Delete, rename, create folder:** use the usual commands (Del, F2, F7).

The image keeps its size and label. If 7-Zip reports the image as read-only,
check that it opened with this plugin (see below).

### Command line

```bat
7z a -tf12 -msize=1.44m -mlabel=MYDISK disk.img C:\stuff\*
7z a -tf12 -msize=720k  disk.ima file.txt
7z l disk.ima
7z x disk.ima -oout
7z a disk.ima more.txt            :: add, keeps the existing size and label
7z d disk.ima old.txt             :: delete
7z rn disk.ima a.txt b.txt        :: rename
7z a -msize=2.88m disk.ima x.txt  :: rebuild at a different size
```

The command-line `7z.exe` never shows a dialog. Without `-msize` it creates
1.44 MB images. Use `-msize=ask` to get the dialog anyway.

| Option | Values |
|---|---|
| `size` | `1.44m`, `1,44`, `1.44`, `1.2m`, `720k`, `720`, `360k`, `2.88`, `dmf`, `ask`, … (floppy "MB" = 1000 KiB, so 1.44 → 1440 KiB) |
| `label` | up to 11 ASCII characters, upper-cased; not allowed: `" * + , . / : ; < = > ? [ \ ] \|`; `-mlabel=` removes the label |

### `.img` vs `.ima` / `.vfd` / `.flp`

7-Zip's built-in `FAT` handler also claims `.img`, and built-in formats are
tried before plugins. As a result:

- `.ima`, `.vfd` and `.flp` files open with this plugin automatically, so
  adding and deleting files works in the File Manager.
- `.img` files open with the built-in read-only handler. On the command line,
  add `-tf12` to update them (`7z a -tf12 disk.img file`). In the GUI,
  either rename the file to `.ima` or create it fresh with *Add to archive*.

## Notes

- Updating an image rewrites it completely, with files stored contiguously.
  The original boot sector (boot code, OEM name, serial) is kept. However,
  bootable DOS system disks may stop booting, because their boot code expects
  `IO.SYS` in the first data cluster.
- FAT stores local time with 2-second precision. Times are converted with the
  current Windows time zone.
- Short names use ASCII. Names with other characters get an LFN plus a
  `~N` short name.
- A damaged image is never updated, so the update cannot silently drop
  files. If a file in it is damaged, delete that file first. If a folder is
  damaged, 7-Zip warns that not all files are listed; extract what you can
  and create a new image. The original image is left unchanged. An image
  whose unused end was cut off can still be updated, and is written back at
  full size.
- Errors such as *disk full* or *root directory full* are shown in a message
  box (GUI) or printed to stderr (console).

## Building

**Visual Studio**: from an *x64 Native Tools Command Prompt*:

```bat
build_msvc.bat
```

**CMake (MSVC or MinGW-w64)**:

```bat
cmake -B build -A x64
cmake --build build --config Release
```

**Cross-compile on Linux (MinGW-w64)**. This is the recommended way to get an
XP-compatible DLL:

```sh
sudo apt install g++-mingw-w64-i686 g++-mingw-w64-x86-64
make                      # -> dist/x86/fat12.dll, dist/x64/fat12.dll
```

### Publishing a release

CI (`.github/workflows/ci.yml`) builds and tests every push. To publish a
GitHub Release with the installer, a zip of both DLLs and their SHA-256 sums,
set the version in `src/version.h`, commit, then push a matching tag:

```sh
git tag v1.0.0 && git push origin v1.0.0
```

The release is created only if all tests, the fuzz run and the Windows build
pass, and only if the tag matches `src/version.h`.

### Windows 2000 / XP compatibility

- All builds use `_WIN32_WINNT=0x0500`, so the code calls only Windows 2000-era APIs.
  The DLL finds its own module handle via `__ImageBase` (`GetModuleHandleEx` needs XP).
  The size dialog is a classic dialog template, not a Vista task dialog.
- The PE header says the DLL needs OS version 5.0 (x86, Windows 2000) or 5.2 (x64,
  XP x64). A DLL built with the default 6.0 would not load on 2000/XP.
- `src/xp_compat.cpp` replaces the `kernel32!GetThreadId` import that GCC 13's
  libgcc adds; Windows 2000 and XP x86 don't have that function. It also
  replaces libstdc++'s verbose terminate handler, which would link in the
  48 KB C++ demangler.
- MSVC builds need the `v141_xp` toolset (VS 2017). Newer toolsets can't
  target XP, and no current MSVC toolset targets Windows 2000; use MinGW for 2000.

To check a build, run `objdump -p fat12.dll` and confirm the
`Major/MinorSubsystemVersion` fields and that every imported function exists
on Windows 2000.

The DLL is self-contained: static runtime, and no 7-Zip SDK is needed.
`src/7z_iface.h` mirrors the required parts of the 7-Zip plugin ABI, checked
against 7-Zip 26.00.

## Layout

| File | |
|---|---|
| `src/fat12.h/.cpp` | Portable FAT12 reader/writer, with no OS dependencies |
| `src/plugin.cpp` | 7-Zip COM handler and DLL exports, size dialog |
| `src/7z_iface.h` | 7-Zip interface and GUID definitions |
| `src/xp_compat.cpp` | MinGW-only shims: Windows 2000/XP support, smaller DLL |
| `src/fat12.def` | Export names |
| `src/fat12.rc`, `src/version.h` | Version resource (Explorer → Properties → Details); bump the version in `version.h` |
| `LICENSE` | GNU General Public License version 2 (the project is version 2 or later) |
| `installer/fat12.nsi` | NSIS installer script (`make installer`) |
| `tests/` | Linux test suite and fuzzer (see below) |
| `tests/fuzz_regress/` | Inputs that once crashed the parser or made it slow or memory-hungry; replayed by every test run |
| `tests/golden/` | Reviewed expected output for the unit tests (generated short names) |

## Tests (Linux)

The same plugin source also builds as a Linux `.so`, which 7-Zip for Linux
loads from its own `Formats/` folder. The test suite runs real 7-Zip
end-to-end and checks every image it writes with `fsck.fat` and with 7-Zip's
built-in FAT reader:

```sh
# needs g++, dosfstools, the 7zip package, and the 7-Zip source tree
tests/build_and_test_linux.sh /path/to/7zip-src/CPP
```

It also runs `tests/unit_fat12.cpp`, which checks the core directly:

- hostile images open within a heap budget, measured by replacing `operator new`
- reads past the end of a truncated image are data errors
- volume labels round-trip through code page 437
- short names match the reviewed list in `tests/golden/shortnames.txt`
- the directory tree builds into an unchanged image
- a directory of 10,000 similar names builds in under 2 seconds
- `Build()` refuses trees that `Image::Open` could not list completely
  (more than 64 nested folders, or too many long paths)

And `tests/handler_fat12.cpp`, which loads the plugin and drives it the way
the 7-Zip File Manager does: it updates the image through the handler that
opened it (the command line creates a new handler instead), then closes and
reopens that same handler. It covers add, delete, rename, new folder,
refused updates and creating a new image, using 7-Zip's own interface headers
so a mismatch in `src/7z_iface.h` shows up too.

If you change short-name generation on purpose, regenerate the list with
`build/unit_fat12 tests --update-golden` and review the diff before committing.

### Fuzzing

7-Zip offers every file you open to each plugin, so the parser has to survive
corrupt and hostile images. `tests/fuzz_fat12.cpp` feeds mutated images to
`IsArc`, `Image::Open` and `ReadFile` under AddressSanitizer and UBSan. It
checks that parsed entries are consistent, and that an image rebuilt from
whatever was read opens again cleanly:

```sh
make fuzz                    # 100000 runs; FUZZ_RUNS=, FUZZ_SEED= to change
build/fuzz_fat12 crash.img   # replay one input
```

An input also fails if it is slow or uses too much memory, because hostile
images can exhaust memory without ever crashing. The limits are 5 s per input
(`FUZZ_TIMEOUT=`), 512 MB peak RSS (`FUZZ_RSS_MB=`), and 128 MB for any
single allocation (`FUZZ_MALLOC_MB=`).

With `clang++` and its sanitizer runtime installed (on Debian/Ubuntu,
`clang` plus `libclang-rt-<version>-dev`), this uses libFuzzer and saves a
failing input as `build/crash-*`, `build/oom-*` or `build/timeout-*`.
Otherwise it uses a built-in driver that starts from images built by `Build()`
and mutates mostly the boot sector, FATs and directories; it saves a failing
input to `build/fuzz-crash.img`. After fixing the bug, add the input to
`tests/fuzz_regress/`.
`make test` replays that folder and runs a short fuzz pass.

## License

Copyright (C) 2026 jaka

Source code: <https://github.com/jaka/7zip-f12>

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2 of the License, or (at your option) any
later version. It is distributed WITHOUT ANY WARRANTY; see
[LICENSE](LICENSE) for the full text of version 2.

`src/7z_iface.h` restates the 7-Zip plugin interface (vtable layouts and
GUIDs) from the 7-Zip headers, which are LGPL 2.1 or later and so can be used
in a program under GPL v2 or later. The Linux test build also compiles 7-Zip's
`Common/MyWindows.cpp`, which is not part of this repository.
