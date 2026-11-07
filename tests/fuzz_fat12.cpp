// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 jaka
// fuzz_fat12.cpp - fuzz target for the FAT12 core (src/fat12.cpp).
//
// 7-Zip probes every file the user opens with each plugin, so IsArc(),
// Image::Open() and Image::ReadFile() see arbitrary, possibly hostile data.
// This target feeds them mutated images and checks invariants:
//   - no crash / out-of-bounds access / UB (build with ASan + UBSan),
//   - every parsed entry is consistent (parent index, size, path),
//   - an image rebuilt from whatever was read opens again cleanly.
//
// Two ways to build it:
//   clang++ -fsanitize=fuzzer,address,undefined ...   (libFuzzer drives it)
//   g++ -DFUZZ_STANDALONE -fsanitize=address,undefined ...  (built-in driver
//       below: seeds from Build(), random mutations biased towards the boot
//       sector, FAT and directories)
// See `make fuzz`.
#include "../src/fat12.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const char *what)
{
  std::fprintf(stderr, "invariant failed: %s\n", what);
  std::abort();
}

void CheckSizeHelpers(const uint8_t *data, size_t size)
{
  // ParseSizeSpec on the first bytes as a short string.
  std::u16string spec;
  for (size_t i = 0; i < size && i < 12; i++) spec += (char16_t)data[i];
  uint32_t kb = 0;
  if (fat12::ParseSizeSpec(spec, kb)) {
    fat12::Geometry g;
    (void)fat12::GeometryForSizeKB(kb, g);
  }
}

// Builds a fresh image from what was read and checks that it opens again.
void CheckRebuild(const fat12::Image &img)
{
  std::vector<fat12::NewItem> items;
  const auto &src = img.Items();
  for (size_t i = 0; i < src.size(); i++) {
    fat12::NewItem it;
    it.path = src[i].path;
    it.isDir = src[i].IsDir();
    it.attr = (uint8_t)(src[i].attr & 0x27);
    it.hasMTime = true;
    it.mTicks = fat12::DosToTicks(src[i].mDate, src[i].mTime);
    if (!it.isDir) (void)img.ReadFile(i, it.data);
    items.push_back(std::move(it));
  }
  fat12::BuildOptions opt;
  if (!fat12::GeometryForSizeKB(1440, opt.geo)) Fail("1440 KB geometry");
  opt.label = img.VolumeLabel();
  opt.nowTicks = fat12::DosToTicks(0x5021, 0); // 2020-01-01
  std::vector<uint8_t> out;
  std::string err;
  if (!fat12::Build(opt, items, out, &err)) return; // disk full, bad names, ...: fine

  if (out.size() != opt.geo.ImageBytes()) Fail("rebuilt image has wrong size");
  if (fat12::IsArc(out.data(), out.size()) != 1) Fail("rebuilt image fails IsArc");
  fat12::Image again;
  if (!again.Open(out, &err)) Fail("rebuilt image does not open");
  if (again.HeadersWarning()) Fail("rebuilt image has header warnings");
  if (again.Items().size() > items.size()) Fail("rebuilt image has extra items");
  std::vector<uint8_t> data;
  for (size_t i = 0; i < again.Items().size(); i++)
    if (!again.ReadFile(i, data)) Fail("rebuilt image has a data error");
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  CheckSizeHelpers(data, size);

  const unsigned arc = fat12::IsArc(data, size);
  if (arc > 2) Fail("IsArc result out of range");

  fat12::Image img;
  std::string err;
  if (!img.Open(std::vector<uint8_t>(data, data + size), &err)) {
    if (err.empty()) Fail("Open failed without an error message");
    return 0;
  }
  const auto &items = img.Items();
  for (size_t i = 0; i < items.size(); i++) {
    const fat12::Entry &e = items[i];
    if (e.parent >= (int)i) Fail("parent index not before child");
    if (e.parent >= 0 && !items[(size_t)e.parent].IsDir()) Fail("parent is not a directory");
    if (e.path.empty()) Fail("empty path");
    if (e.IsDir() && e.size != 0) Fail("directory with a size");
    std::vector<uint8_t> out;
    const bool ok = img.ReadFile(i, out);
    if (out.size() > e.size) Fail("ReadFile returned more than the file size");
    if (ok && out.size() != e.size) Fail("ReadFile ok but short");
  }
  (void)img.VolumeLabel();
  const uint64_t freeBytes = img.FreeBytes();
  if (freeBytes > img.Geo().ImageBytes()) Fail("free space larger than the disk");
  CheckRebuild(img);
  return 0;
}

