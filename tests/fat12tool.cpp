// fat12tool - Linux/portable CLI over the fat12 core, used by the tests.
//   fat12tool list   <img>
//   fat12tool extract <img> <outdir>
//   fat12tool create <img> <sizeKB> <srcdir> [label]
#include "../src/fat12.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static std::string ToUtf8(const std::u16string &s)
{
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    if (c < 0x80)
      r += (char)c;
    else if (c < 0x800) {
      r += (char)(0xC0 | (c >> 6));
      r += (char)(0x80 | (c & 63));
    }
    else if (c < 0x10000) {
      r += (char)(0xE0 | (c >> 12));
      r += (char)(0x80 | ((c >> 6) & 63));
      r += (char)(0x80 | (c & 63));
    }
    else {
      r += (char)(0xF0 | (c >> 18));
      r += (char)(0x80 | ((c >> 12) & 63));
      r += (char)(0x80 | ((c >> 6) & 63));
      r += (char)(0x80 | (c & 63));
    }
  }
  return r;
}

static std::u16string FromUtf8(const std::string &s)
{
  std::u16string r;
  for (size_t i = 0; i < s.size();) {
    uint32_t c = (uint8_t)s[i];
    int n = c < 0x80 ? 0 : c < 0xE0 ? 1
                       : c < 0xF0   ? 2
                                    : 3;
    c = n == 0 ? c : n == 1 ? c & 31
                 : n == 2   ? c & 15
                            : c & 7;
    for (int k = 1; k <= n && i + k < s.size(); k++) c = (c << 6) | ((uint8_t)s[i + k] & 63);
    i += n + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      r += (char16_t)(0xD800 + (c >> 10));
      r += (char16_t)(0xDC00 + (c & 1023));
    }
    else
      r += (char16_t)c;
  }
  return r;
}

static std::vector<uint8_t> ReadAll(const fs::path &p)
{
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint64_t NowTicks()
{
  return ((uint64_t)time(nullptr) + 11644473600ULL) * 10000000ULL;
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    std::cerr << "usage: see source\n";
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == "list" || cmd == "extract") {
    fat12::Image img;
    std::string err;
    if (!img.Open(ReadAll(argv[2]), &err)) {
      std::cerr << "open failed: " << err << "\n";
      return 1;
    }
    const auto &g = img.Geo();
    if (cmd == "list") {
      std::cout << "label=" << ToUtf8(img.VolumeLabel()) << " sectors=" << g.totalSectors
                << " spc=" << (int)g.sectorsPerCluster << " free=" << img.FreeBytes()
                << " warning=" << img.HeadersWarning() << "\n";
      for (const auto &e : img.Items())
        std::cout << (e.IsDir() ? "D " : "F ") << e.size << " " << ToUtf8(e.shortName) << " | "
                  << ToUtf8(e.path) << "\n";
      return 0;
    }
    fs::path out = argv[3];
    int bad = 0;
    for (size_t i = 0; i < img.Items().size(); i++) {
      const auto &e = img.Items()[i];
      fs::path p = out / ToUtf8(e.path);
      if (e.IsDir()) {
        fs::create_directories(p);
        continue;
      }
      fs::create_directories(p.parent_path());
      std::vector<uint8_t> data;
      if (!img.ReadFile(i, data)) {
        std::cerr << "data error: " << ToUtf8(e.path) << "\n";
        bad++;
      }
      std::ofstream f(p, std::ios::binary);
      f.write((const char *)data.data(), (std::streamsize)data.size());
    }
    return bad ? 1 : 0;
  }
  if (cmd == "create" && argc >= 5) {
    fat12::BuildOptions opt;
    if (!fat12::GeometryForSizeKB((uint32_t)atoi(argv[3]), opt.geo)) {
      std::cerr << "bad size\n";
      return 1;
    }
    if (argc > 5) opt.label = FromUtf8(argv[5]);
    opt.nowTicks = NowTicks();
    std::vector<fat12::NewItem> items;
    fs::path src = argv[4];
    std::vector<fs::path> paths;
    for (auto &de : fs::recursive_directory_iterator(src)) paths.push_back(de.path());
    std::sort(paths.begin(), paths.end());
    for (auto &p : paths) {
      fat12::NewItem it;
      it.path = FromUtf8(fs::relative(p, src).generic_string());
      it.isDir = fs::is_directory(p);
      it.attr = it.isDir ? 0 : fat12::kAttrArchive;
      if (!it.isDir) it.data = ReadAll(p);
      auto ft = fs::last_write_time(p);
      auto sys = std::chrono::file_clock::to_sys(ft);
      it.hasMTime = true;
      it.mTicks =
          ((uint64_t)std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count() +
           11644473600ULL) *
          10000000ULL;
      items.push_back(std::move(it));
    }
    std::vector<uint8_t> out;
    std::string err;
    if (!fat12::Build(opt, items, out, &err)) {
      std::cerr << "build failed: " << err << "\n";
      return 1;
    }
    std::ofstream f(argv[2], std::ios::binary);
    f.write((const char *)out.data(), (std::streamsize)out.size());
    return 0;
  }
  std::cerr << "bad command\n";
  return 2;
}
