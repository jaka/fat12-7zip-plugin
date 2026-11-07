# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 jaka
# Convenience targets (Linux host).
#   make            build Windows DLLs into dist/{x86,x64}/fat12.dll with MinGW-w64
#                   cross-compilers; an architecture whose compiler is missing is skipped
#                   (apt install g++-mingw-w64-x86-64 g++-mingw-w64-i686)
#   make zip        also package dist/fat12-7zip-plugin.zip
#   make installer  also build dist/fat12-7zip-plugin-<ver>-setup.exe (needs NSIS 3: apt install nsis)
#   make test SRC7Z=/path/to/7zip-src/CPP   run the Linux test suite
#   make lint       strict compiler warnings (Linux + Windows code paths),
#                   shell syntax, and cppcheck/clang-tidy/shellcheck if installed
#   make fuzz       fuzz the FAT12 parser with ASan/UBSan (FUZZ_RUNS=, FUZZ_SEED=);
#                   uses libFuzzer if clang++ is installed, else a built-in driver.
#                   An input also fails if it is slow or memory-hungry (FUZZ_TIMEOUT=,
#                   FUZZ_RSS_MB=, FUZZ_MALLOC_MB=)
#   make format     reformat sources with clang-format (./.clang-format)
#   make format-check  fail if any source is not formatted
#   make clean      remove build/ and dist/

.PHONY: all dll zip installer test lint fuzz format format-check clean

# Strict warnings that must stay clean.  Not enabled: -Wold-style-cast,
# -Wuseless-cast, -Wsign-conversion (reviewed: style only / guarded indices).
LINT_FLAGS = -fsyntax-only -Werror -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
             -Wcast-qual -Wnull-dereference -Wduplicated-cond -Wduplicated-branches \
             -Wlogical-op -Wformat=2 -Wundef -Wno-non-virtual-dtor
WIN_DEFS   = -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0500
SH_FILES   = tests/*.sh
CXX_FILES  = $(wildcard src/*.cpp src/*.h tests/*.cpp)

all: dll

# ---- Windows DLLs (MinGW-w64 cross-compile) --------------------------------
# x86 DLL: Windows 2000 and later; x64 DLL: XP x64 and later.  The PE header's
# OS/subsystem version must say so: the linker default (6.0) won't load on 2000/XP.
CROSS_x64   = x86_64-w64-mingw32-
CROSS_x86   = i686-w64-mingw32-
OSMINOR_x64 = 2   # 5.2 = XP x64
OSMINOR_x86 = 0   # 5.0 = Windows 2000

WIN_SRCS     = src/plugin.cpp src/fat12.cpp src/xp_compat.cpp
WIN_DEPS     = $(WIN_SRCS) src/fat12.h src/7z_iface.h src/fat12.def
# Keep in sync with the MinGW branch of CMakeLists.txt.
# -Os and per-function sections + --gc-sections: drop unused code (mostly from the
# static C++ runtime).  The DLL is small and I/O-bound, so -Os costs nothing noticeable.
WIN_CXXFLAGS = -std=c++17 -Os -ffunction-sections -fdata-sections -Wall -Wextra -Wno-non-virtual-dtor $(WIN_DEFS)
WIN_LDFLAGS  = -shared -static -static-libgcc -static-libstdc++ -s -Wl,--enable-stdcall-fixup,--gc-sections
WIN_LIBS     = -loleaut32 -luser32

# architectures whose cross-compiler is installed
ARCHES  = $(foreach a,x64 x86,$(if $(shell command -v $(CROSS_$(a))g++),$(a)))
MISSING = $(filter-out $(ARCHES),x64 x86)

dll: $(ARCHES:%=dist/%/fat12.dll)
	@$(foreach a,$(MISSING),echo "skip $(a): $(CROSS_$(a))g++ not installed";)

dist/%/fat12.dll: $(WIN_DEPS) build/%/fat12.res.o
	@mkdir -p $(@D)
	$(CROSS_$*)g++ $(WIN_CXXFLAGS) $(WIN_LDFLAGS) \
	  -Wl,--major-os-version,5,--minor-os-version,$(strip $(OSMINOR_$*)) \
	  -Wl,--major-subsystem-version,5,--minor-subsystem-version,$(strip $(OSMINOR_$*)) \
	  -o $@ $(WIN_SRCS) src/fat12.def build/$*/fat12.res.o $(WIN_LIBS)

.PRECIOUS: build/%/fat12.res.o
build/%/fat12.res.o: src/fat12.rc src/version.h
	@mkdir -p $(@D)
	$(CROSS_$*)windres -O coff -o $@ $<

# ----------------------------------------------------------------------------