// ======================================================= standalone driver
#ifdef FUZZ_STANDALONE

#include <sanitizer/common_interface_defs.h>

#include <chrono>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>

// No single allocation may exceed 128 MiB (like libFuzzer's -malloc_limit_mb):
// a parser that sizes a buffer from header fields fails here instead of
// quietly allocating gigabytes.  ASAN_OPTIONS in the environment overrides it.
extern "C" const char *__asan_default_options()
{
  return "max_allocation_size_mb=128:allocator_may_return_null=0";
}

namespace {

std::vector<uint8_t> g_current; // input under test, saved if we crash
const char *g_crashFile = "fuzz-crash.img";
long g_rssLimitMb = 512;   // -rss_limit_mb=, 0 = off
unsigned g_timeoutSec = 5; // -timeout=, per input, 0 = off

// Async-signal-safe: also called from the SIGALRM handler.
void SaveCurrent()
{
  const int fd = ::open(g_crashFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  const uint8_t *p = g_current.data();
  size_t left = g_current.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n <= 0) break;
    p += n;
    left -= (size_t)n;
  }
  ::close(fd);
  const char msg[] = "input saved to the -crash= file\n";
  (void)!::write(2, msg, sizeof msg - 1);
}

void OnTimeout(int)
{
  const char msg[] = "\nfuzz: input exceeded -timeout (slow input, e.g. quadratic parsing)\n";
  (void)!::write(2, msg, sizeof msg - 1);
  SaveCurrent();
  ::_exit(70);
}

long PeakRssMb()
{
  rusage ru;
  ::getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss / 1024; // Linux: KiB
}

void RunOne(const std::vector<uint8_t> &input)
{
  g_current = input;
  ::alarm(g_timeoutSec);
  LLVMFuzzerTestOneInput(input.data(), input.size());
  ::alarm(0);
  // The peak only grows, so the first input that pushes it over is the culprit.
  if (g_rssLimitMb > 0 && PeakRssMb() > g_rssLimitMb) {
    std::fprintf(stderr, "\nfuzz: peak RSS %ld MB exceeds -rss_limit_mb=%ld\n", PeakRssMb(), g_rssLimitMb);
    SaveCurrent();
    std::abort();
  }
}

std::vector<uint8_t> ReadAll(const std::string &path)
{
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Rng {
  uint64_t s;
  uint64_t Next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  size_t Below(size_t n)
  {
    return n ? (size_t)(Next() % n) : 0;
  }
};

// Seed images: every standard size, empty and populated (LFNs, subdirs,
// multi-cluster files, volume label).
std::vector<std::vector<uint8_t>> MakeSeeds()
{
  std::vector<std::vector<uint8_t>> seeds;
  const uint32_t sizes[] = {160, 180, 320, 360, 720, 1200, 1440, 1680, 2880, 4000};
  for (uint32_t kb : sizes) {
    for (int populated = 0; populated < 2; populated++) {
      fat12::BuildOptions opt;
      if (!fat12::GeometryForSizeKB(kb, opt.geo)) continue;
      opt.label = u"SEED";
      opt.serial = 0x12345678;
      opt.nowTicks = fat12::DosToTicks(0x5021, 0);
      std::vector<fat12::NewItem> items;
      if (populated) {
        const char16_t *names[] = {u"README.TXT", u"A long file name.text", u"dir/sub/deep.bin",
                                   u"dir/Mixed Case.Doc", u"lower.txt", u"dir/sub/x"};
        size_t n = 0;
        for (const char16_t *name : names) {
          fat12::NewItem it;
          it.path = name;
          it.attr = fat12::kAttrArchive;
          it.data.assign(300 + 700 * n, (uint8_t)(n + 1));
          n++;
          items.push_back(it);
        }
      }
      std::vector<uint8_t> out;
      std::string err;
      if (fat12::Build(opt, items, out, &err)) seeds.push_back(out);
    }
  }
  return seeds;
}

void Mutate(std::vector<uint8_t> &d, Rng &r, uint32_t metaBytes)
{
  static const uint32_t kInteresting[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0xFF0, 0xFF7, 0xFF8, 0xFFF, 0xE5, 0x0F, 0x10, 0x2E, 0x40};
  const int n = 1 + (int)r.Below(8);
  for (int k = 0; k < n && !d.empty(); k++) {
    // Mostly hit the boot sector, FATs and root directory.
    const size_t limit = (r.Below(4) != 0) ? std::min<size_t>(metaBytes, d.size()) : d.size();
    const size_t pos = r.Below(limit);
    switch (r.Below(7)) {
      case 0:
        d[pos] ^= (uint8_t)(1u << r.Below(8));
        break;
      case 1:
        d[pos] = (uint8_t)r.Next();
        break;
      case 2:
        d[pos] = (uint8_t)kInteresting[r.Below(sizeof(kInteresting) / sizeof(kInteresting[0]))];
        break;
      case 3:
        if (pos + 1 < d.size()) {
          const uint32_t v = kInteresting[r.Below(sizeof(kInteresting) / sizeof(kInteresting[0]))];
          d[pos] = (uint8_t)v;
          d[pos + 1] = (uint8_t)(v >> 8);
        }
        break;
      case 4: { // copy a 32-byte directory entry / chunk elsewhere
        const size_t from = r.Below(limit), len = std::min<size_t>(32, d.size() - std::max(pos, from));
        std::memmove(&d[pos], &d[from], len);
        break;
      }
      case 5: // truncate (rare)
        if (r.Below(16) == 0) d.resize(r.Below(d.size() + 1));
        break;
      default:
        d[pos] = (uint8_t)(d[pos] + 1 - 2 * (int)r.Below(2));
        break;
    }
  }
}

int Usage()
{
  std::cerr << "usage: fuzz_fat12 [-runs=N] [-seed=S] [-max_total_time=SEC] [-crash=FILE]\n"
               "                  [-rss_limit_mb=MB (512)] [-timeout=SEC per input (5)] [seed files/dirs...]\n"
               "       fuzz_fat12 FILE...   (replay: run each file once, same limits)\n"
               "Single allocations over 128 MiB fail (ASAN_OPTIONS=max_allocation_size_mb=N to change).\n";
  return 2;
}

} // namespace

