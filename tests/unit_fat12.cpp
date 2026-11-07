// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 jaka
// unit_fat12.cpp - regression tests for the FAT12 core (src/fat12.cpp).
//
// Covers behaviour that the round-trip scripts cannot see:
//   - hostile images stay within a memory budget (peak heap measured by
//     replacing operator new) and are flagged as damaged,
//   - reads past the end of a truncated image are data errors,
//   - volume labels round-trip through CP437,
//   - generated short names match a reviewed golden list,
//   - directory-tree building gives a byte-identical image,
//   - building a big directory takes linear, not quadratic/cubic, time,
//   - Build refuses trees that Image::Open could not list completely.
//
// Build without AddressSanitizer (it replaces operator new itself):
//   g++ -std=c++17 -O2 -fsanitize=undefined tests/unit_fat12.cpp src/fat12.cpp
// Run from anywhere:   unit_fat12 <repo>/tests
// After a deliberate change to short-name generation, review and refresh
// the golden list with:   unit_fat12 <repo>/tests --update-golden
#include "../src/fat12.h"

#include <malloc.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// ------------------------------------------------------------ heap tracking

namespace {
size_t g_live = 0, g_peak = 0; // bytes; the tests are single-threaded

void *Alloc(size_t n)
{
  void *p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  g_live += malloc_usable_size(p);
  if (g_live > g_peak) g_peak = g_live;
  return p;
}
void Free(void *p) noexcept
{
  if (!p) return;
  g_live -= malloc_usable_size(p);
  std::free(p);
}
} // namespace

void *operator new(size_t n)
{
  return Alloc(n);
}
void *operator new[](size_t n)
{
  return Alloc(n);
}
void operator delete(void *p) noexcept
{
  Free(p);
}
void operator delete[](void *p) noexcept
{
  Free(p);
}
void operator delete(void *p, size_t) noexcept
{
  Free(p);
}
void operator delete[](void *p, size_t) noexcept
{
  Free(p);
}

// ------------------------------------------------------------ helpers

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &what)
{
  std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what.c_str());
  if (!cond) g_failures++;
}

