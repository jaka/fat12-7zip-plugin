# Convenience targets (Linux host).
#   make            build Windows DLLs into dist/ (MinGW-w64 cross-compilers)
#   make zip        also package dist/fat12-7zip-plugin.zip
#   make test SRC7Z=/path/to/7zip-src/CPP   run the Linux test suite
#   make lint       strict compiler warnings (Linux + Windows code paths),
#                   shell syntax, and cppcheck/clang-tidy/shellcheck if installed
#   make fuzz       fuzz the FAT12 parser with ASan/UBSan (FUZZ_RUNS=, FUZZ_SEED=);
#                   uses libFuzzer if clang++ is installed, else a built-in driver
#   make format     reformat sources with clang-format (./.clang-format)
#   make format-check  fail if any source is not formatted
#   make clean      remove build/ and dist/

.PHONY: all dll zip test lint fuzz format format-check clean

# Strict warnings that must stay clean.  Not enabled: -Wold-style-cast,
# -Wuseless-cast, -Wsign-conversion (reviewed: style only / guarded indices).
LINT_FLAGS = -fsyntax-only -Werror -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
             -Wcast-qual -Wnull-dereference -Wduplicated-cond -Wduplicated-branches \
             -Wlogical-op -Wformat=2 -Wundef -Wno-non-virtual-dtor
WIN_DEFS   = -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0500
SH_FILES   = build_mingw.sh tests/*.sh
CXX_FILES  = $(wildcard src/*.cpp src/*.h tests/*.cpp)

all: dll

dll:
	./build_mingw.sh

zip: dll
	python3 -c "import zipfile, os; z = zipfile.ZipFile('dist/fat12-7zip-plugin.zip', 'w', zipfile.ZIP_DEFLATED); \
	[z.write(f'dist/{a}/fat12.dll', f'{a}/fat12.dll') for a in ('x86', 'x64') if os.path.exists(f'dist/{a}/fat12.dll')]; \
	z.write('README.md'); z.close()"
	@echo "packaged dist/fat12-7zip-plugin.zip"

FUZZ_RUNS ?= 100000
FUZZ_SEED ?= 1
SAN_FLAGS  = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

fuzz:
	mkdir -p build
	@if command -v clang++ >/dev/null; then \
	  echo "libFuzzer (clang++)"; \
	  clang++ -std=c++17 $(SAN_FLAGS) -fsanitize=fuzzer -o build/fuzz_fat12 tests/fuzz_fat12.cpp src/fat12.cpp && \
	  mkdir -p build/fuzz_corpus && \
	  build/fuzz_fat12 -runs=$(FUZZ_RUNS) -seed=$(FUZZ_SEED) build/fuzz_corpus tests/fuzz_regress; \
	else \
	  echo "built-in driver (g++; install clang for libFuzzer)"; \
	  g++ -std=c++17 $(SAN_FLAGS) -DFUZZ_STANDALONE -o build/fuzz_fat12 tests/fuzz_fat12.cpp src/fat12.cpp && \
	  build/fuzz_fat12 tests/fuzz_regress && \
	  build/fuzz_fat12 -runs=$(FUZZ_RUNS) -seed=$(FUZZ_SEED) -crash=build/fuzz-crash.img tests/fuzz_regress; \
	fi

test:
	@test -n "$(SRC7Z)" || { echo "usage: make test SRC7Z=/path/to/7zip-src/CPP"; exit 2; }
	tests/build_and_test_linux.sh "$(SRC7Z)" build

lint:
	g++ -std=c++17 $(LINT_FLAGS) src/fat12.cpp
	g++ -std=c++20 $(LINT_FLAGS) tests/fat12tool.cpp
	g++ -std=c++17 $(LINT_FLAGS) -DFUZZ_STANDALONE tests/fuzz_fat12.cpp
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