zip: dll
	python3 -c "import zipfile, os; z = zipfile.ZipFile('dist/fat12-7zip-plugin.zip', 'w', zipfile.ZIP_DEFLATED); \
	[z.write(f'dist/{a}/fat12.dll', f'{a}/fat12.dll') for a in ('x86', 'x64') if os.path.exists(f'dist/{a}/fat12.dll')]; \
	z.write('README.md'); z.close()"
	@echo "packaged dist/fat12-7zip-plugin.zip"

MAKENSIS ?= makensis

installer: dist/x86/fat12.dll dist/x64/fat12.dll
	$(MAKENSIS) -V2 -WX installer/fat12.nsi

FUZZ_RUNS ?= 100000
FUZZ_SEED ?= 1
# Per-input limits: seconds, peak RSS (MB), and largest single allocation (MB).
# Hostile images once needed 1-2 GB; a sane floppy image needs a few MB.
FUZZ_TIMEOUT   ?= 5
FUZZ_RSS_MB    ?= 512
FUZZ_MALLOC_MB ?= 128
FUZZ_LIMITS = -timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_MB)
SAN_FLAGS  = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

fuzz:
	mkdir -p build
	@if printf 'extern "C" int LLVMFuzzerTestOneInput(const char *, unsigned long) { return 0; }' | \
	    clang++ $(SAN_FLAGS) -fsanitize=fuzzer -x c++ - -o build/.fuzz_probe 2>/dev/null; then \
	  rm -f build/.fuzz_probe; \
	  echo "libFuzzer (clang++)"; \
	  clang++ -std=c++17 $(SAN_FLAGS) -fsanitize=fuzzer -o build/fuzz_fat12 tests/fuzz_fat12.cpp src/fat12.cpp && \
	  mkdir -p build/fuzz_corpus && \
	  build/fuzz_fat12 -runs=$(FUZZ_RUNS) -seed=$(FUZZ_SEED) $(FUZZ_LIMITS) -malloc_limit_mb=$(FUZZ_MALLOC_MB) \
	    -artifact_prefix=build/ build/fuzz_corpus tests/fuzz_regress; \
	else \
	  echo "built-in driver (g++; for libFuzzer install clang and its runtime, e.g. libclang-rt-dev)"; \
	  g++ -std=c++17 $(SAN_FLAGS) -DFUZZ_STANDALONE -o build/fuzz_fat12 tests/fuzz_fat12.cpp src/fat12.cpp && \
	  export ASAN_OPTIONS="max_allocation_size_mb=$(FUZZ_MALLOC_MB):allocator_may_return_null=0:$$ASAN_OPTIONS"; \
	  build/fuzz_fat12 $(FUZZ_LIMITS) -crash=build/fuzz-crash.img tests/fuzz_regress && \
	  build/fuzz_fat12 -runs=$(FUZZ_RUNS) -seed=$(FUZZ_SEED) $(FUZZ_LIMITS) -crash=build/fuzz-crash.img tests/fuzz_regress; \
	fi

test:
	@test -n "$(SRC7Z)" || { echo "usage: make test SRC7Z=/path/to/7zip-src/CPP"; exit 2; }
	tests/build_and_test_linux.sh "$(SRC7Z)" build

lint:
	g++ -std=c++17 $(LINT_FLAGS) src/fat12.cpp
	g++ -std=c++20 $(LINT_FLAGS) tests/fat12tool.cpp
	g++ -std=c++17 $(LINT_FLAGS) -DFUZZ_STANDALONE tests/fuzz_fat12.cpp
	g++ -std=c++17 $(LINT_FLAGS) tests/unit_fat12.cpp
	@for cc in i686-w64-mingw32-g++ x86_64-w64-mingw32-g++; do \
	  if command -v $$cc >/dev/null; then echo "$$cc (Windows code paths)"; \
	    $$cc -std=c++17 $(LINT_FLAGS) $(WIN_DEFS) src/plugin.cpp src/xp_compat.cpp src/fat12.cpp || exit 1; \
	  else echo "skip $$cc: not installed"; fi; done
	@for f in $(SH_FILES); do bash -n $$f || exit 1; done; echo "shell syntax ok"
	@if command -v cppcheck >/dev/null; then cppcheck --quiet --error-exitcode=1 --enable=warning,performance,portability --std=c++17 src tests; else echo "skip cppcheck: not installed"; fi
	@if command -v clang-tidy >/dev/null; then clang-tidy --quiet src/fat12.cpp tests/fat12tool.cpp -- -std=c++20; else echo "skip clang-tidy: not installed"; fi
	@if command -v shellcheck >/dev/null; then shellcheck $(SH_FILES); else echo "skip shellcheck: not installed"; fi
	@echo "lint ok"

format:
	clang-format -i $(CXX_FILES)

format-check:
	@bad=0; for f in $(CXX_FILES); do clang-format "$$f" | diff -q "$$f" - >/dev/null || { echo "needs formatting: $$f"; bad=1; }; done; \
	[ $$bad = 0 ] && echo "format ok" || { echo "run: make format"; exit 1; }

clean:
	rm -rf build dist