std::vector<uint8_t> ReadFileBytes(const std::string &path)
{
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::u16string U(const std::string &s) // Latin-1 / ASCII literal to UTF-16
{
  std::u16string r;
  for (unsigned char c : s) r += (char16_t)c;
  return r;
}

std::string Num(size_t v)
{
  return std::to_string(v);
}

double Seconds(std::chrono::steady_clock::time_point since)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

uint64_t Fnv1a(const std::vector<uint8_t> &d)
{
  uint64_t h = 1469598103934665603ull;
  for (uint8_t b : d) h = (h ^ b) * 1099511628211ull;
  return h;
}

fat12::BuildOptions FixedOptions(uint32_t kb)
{
  fat12::BuildOptions o;
  if (!fat12::GeometryForSizeKB(kb, o.geo)) std::abort();
  o.serial = 0x12345678;
  o.nowTicks = fat12::DosToTicks(0x5021, 0); // 2020-01-01
  return o;
}

// Opens `path` and reports peak heap use; budget in MiB.
void HostileImage(const std::string &path, size_t budgetMiB, size_t maxItems, bool incomplete)
{
  const std::vector<uint8_t> data = ReadFileBytes(path);
  if (data.empty()) {
    Check(false, "read " + path);
    return;
  }
  const size_t base = g_live;
  g_peak = g_live;
  fat12::Image img;
  const auto t = std::chrono::steady_clock::now();
  const bool ok = img.Open(data);
  const double secs = Seconds(t);
  // Read every file too: that is what 7z t / 7z x does next.
  std::vector<uint8_t> out;
  for (size_t i = 0; i < img.Items().size(); i++) (void)img.ReadFile(i, out);
  const size_t peakKiB = (g_peak - base) >> 10;
  const std::string name = path.substr(path.rfind('/') + 1);
  Check(ok && img.HeadersWarning(), name + ": opens, flagged as damaged");
  Check(img.ListingIncomplete() == incomplete,
        name + (incomplete ? ": listing flagged incomplete (update refused)" : ": listing not flagged incomplete"));
  Check(peakKiB <= budgetMiB << 10,
        name + ": peak heap " + Num(peakKiB) + " KiB (budget " + Num(budgetMiB) + " MiB)");
  Check(img.Items().size() <= maxItems, name + ": " + Num(img.Items().size()) + " items (max " + Num(maxItems) + ")");
  Check(secs < 1.0, name + ": opened in " + std::to_string(secs) + " s");
}

// ------------------------------------------------------------ tests

void TestHostileImages(const std::string &tests)
{
  std::printf("hostile images\n");
  // 24 KiB file whose BPB claims ~2 GiB: must not be padded to that size.
  HostileImage(tests + "/fuzz_regress/huge-claimed-size.img", 8, 0, false);
  // Cross-linked directories that fan out 16x per level: must not expand.
  // A sane image parses each directory cluster once, so items <= data/32.
  HostileImage(tests + "/fuzz_regress/cross-linked-dirs.img", 16, 1474560 / 32, true);
  // Folders nested deeper than Open descends (found by libFuzzer).
  HostileImage(tests + "/fuzz_regress/deep-folders.img", 16, 1474560 / 32, true);
}

void TestTruncatedRead()
{
  std::printf("truncated image\n");
  fat12::BuildOptions o = FixedOptions(1440);
  std::vector<fat12::NewItem> items(2);
  items[0].path = u"first.bin";
  items[0].data.assign(4096, 1);
  items[1].path = u"second.bin";
  items[1].data.assign(4096, 2);
  std::vector<uint8_t> img;
  std::string err;
  if (!fat12::Build(o, items, img, &err)) return Check(false, "build: " + err);
  // Files are contiguous from cluster 2: cut the image inside second.bin.
  img.resize((33 + 8 + 3) * 512);
  fat12::Image im;
  Check(im.Open(img) && im.HeadersWarning(), "truncated image opens with a warning");
  // Every entry is still listed (a file past the cut fails on its own), so
  // the image may be updated: some tools save images with the unused end cut.
  Check(!im.ListingIncomplete(), "truncated image is listed completely");
  std::vector<uint8_t> out;
  Check(im.Items().size() == 2 && im.ReadFile(0, out) && out == items[0].data, "file before the cut reads ok");
  Check(im.Items().size() == 2 && !im.ReadFile(1, out), "file past the cut is a data error, not zeros");
}

void TestLabels()
{
  std::printf("volume labels\n");
  struct Case {
    std::u16string label, expect;
    const char *what;
  };
  const Case cases[] = {
      {u"DISKé", u"DISKé", "CP437 letter (e acute) survives"},
      {u"σDISK", u"σDISK", "label starting with byte 0xE5 (sigma) survives"},
      {u"A中B", u"A_B", "character outside CP437 becomes '_'"},
      {u"PLAIN", u"PLAIN", "ASCII label"},
  };
  for (const Case &c : cases) {
    fat12::BuildOptions o = FixedOptions(1440);
    o.label = c.label;
    std::vector<uint8_t> img, img2;
    std::vector<fat12::NewItem> none;
    fat12::Build(o, none, img, nullptr);
    fat12::Image a;
    const bool ok1 = a.Open(img) && a.VolumeLabel() == c.expect;
    // Round trip like an update does: label and boot sector from the old image.
    o.label = a.VolumeLabel();
    o.bootSector = &a.Raw();
    fat12::Build(o, none, img2, nullptr);
    fat12::Image b;
    const bool ok2 = b.Open(img2) && b.VolumeLabel() == c.expect;
    Check(ok1 && ok2, c.what);
  }
}

// Short names for a deliberately awkward set of names, one per line.
std::string ShortNameListing()
{
  fat12::BuildOptions o = FixedOptions(2880);
  std::vector<fat12::NewItem> it;
  auto add = [&](const std::string &p, bool dir = false) {
    fat12::NewItem n;
    n.path = U(p);
    n.isDir = dir;
    it.push_back(n);
  };
  const char *bases[] = {"Long file name", "long file NAME", "LongFi", "a b", "My.Document.v2", "x"};
  const char *exts[] = {".txt", ".TXT", ".text", "", ".tx t"};
  for (int d = 0; d < 2; d++) {
    const std::string pre = d ? "sub/" : "top/";
    for (const char *b : bases)
      for (const char *e : exts)
        for (int i = 0; i < 40; i++) add(pre + b + std::to_string(i) + e);
    for (int i = 0; i < 30; i++) add(pre + "LONGFI~" + std::to_string(i) + ".TXT"); // tails already taken
    for (int i = 0; i < 15; i++) add(pre + "Long file name.txt" + std::string((size_t)i, 'x'));
    add(pre + ".hidden");
    add(pre + "..x..");
    add(pre + "README.txt");
    add(pre + "readme.TXT2");
    const std::string uni = "\xDCn\xEF"; // Latin-1 U+00DC, U+00EF ("Uni" with diacritics)
    add(pre + uni + "code name.txt");
    add(pre + uni + "code name2.txt");
    add(pre + "Dir One", true);
  }
  std::vector<uint8_t> img;
  std::string err;
  if (!fat12::Build(o, it, img, &err)) return "build failed: " + err + "\n";
  fat12::Image im;
  if (!im.Open(img)) return "open failed\n";
  std::string s;
  for (const fat12::Entry &e : im.Items()) {
    for (char16_t c : e.shortName) s += (char)(c < 0x80 ? c : '?');
    s += '\n';
  }
  return s;
}

void TestShortNames(const std::string &tests, bool update)
{
  std::printf("short names\n");
  const std::string golden = tests + "/golden/shortnames.txt";
  const std::string got = ShortNameListing();
  if (update) {
    std::ofstream(golden, std::ios::binary) << got;
    std::printf("  wrote %s - review the diff before committing\n", golden.c_str());
    return;
  }
  const std::vector<uint8_t> want = ReadFileBytes(golden);
  const bool same = std::string(want.begin(), want.end()) == got;
  Check(same, "short names match " + golden.substr(golden.rfind("tests/")));
  if (!same) {
    const std::string actual = golden + ".actual";
    std::ofstream(actual, std::ios::binary) << got;
    std::printf("       actual output written to %s; diff it against the golden file\n", actual.c_str());
  }

  bool lfn = false;
  uint8_t cf = 0;
  const std::vector<std::u16string> used = {u"LONGFI~1TXT", u"LONGFI~2TXT"};
  Check(fat12::MakeShortName(u"Long file name.txt", used, &lfn, &cf) == u"LONGFI~3TXT" && lfn,
        "MakeShortName skips taken tails");
  Check(fat12::MakeShortName(u"readme.txt", {}, &lfn, &cf) == u"README  TXT" && !lfn && cf == 0x18,
        "MakeShortName keeps lowercase 8.3 via case flags");
}

void TestTreeBuilding()
{
  std::printf("directory tree\n");
  fat12::BuildOptions o = FixedOptions(2880);
  o.label = u"TEST";
  std::vector<fat12::NewItem> it;
  int seq = 0;
  auto add = [&](const std::u16string &p, bool dir = false) {
    fat12::NewItem n;
    n.path = p;
    n.isDir = dir;
    n.data.assign((size_t)(seq % 7) * 100, (uint8_t)seq);
    seq++;
    it.push_back(n);
  };
  for (int i = 0; i < 300; i++) {
    add(u"Docs/Report " + U(std::to_string(i)) + u".txt");
    add(u"DOCS/sub/File" + U(std::to_string(i)));
  }
  add(u"docs/report 5.TXT"); // case duplicate of an existing file: later one wins
  add(u"Docs\\Sub\\file7");  // backslashes, different case
  add(u"Café/x.txt");        // Latin-1 case folding: one directory
  add(u"CAFÉ/y.txt");
  add(u"café/Z.TXT");
  add(u"./a/./b/c.txt");
  add(u"a/B/d.txt");
  add(u"Ω/one"); // outside Latin-1: not folded, two directories
  add(u"ω/two");
  add(u"empty dir", true);
  add(u"EMPTY DIR/inner.txt");
  std::vector<uint8_t> img;
  std::string err;
  const bool ok = fat12::Build(o, it, img, &err);
  // Image produced by the original sibling-scan implementation.  If this
  // changes, the directory layout changed: check that it was intended.
  const uint64_t kExpected = 0xff0e48eef19925ddull;
  char hex[32];
  std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)Fnv1a(img));
  Check(ok && Fnv1a(img) == kExpected, std::string("tree image unchanged (fnv1a ") + hex + ")");

  std::vector<fat12::NewItem> clash(2);
  clash[0].path = u"x/y";
  clash[1].path = u"X";
  err.clear();
  Check(!fat12::Build(o, clash, img, &err) && err == "name used both as file and directory: X",
        "file/directory name clash is rejected");
}

