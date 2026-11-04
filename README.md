# FAT12 floppy image plugin for 7-Zip

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

1. Build `fat12.dll` (see below). It must match 7-Zip's bitness: x64 DLL for
   64-bit 7-Zip, x86 DLL for 32-bit 7-Zip.
2. Create a `Formats` folder next to `7z.exe`, for example
   `C:\Program Files\7-Zip\Formats\`, and copy `fat12.dll` into it.
3. Check with `7z i`. The format list should include
   `f12  img ima vfd flp` with `C` (can create) in the flags column.

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
| `label` | up to 11 characters, upper-cased; `-mlabel=` removes the label |

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
./build_mingw.sh          # -> dist/x86/fat12.dll, dist/x64/fat12.dll
```

### Windows 2000 / XP compatibility

- All builds use `_WIN32_WINNT=0x0500`, so the code calls only Windows 2000-era APIs.
  The DLL finds its own module handle via `__ImageBase` (`GetModuleHandleEx` needs XP).
  The size dialog is a classic dialog template, not a Vista task dialog.
- The PE header says the DLL needs OS version 5.0 (x86, Windows 2000) or 5.2 (x64,
  XP x64). A DLL built with the default 6.0 would not load on 2000/XP.
- `src/xp_compat.cpp` replaces the `kernel32!GetThreadId` import that GCC 13's
  libgcc adds; Windows 2000 and XP x86 don't have that function.
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
| `src/xp_compat.cpp` | Windows XP shim for MinGW builds |
| `src/fat12.def` | Export names |
| `src/fat12.rc`, `src/version.h` | Version resource (Explorer → Properties → Details); bump the version in `version.h` |
| `tests/` | Linux test suite and fuzzer (see below) |
| `tests/fuzz_regress/` | Inputs that once crashed the parser; replayed by every test run |

## Tests (Linux)

The same plugin source also builds as a Linux `.so`, which 7-Zip for Linux
loads from its own `Formats/` folder. The test suite runs real 7-Zip
end-to-end and checks every image it writes with `fsck.fat` and with 7-Zip's
built-in FAT reader:

```sh
# needs g++, dosfstools, the 7zip package, and the 7-Zip source tree
tests/build_and_test_linux.sh /path/to/7zip-src/CPP
```

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

With `clang++` installed, this uses libFuzzer. Otherwise it uses a built-in
driver that starts from images built by `Build()` and mutates mostly the boot
sector, FATs and directories. A crashing input is saved to
`build/fuzz-crash.img`; after fixing the bug, add it to `tests/fuzz_regress/`.
`make test` replays that folder and runs a short fuzz pass.