int main(int argc, char **argv)
{
  long runs = -1, maxTime = 0;
  uint64_t seed = 1;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("-runs=", 0) == 0) runs = std::atol(a.c_str() + 6);
    else if (a.rfind("-seed=", 0) == 0) seed = std::strtoull(a.c_str() + 6, nullptr, 0);
    else if (a.rfind("-max_total_time=", 0) == 0) maxTime = std::atol(a.c_str() + 16);
    else if (a.rfind("-crash=", 0) == 0) g_crashFile = argv[i] + 7;
    else if (a.rfind("-rss_limit_mb=", 0) == 0) g_rssLimitMb = std::atol(a.c_str() + 14);
    else if (a.rfind("-timeout=", 0) == 0) g_timeoutSec = (unsigned)std::atol(a.c_str() + 9);
    else if (a[0] == '-') return Usage();
    else paths.push_back(a);
  }
  __sanitizer_set_death_callback(SaveCurrent);
  std::signal(SIGALRM, OnTimeout);

  std::vector<std::vector<uint8_t>> corpus;
  for (const std::string &p : paths) {
    if (DIR *dir = opendir(p.c_str())) {
      while (dirent *de = readdir(dir))
        if (de->d_name[0] != '.') corpus.push_back(ReadAll(p + "/" + de->d_name));
      closedir(dir);
    }
    else corpus.push_back(ReadAll(p));
  }
  // Replay mode: files given, no -runs.
  if (runs < 0 && !paths.empty()) {
    for (auto &c : corpus) RunOne(c);
    std::cerr << "replayed " << corpus.size() << " input(s), no failures\n";
    return 0;
  }
  if (runs < 0) runs = 100000;
  for (auto &s : MakeSeeds()) corpus.push_back(std::move(s));
  for (auto &c : corpus) RunOne(c); // seeds must pass unmodified

  Rng rng{seed * 0x9E3779B97F4A7C15ull | 1};
  const auto start = std::chrono::steady_clock::now();
  long done = 0;
  for (; done < runs; done++) {
    if (maxTime > 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(maxTime)) break;
    std::vector<uint8_t> d = corpus[rng.Below(corpus.size())];
    fat12::Image probe; // metadata region = everything before the data area
    uint32_t meta = 33 * 512;
    if (probe.Open(d)) meta = probe.Geo().FirstDataSector() * probe.Geo().bytesPerSector + 8192;
    Mutate(d, rng, meta);
    RunOne(d);
    if ((done + 1) % 10000 == 0) std::cerr << "  " << (done + 1) << " runs\n";
  }
  std::cerr << "fuzz: " << done << " runs, " << corpus.size() << " seeds, seed=" << seed << ", no failures\n";
  return 0;
}

#endif // FUZZ_STANDALONE