void TestBigDirectoryTime()
{
  std::printf("big directory timing\n");
  const size_t n = 10000;
  fat12::BuildOptions o = FixedOptions(2880);
  std::vector<fat12::NewItem> it(n);
  for (size_t i = 0; i < n; i++) it[i].path = u"dir/Long file name " + U(std::to_string(i)) + u".txt";
  std::vector<uint8_t> img;
  std::string err;
  const auto t = std::chrono::steady_clock::now();
  const bool ok = fat12::Build(o, it, img, &err);
  const double secs = Seconds(t);
  // Linear code needs ~0.05 s; the old cubic code needed minutes.
  Check(ok && secs < 2.0, Num(n) + " similar names built in " + std::to_string(secs) + " s (limit 2 s)");
}

} // namespace

// Build must never write an image that Image::Open then reports as damaged:
// 7-Zip would list it partially and a later update would drop the rest.
void TestBuildLimits()
{
  std::printf("build limits\n");
  const fat12::BuildOptions o = FixedOptions(1440);
  std::vector<uint8_t> img;
  std::string err;
  fat12::Image im;

  std::u16string deep; // 64 nested folders: the most Open descends into
  for (int i = 0; i < 64; i++) deep += U("d" + std::to_string(i)) + u"/";
  std::vector<fat12::NewItem> it(1);
  it[0].path = deep + u"f.txt";
  bool ok = fat12::Build(o, it, img, &err);
  Check(ok && im.Open(img) && !im.HeadersWarning() && im.Items().size() == 65,
        "file in 64 nested folders builds and reads back completely");
  it[0].path = deep + u"d64";
  it[0].isDir = true;
  ok = fat12::Build(o, it, img, &err);
  Check(!ok && err.find("folders nested more than 64 levels deep") == 0,
        "65 nested folders are rejected (" + err + ")");

  // About 16.6k path characters per file times 1100 files is more than the
  // total path length Image::Open accepts, yet it fits on a 1.44 MB disk.
  const std::u16string longName(250, u'n');
  std::u16string prefix;
  for (int i = 0; i < 64; i++) prefix += U(std::to_string(i % 10)) + longName + u"/";
  it.assign(1100, fat12::NewItem());
  for (size_t i = 0; i < it.size(); i++) it[i].path = prefix + U(std::to_string(1000 + i)) + longName;
  err.clear();
  ok = fat12::Build(o, it, img, &err);
  Check(!ok && err.find("could not be read back completely") != std::string::npos,
        "tree over the total path length limit is rejected (" + err + ")");
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: unit_fat12 <repo>/tests [--update-golden]\n");
    return 2;
  }
  const std::string tests = argv[1];
  const bool update = argc > 2 && std::string(argv[2]) == "--update-golden";
  std::setvbuf(stdout, nullptr, _IOLBF, 0); // results stay visible if a slow test is killed
  if (update) {
    TestShortNames(tests, true);
    return 0;
  }
  TestHostileImages(tests);
  TestTruncatedRead();
  TestLabels();
  TestShortNames(tests, update);
  TestTreeBuilding();
  TestBigDirectoryTime();
  TestBuildLimits();
  std::printf(g_failures ? "SOME UNIT TESTS FAILED\n" : "ALL UNIT TESTS PASSED\n");
  return g_failures ? 1 : 0;
}
