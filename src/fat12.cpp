// fat12.cpp - portable FAT12 floppy image reader/writer.
#include "fat12.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace fat12 {

namespace {

inline uint16_t Get16(const uint8_t *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t Get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void Set16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
inline void Set32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

void SetErr(std::string *err, const std::string &msg)
{
  if (err) *err = msg;
}

const uint32_t kMaxFat12Clusters = 4084; // >= 4085 clusters means FAT16
const unsigned kMaxDepth = 64;
const size_t kMaxItems = 1u << 20;

// Code page 437, bytes 0x80..0xFF.
// clang-format off
const char16_t kCp437[128] = {
  0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
  0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
  0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
  0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
  0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
  0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
  0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
  0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0};
// clang-format on

char16_t OemToUnicode(uint8_t c)
{
  return c < 0x80 ? (char16_t)c : kCp437[c - 0x80];
}

char16_t Upper(char16_t c)
{
  if (c >= 'a' && c <= 'z') return (char16_t)(c - 32);
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (char16_t)(c - 32); // Latin-1
  return c;
}

bool EqualNoCase(const std::u16string &a, const std::u16string &b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (Upper(a[i]) != Upper(b[i])) return false;
  return true;
}

bool IsValidBps(unsigned v)
{
  return v == 512 || v == 1024 || v == 2048 || v == 4096;
}
bool IsPow2(unsigned v)
{
  return v != 0 && (v & (v - 1)) == 0;
}

// Parses a BIOS parameter block; returns false if it doesn't look like FAT.
bool ParseBpb(const uint8_t *p, Geometry &g)
{
  g.bytesPerSector = Get16(p + 11);
  g.sectorsPerCluster = p[13];
  g.reservedSectors = Get16(p + 14);
  g.numFats = p[16];
  g.rootEntries = Get16(p + 17);
  uint16_t tot16 = Get16(p + 19);
  g.media = p[21];
  g.sectorsPerFat = Get16(p + 22);
  g.sectorsPerTrack = Get16(p + 24);
  g.numHeads = Get16(p + 26);
  g.hiddenSectors = Get32(p + 28);
  g.totalSectors = tot16 ? tot16 : Get32(p + 32);
  if (!IsValidBps(g.bytesPerSector) || !IsPow2(g.sectorsPerCluster) || g.reservedSectors == 0 ||
      g.numFats == 0 || g.numFats > 4 || g.rootEntries == 0 || g.sectorsPerFat == 0 || g.media < 0xE5)
    return false;
  if (g.totalSectors <= g.FirstDataSector()) return false;
  return true;
}

struct StdFormat {
  uint32_t kb;
  uint16_t spc, root, spf;
  uint8_t media;
  uint16_t spt, heads;
};
// clang-format off
const StdFormat kStdFormats[] = {
  { 160, 1,  64, 1, 0xFE,  8, 1},
  { 180, 1,  64, 2, 0xFC,  9, 1},
  { 320, 2, 112, 1, 0xFF,  8, 2},
  { 360, 2, 112, 2, 0xFD,  9, 2},
  { 720, 2, 112, 3, 0xF9,  9, 2},
  {1200, 1, 224, 7, 0xF9, 15, 2},
  {1440, 1, 224, 9, 0xF0, 18, 2},
  {1680, 4,  16, 3, 0xF0, 21, 2},   // Microsoft DMF
  {2880, 2, 240, 9, 0xF0, 36, 2},
};
// clang-format on

// -------- dates (Howard Hinnant's civil-from-days algorithms)
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
void CivilFromDays(int64_t z, int64_t &y, unsigned &m, unsigned &d)
{
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}
const int64_t kUnixEpochSecs = 11644473600LL; // 1601-01-01 .. 1970-01-01
const uint64_t kTicksPerSec = 10000000;

uint8_t LfnChecksum(const uint8_t *name11)
{
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
  return sum;
}

const uint8_t kLfnOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

bool IsShortChar(char16_t c)
{
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= 'a' && c <= 'z') return true;
  if (c >= '0' && c <= '9') return true;
  return c < 0x80 && std::strchr("!#$%&'()-@^_`{}~", (int)c) != nullptr && c != 0;
}

} // namespace

// ================================================================ geometry

bool GeometryForSizeKB(uint32_t sizeKB, Geometry &g)
{
  g = Geometry();
  for (const StdFormat &f : kStdFormats) {
    if (f.kb != sizeKB) continue;
    g.totalSectors = f.kb * 2;
    g.sectorsPerCluster = (uint8_t)f.spc;
    g.rootEntries = f.root;
    g.sectorsPerFat = f.spf;
    g.media = f.media;
    g.sectorsPerTrack = f.spt;
    g.numHeads = f.heads;
    return true;
  }
  // Generic layout for non-standard sizes.
  if (sizeKB < 64 || sizeKB > 128 * 1024) return false;
  g.totalSectors = sizeKB * 2;
  g.rootEntries = sizeKB > 2880 ? 512 : 224;
  g.media = 0xF8;
  g.sectorsPerTrack = 32;
  g.numHeads = 2;
  for (unsigned spc = 1; spc <= 64; spc <<= 1) {
    g.sectorsPerCluster = (uint8_t)spc;
    uint32_t spf = 1;
    for (int iter = 0; iter < 16; iter++) {
      g.sectorsPerFat = (uint16_t)spf;
      uint32_t clusters = g.DataClusters();
      uint32_t need = (((clusters + 2) * 3 + 1) / 2 + 511) / 512;
      if (need <= spf) break;
      spf = need;
    }
    if (g.DataClusters() <= kMaxFat12Clusters && g.DataClusters() > 0) return true;
  }
  return false;
}

bool ParseSizeSpec(const std::u16string &spec, uint32_t &kb)
{
  std::string s;
  for (char16_t c : spec) {
    if (c == u' ') continue;
    if (c >= 0x80) return false;
    char ch = (char)c;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    s += ch == ',' ? '.' : ch;
  }
  if (s == "dmf") {
    kb = 1680;
    return true;
  }
  int unit = 0; // 0 = auto, 1 = KiB, 2 = MB
  auto strip = [&](const char *suf, int u) {
    size_t n = std::strlen(suf);
    if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) {
      s.resize(s.size() - n);
      unit = u;
      return true;
    }
    return false;
  };
  if (!strip("kib", 1) && !strip("kb", 1) && !strip("k", 1) && !strip("mib", 2) && !strip("mb", 2))
    strip("m", 2);
  uint64_t ip = 0, frac = 0;
  int fracDigits = 0;
  bool dot = false, any = false;
  for (char ch : s) {
    if (ch == '.') {
      if (dot) return false;
      dot = true;
      continue;
    }
    if (ch < '0' || ch > '9') return false;
    any = true;
    if (!dot) {
      ip = ip * 10 + (uint64_t)(ch - '0');
      if (ip > 1000000) return false;
    }
    else if (fracDigits < 3) {
      frac = frac * 10 + (uint64_t)(ch - '0');
      fracDigits++;
    }
  }
  if (!any) return false;
  while (fracDigits < 3) {
    frac *= 10;
    fracDigits++;
  }
  if (unit == 0) unit = (dot || ip < 100) ? 2 : 1;
  uint64_t v = unit == 1 ? ip : ip * 1000 + frac;
  if (v == 0 || v > 0xFFFFFFFFu) return false;
  kb = (uint32_t)v;
  return true;
}

// ================================================================ times

uint64_t DosToTicks(uint16_t date, uint16_t time, uint8_t tenths)
{
  unsigned day = date & 31, month = (date >> 5) & 15;
  int64_t year = 1980 + (date >> 9);
  if (day == 0 || month == 0 || month > 12) return 0;
  unsigned sec = (time & 31) * 2, min = (time >> 5) & 63, hour = time >> 11;
  if (hour > 23 || min > 59 || sec > 59) {
    hour = min = sec = 0;
  }
  int64_t secs = DaysFromCivil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
  uint64_t t = (uint64_t)(secs + kUnixEpochSecs) * kTicksPerSec;
  if (tenths < 200) t += (uint64_t)tenths * 100000; // 10 ms units
  return t;
}

void TicksToDos(uint64_t ticks, uint16_t &date, uint16_t &time, uint8_t &tenths)
{
  int64_t secs = (int64_t)(ticks / kTicksPerSec) - kUnixEpochSecs;
  const int64_t minSecs = DaysFromCivil(1980, 1, 1) * 86400;
  const int64_t maxSecs = DaysFromCivil(2107, 12, 31) * 86400 + 86399;
  unsigned ms10 = (unsigned)((ticks % kTicksPerSec) / 100000);
  if (secs < minSecs) {
    secs = minSecs;
    ms10 = 0;
  }
  if (secs > maxSecs) {
    secs = maxSecs;
    ms10 = 0;
  }
  int64_t days = secs / 86400, rem = secs % 86400;
  int64_t y;
  unsigned m, d;
  CivilFromDays(days, y, m, d);
  unsigned hour = (unsigned)(rem / 3600), min = (unsigned)(rem / 60 % 60), sec = (unsigned)(rem % 60);
  date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
  time = (uint16_t)((hour << 11) | (min << 5) | (sec / 2));
  tenths = (uint8_t)((sec & 1) * 100 + ms10);
}

// ================================================================ reading

unsigned IsArc(const uint8_t *p, size_t size)
{
  if (size < 512) return 2;
  if (Get16(p + 510) != 0xAA55) return 0;
  if (p[0] != 0xEB && p[0] != 0xE9) return 0;
  Geometry g;
  if (!ParseBpb(p, g)) return 0;
  uint32_t c = g.DataClusters();
  if (c == 0 || c > kMaxFat12Clusters) return 0;
  // A FAT12 FAT needs 1.5 bytes per cluster.
  if ((uint64_t)g.sectorsPerFat * g.bytesPerSector * 2 / 3 < c + 2) return 0;
  return 1;
}

bool Image::Open(std::vector<uint8_t> data, std::string *err)
{
  data_ = std::move(data);
  items_.clear();
  label_.clear();
  serial_ = 0;
  warning_ = false;

  if (data_.size() < 512) {
    SetErr(err, "file too small");
    return false;
  }
  const uint8_t *p = data_.data(); // refreshed after resize below
  bool ok = ParseBpb(p, geo_);
  if (ok) {
    if (p[38] == 0x29) {
      serial_ = Get32(p + 39);
    }
  }
  else {
    // DOS 1.x disks have no BPB: identify them by size and the media byte
    // stored at the start of the FAT.
    for (const StdFormat &f : kStdFormats) {
      if (f.kb > 360 || (uint64_t)f.kb * 1024 != data_.size()) continue;
      if (p[512] == f.media && p[513] == 0xFF && p[514] == 0xFF) {
        GeometryForSizeKB(f.kb, geo_);
        ok = true;
        break;
      }
    }
  }
  if (!ok) {
    SetErr(err, "no valid FAT boot sector");
    return false;
  }

  const uint32_t clusters = geo_.DataClusters();
  if (clusters == 0) {
    SetErr(err, "no data area");
    return false;
  }
  if (clusters > kMaxFat12Clusters) {
    SetErr(err, "FAT16/FAT32 volume, not FAT12");
    return false;
  }
  if ((uint64_t)geo_.sectorsPerFat * geo_.bytesPerSector * 2 / 3 < clusters + 2) {
    SetErr(err, "FAT is too small for the volume");
    return false;
  }

  const uint64_t needHeaders = (uint64_t)geo_.FirstDataSector() * geo_.bytesPerSector;
  if (data_.size() < needHeaders) {
    SetErr(err, "image is truncated");
    return false;
  }
  if (data_.size() < geo_.ImageBytes()) {
    warning_ = true; // truncated data area: pad so reads stay in bounds
    data_.resize((size_t)geo_.ImageBytes(), 0);
    p = data_.data(); // resize may have moved the buffer
  }

  const uint8_t *root = data_.data() + (size_t)geo_.FirstRootSector() * geo_.bytesPerSector;
  ParseDir(root, geo_.rootEntries, -1, std::u16string(), 0);

  if (label_.empty() && ok && p[38] == 0x29) {
    std::u16string l;
    for (int i = 0; i < 11; i++) l += OemToUnicode(p[43 + i]);
    while (!l.empty() && l.back() == u' ') l.pop_back();
    if (l != u"NO NAME") label_ = l;
  }
  return true;
}

uint16_t Image::FatEntry(uint32_t cluster) const
{
  const size_t fatBytes = (size_t)geo_.sectorsPerFat * geo_.bytesPerSector;
  const size_t off = cluster + cluster / 2;
  if (off + 1 >= fatBytes) return 0xFF7;
  const uint8_t *fat = data_.data() + (size_t)geo_.FirstFatSector() * geo_.bytesPerSector;
  uint16_t v = Get16(fat + off);
  return (cluster & 1) ? (uint16_t)(v >> 4) : (uint16_t)(v & 0xFFF);
}

uint64_t Image::FreeBytes() const
{
  uint64_t n = 0;
  const uint32_t clusters = geo_.DataClusters();
  for (uint32_t c = 2; c < clusters + 2; c++)
    if (FatEntry(c) == 0) n++;
  return n * geo_.ClusterBytes();
}

bool Image::ReadChain(uint16_t first, uint64_t maxBytes, std::vector<uint8_t> &out) const
{
  out.clear();
  const uint32_t clusters = geo_.DataClusters();
  const uint32_t cb = geo_.ClusterBytes();
  uint32_t c = first;
  for (uint32_t steps = 0; out.size() < maxBytes; steps++) {
    if (c < 2 || c >= clusters + 2 || steps >= clusters) return false;
    const size_t off =
        ((size_t)geo_.FirstDataSector() + (size_t)(c - 2) * geo_.sectorsPerCluster) * geo_.bytesPerSector;
    size_t n = (size_t)std::min<uint64_t>(cb, maxBytes - out.size());
    out.insert(out.end(), data_.begin() + off, data_.begin() + off + n);
    uint16_t next = FatEntry(c);
    if (next >= 0xFF8) return true; // end of chain
    if (next == 0xFF7 || next < 2) return out.size() >= maxBytes && next != 0xFF7;
    c = next;
  }
  return true;
}

bool Image::ReadFile(size_t index, std::vector<uint8_t> &out) const
{
  out.clear();
  if (index >= items_.size()) return false;
  const Entry &e = items_[index];
  if (e.IsDir() || e.size == 0) return true;
  bool ok = ReadChain(e.firstCluster, e.size, out);
  return ok && out.size() == e.size;
}

void Image::ParseDir(const uint8_t *p, size_t numEntries, int parent, const std::u16string &prefix,
                     int depth)
{
  std::u16string lfn(20 * 13, u'\0');
  int lfnNext = 0; // next expected LFN sequence number (counts down)
  bool lfnValid = false;
  uint8_t lfnSum = 0;

  for (size_t i = 0; i < numEntries; i++) {
    const uint8_t *e = p + i * 32;
    if (e[0] == 0) break;
    if (e[0] == 0xE5) {
      lfnValid = false;
      continue;
    }
    const uint8_t attr = e[11];

    if ((attr & 0x3F) == kAttrLfn) {
      const unsigned seq = e[0] & 0x1F;
      if (e[0] & 0x40) {
        lfnValid = seq >= 1 && seq <= 20;
        lfnNext = (int)seq;
        lfnSum = e[13];
        std::fill(lfn.begin(), lfn.end(), u'\0');
      }
      else if (!lfnValid || (int)seq != lfnNext || e[13] != lfnSum) {
        lfnValid = false;
      }
      if (lfnValid && (int)seq == lfnNext) {
        for (int j = 0; j < 13; j++) lfn[(seq - 1) * 13 + j] = (char16_t)Get16(e + kLfnOffsets[j]);
        lfnNext--;
      }
      continue;
    }

    const bool haveLfn = lfnValid && lfnNext == 0 && LfnChecksum(e) == lfnSum;
    lfnValid = false;

    if (attr & kAttrVolume) {
      if (parent < 0 && label_.empty()) {
        for (int k = 0; k < 11; k++) label_ += OemToUnicode(e[k]);
        while (!label_.empty() && label_.back() == u' ') label_.pop_back();
      }
      continue;
    }
    if (e[0] == '.') continue; // "." and ".."

    // Short name, with NT case flags applied.
    std::u16string base, ext;
    for (int k = 0; k < 8; k++) {
      uint8_t c = (k == 0 && e[0] == 0x05) ? 0xE5 : e[k];
      char16_t u = OemToUnicode(c);
      if ((e[12] & 0x08) && u >= 'A' && u <= 'Z') u = (char16_t)(u + 32);
      base += u;
    }
    for (int k = 8; k < 11; k++) {
      char16_t u = OemToUnicode(e[k]);
      if ((e[12] & 0x10) && u >= 'A' && u <= 'Z') u = (char16_t)(u + 32);
      ext += u;
    }
    while (!base.empty() && base.back() == u' ') base.pop_back();
    while (!ext.empty() && ext.back() == u' ') ext.pop_back();
    std::u16string shortName = ext.empty() ? base : base + u"." + ext;

    std::u16string name;
    if (haveLfn) {
      for (char16_t c : lfn) {
        if (c == 0 || c == 0xFFFF) break;
        name += c;
      }
    }
    if (name.empty()) name = shortName;
    if (name.empty()) name = u"_";
    for (char16_t &c : name)
      if (c == u'/' || c == u'\\' || c < 0x20) c = u'_';

    if (items_.size() >= kMaxItems) {
      warning_ = true;
      return;
    }
    Entry it;
    it.path = prefix + name;
    it.shortName = shortName;
    for (char16_t &c : it.shortName)
      if (c >= 'a' && c <= 'z') c = (char16_t)(c - 32);
    it.parent = parent;
    it.attr = attr;
    it.firstCluster = Get16(e + 26);
    it.size = (attr & kAttrDir) ? 0 : Get32(e + 28);
    it.cTenths = e[13];
    it.cTime = Get16(e + 14);
    it.cDate = Get16(e + 16);
    it.aDate = Get16(e + 18);
    it.mTime = Get16(e + 22);
    it.mDate = Get16(e + 24);
    items_.push_back(it);
    const int idx = (int)items_.size() - 1;

    if (it.IsDir()) {
      if (depth >= (int)kMaxDepth) {
        warning_ = true;
        continue;
      }
      // Guard against directory loops: an ancestor with the same cluster.
      bool loop = false;
      for (int a = parent; a >= 0; a = items_[a].parent)
        if (items_[a].firstCluster == it.firstCluster) {
          loop = true;
          break;
        }
      if (loop || it.firstCluster < 2) {
        warning_ = true;
        continue;
      }
      std::vector<uint8_t> dir;
      if (!ReadChain(it.firstCluster, 65536u * 32, dir)) warning_ = true;
      std::u16string sub = items_[idx].path + u"/";
      ParseDir(dir.data(), dir.size() / 32, idx, sub, depth + 1);
    }
  }
}

// ================================================================ writing

std::u16string MakeShortName(const std::u16string &longName, const std::vector<std::u16string> &existing,
                             bool *needLfn, uint8_t *caseFlags)
{
  *needLfn = false;
  *caseFlags = 0;
  auto exists = [&](const std::u16string &s) {
    for (const auto &x : existing)
      if (x == s) return true;
    return false;
  };
  auto pad = [](std::u16string b, std::u16string e) {
    b.resize(8, u' ');
    e.resize(3, u' ');
    return b + e;
  };

  // 1. Already a valid 8.3 name (possibly all-lowercase base and/or ext)?
  {
    const size_t dot = longName.rfind(u'.');
    std::u16string b = dot == std::u16string::npos ? longName : longName.substr(0, dot);
    std::u16string e = dot == std::u16string::npos ? std::u16string() : longName.substr(dot + 1);
    bool ok = !b.empty() && b.size() <= 8 && e.size() <= 3 && (dot == std::u16string::npos || !e.empty());
    int bCase = 0, eCase = 0; // bit0 = has upper, bit1 = has lower
    for (char16_t c : b) {
      if (!IsShortChar(c)) ok = false;
      if (c >= 'A' && c <= 'Z') bCase |= 1;
      if (c >= 'a' && c <= 'z') bCase |= 2;
    }
    for (char16_t c : e) {
      if (!IsShortChar(c)) ok = false;
      if (c >= 'A' && c <= 'Z') eCase |= 1;
      if (c >= 'a' && c <= 'z') eCase |= 2;
    }
    if (ok) {
      std::u16string s = pad(b, e);
      for (char16_t &c : s) c = Upper(c);
      if (!exists(s)) {
        if (bCase == 3 || eCase == 3) {
          *needLfn = true; // mixed case: keep exact spelling in an LFN
        }
        else {
          if (bCase == 2) *caseFlags |= 0x08;
          if (eCase == 2) *caseFlags |= 0x10;
        }
        return s;
      }
    }
  }

  // 2. Generate a basis name with a numeric tail, like Windows does.
  *needLfn = true;
  bool lossy = false;
  std::u16string name;
  for (char16_t c : longName) {
    if (c == u' ')
      lossy = true;
    else
      name += c;
  }
  size_t lead = 0;
  while (lead < name.size() && name[lead] == u'.') lead++;
  if (lead) {
    lossy = true;
    name.erase(0, lead);
  }
  const size_t dot = name.rfind(u'.');
  std::u16string rawB = dot == std::u16string::npos ? name : name.substr(0, dot);
  std::u16string rawE = dot == std::u16string::npos ? std::u16string() : name.substr(dot + 1);
  auto clean = [&](const std::u16string &in, size_t maxLen) {
    std::u16string out;
    for (char16_t c : in) {
      if (c == u'.') {
        lossy = true;
        continue;
      }
      char16_t u = Upper(c);
      if (!IsShortChar(u) || u >= 'a') { // after Upper only A-Z/digits/specials are fine
        if (!IsShortChar(u)) {
          u = u'_';
          lossy = true;
        }
      }
      if (out.size() < maxLen)
        out += u;
      else
        lossy = true;
    }
    return out;
  };
  std::u16string b = clean(rawB, 8), e = clean(rawE, 3);
  if (b.empty()) {
    b = u"_";
    lossy = true;
  }

  if (!lossy) {
    std::u16string s = pad(b, e);
    if (!exists(s)) return s;
  }
  for (unsigned n = 1; n < 1000000; n++) {
    std::u16string tail = u"~";
    for (char ch : std::to_string(n)) tail += (char16_t)ch;
    std::u16string bb = b.substr(0, std::min(b.size(), 8 - tail.size())) + tail;
    std::u16string s = pad(bb, e);
    if (!exists(s)) return s;
  }
  return pad(u"_", e); // unreachable in practice
}

namespace {

struct Node {
  std::u16string name;
  bool isDir = false;
  int item = -1; // index into items, -1 = implicit directory
  int parent = -1;
  std::vector<int> children;
  std::u16string short11;
  bool needLfn = false;
  uint8_t caseFlags = 0;
  uint32_t firstCluster = 0;
  uint32_t numClusters = 0;
  uint32_t dirEntries = 0;
};

std::u16string SanitizeComponent(std::u16string s)
{
  for (char16_t &c : s)
    if (c < 0x20 || (c < 0x80 && std::strchr("\"*:<>?|", (int)c) && c != 0)) c = u'_';
  while (!s.empty() && (s.back() == u'.' || s.back() == u' ')) s.pop_back();
  return s;
}

std::string Narrow(const std::u16string &s)
{
  std::string r;
  for (char16_t c : s) r += (c < 0x80) ? (char)c : '?';
  return r;
}

} // namespace

bool Build(const BuildOptions &opt, const std::vector<NewItem> &items, std::vector<uint8_t> &out,
           std::string *err)
{
  const Geometry &g = opt.geo;
  if (!IsValidBps(g.bytesPerSector) || !IsPow2(g.sectorsPerCluster) || g.numFats == 0 || g.rootEntries == 0 ||
      g.sectorsPerFat == 0 || g.totalSectors <= g.FirstDataSector()) {
    SetErr(err, "invalid disk geometry");
    return false;
  }
  const uint32_t clusters = g.DataClusters();
  if (clusters == 0 || clusters > kMaxFat12Clusters) {
    SetErr(err, "geometry is not FAT12");
    return false;
  }
  if ((uint64_t)g.sectorsPerFat * g.bytesPerSector * 2 / 3 < clusters + 2) {
    SetErr(err, "FAT too small for geometry");
    return false;
  }

  // ---- build the directory tree
  std::vector<Node> nodes(1);
  nodes[0].isDir = true;
  for (size_t i = 0; i < items.size(); i++) {
    std::vector<std::u16string> parts;
    std::u16string cur;
    for (char16_t c : items[i].path + u"/") {
      if (c == u'/' || c == u'\\') {
        if (!cur.empty() && cur != u".") {
          if (cur == u"..") {
            SetErr(err, "path contains '..': " + Narrow(items[i].path));
            return false;
          }
          std::u16string s = SanitizeComponent(cur);
          if (s.empty()) s = u"_";
          if (s.size() > 255) {
            SetErr(err, "file name longer than 255 chars: " + Narrow(cur));
            return false;
          }
          parts.push_back(s);
        }
        cur.clear();
      }
      else {
        cur += c;
      }
    }
    if (parts.empty()) continue;
    int dir = 0;
    for (size_t k = 0; k < parts.size(); k++) {
      const bool last = k + 1 == parts.size();
      const bool wantDir = !last || items[i].isDir;
      int found = -1;
      for (int ch : nodes[dir].children)
        if (EqualNoCase(nodes[ch].name, parts[k])) {
          found = ch;
          break;
        }
      if (found >= 0) {
        if (nodes[found].isDir != wantDir) {
          SetErr(err, "name used both as file and directory: " + Narrow(items[i].path));
          return false;
        }
        if (last) nodes[found].item = (int)i; // later duplicate wins
      }
      else {
        Node n;
        n.name = parts[k];
        n.isDir = wantDir;
        n.parent = dir;
        n.item = last ? (int)i : -1;
        nodes.push_back(n);
        found = (int)nodes.size() - 1;
        nodes[dir].children.push_back(found);
      }
      dir = found;
    }
  }

  // ---- short names and directory sizes
  const uint32_t cb = g.ClusterBytes();
  for (Node &d : nodes) {
    if (!d.isDir) continue;
    std::vector<std::u16string> used;
    // Pass 1: names that are already valid 8.3 keep their exact short name.
    std::vector<bool> done(d.children.size(), false);
    for (int pass = 0; pass < 2; pass++) {
      for (size_t k = 0; k < d.children.size(); k++) {
        if (done[k]) continue;
        Node &c = nodes[d.children[k]];
        bool lfn;
        uint8_t cf;
        std::u16string s = MakeShortName(c.name, used, &lfn, &cf);
        if (pass == 0 && lfn) continue;
        c.short11 = s;
        c.needLfn = lfn;
        c.caseFlags = cf;
        used.push_back(s);
        done[k] = true;
      }
    }
    uint32_t n = 0;
    for (int ch : d.children) n += 1 + (nodes[ch].needLfn ? (uint32_t)(nodes[ch].name.size() + 12) / 13 : 0);
    d.dirEntries = n;
  }
  nodes[0].dirEntries += opt.label.empty() ? 0 : 1;
  if (nodes[0].dirEntries > g.rootEntries) {
    SetErr(err, "root directory full: " + std::to_string(nodes[0].dirEntries) + " entries needed, " +
                    std::to_string(g.rootEntries) + " available");
    return false;
  }

  // ---- cluster allocation (contiguous, in tree order)
  uint64_t needed = 0;
  for (size_t i = 1; i < nodes.size(); i++) {
    Node &n = nodes[i];
    uint64_t bytes =
        n.isDir ? (uint64_t)(n.dirEntries + 2) * 32 : (n.item >= 0 ? items[n.item].data.size() : 0);
    n.numClusters = (uint32_t)((bytes + cb - 1) / cb);
    if (n.isDir && n.numClusters == 0) n.numClusters = 1;
    needed += n.numClusters;
  }
  if (needed > clusters) {
    SetErr(err, "disk full: need " + std::to_string(needed * cb) + " bytes, " +
                    std::to_string((uint64_t)clusters * cb) + " available");
    return false;
  }
  uint32_t next = 2;
  for (size_t i = 1; i < nodes.size(); i++) {
    if (nodes[i].numClusters == 0) continue;
    nodes[i].firstCluster = next;
    next += nodes[i].numClusters;
  }

  // ---- image
  out.assign((size_t)g.ImageBytes(), 0);
  uint8_t *img = out.data();
  const uint32_t bps = g.bytesPerSector;

  // Boot sector
  uint8_t *bs = img;
  if (opt.bootSector && opt.bootSector->size() >= 512 && (*opt.bootSector)[0] != 0) {
    std::memcpy(bs, opt.bootSector->data(), 512);
  }
  else {
    static const uint8_t kJump[3] = {0xEB, 0x3C, 0x90};
    std::memcpy(bs, kJump, 3);
    std::memcpy(bs + 3, "MSDOS5.0", 8);
    // Boot code at 0x3E: print a message, wait for a key, reboot.
    static const char kMsg[] = "\r\nNon-system disk. Press any key to reboot.\r\n";
    uint8_t code[] = {0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E,
                      0xD0, 0xBC, 0x00, 0x7C, 0xFB, 0xFC, // cli; ds=ss=0; sp=7C00; sti; cld
                      0xBE, 0x00, 0x00,                   // mov si, msg
                      0xAC, 0x08, 0xC0, 0x74, 0x09, 0xB4,
                      0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10,  // lodsb; or al,al; jz +9; teletype
                      0xEB, 0xF2,                          // jmp lodsb
                      0x30, 0xE4, 0xCD, 0x16, 0xCD, 0x19}; // xor ah,ah; int 16h; int 19h
    const uint16_t msgAddr = (uint16_t)(0x7C00 + 0x3E + sizeof(code));
    Set16(code + 13, msgAddr);
    std::memcpy(bs + 0x3E, code, sizeof(code));
    std::memcpy(bs + 0x3E + sizeof(code), kMsg, sizeof(kMsg));
  }
  Set16(bs + 11, g.bytesPerSector);
  bs[13] = g.sectorsPerCluster;
  Set16(bs + 14, g.reservedSectors);
  bs[16] = g.numFats;
  Set16(bs + 17, g.rootEntries);
  Set16(bs + 19, g.totalSectors < 65536 ? (uint16_t)g.totalSectors : 0);
  bs[21] = g.media;
  Set16(bs + 22, g.sectorsPerFat);
  Set16(bs + 24, g.sectorsPerTrack);
  Set16(bs + 26, g.numHeads);
  Set32(bs + 28, g.hiddenSectors);
  Set32(bs + 32, g.totalSectors < 65536 ? 0 : g.totalSectors);
  bs[36] = 0x00; // drive number (floppy)
  bs[37] = 0;
  bs[38] = 0x29; // extended boot signature
  Set32(bs + 39, opt.serial ? opt.serial : (uint32_t)(opt.nowTicks / kTicksPerSec * 2654435761u));
  std::u16string lab = opt.label.empty() ? u"NO NAME" : opt.label;
  for (int k = 0; k < 11; k++) bs[43 + k] = k < (int)lab.size() ? (uint8_t)lab[k] : ' ';
  std::memcpy(bs + 54, "FAT12   ", 8);
  bs[510] = 0x55;
  bs[511] = 0xAA;

  // FAT
  std::vector<uint8_t> fat((size_t)g.sectorsPerFat * bps, 0);
  auto setFat = [&](uint32_t c, uint16_t v) {
    size_t off = c + c / 2;
    uint16_t old = Get16(&fat[off]);
    if (c & 1)
      old = (uint16_t)((old & 0x000F) | (v << 4));
    else
      old = (uint16_t)((old & 0xF000) | (v & 0xFFF));
    Set16(&fat[off], old);
  };
  setFat(0, (uint16_t)(0xF00 | g.media));
  setFat(1, 0xFFF);
  for (const Node &n : nodes) {
    for (uint32_t k = 0; k < n.numClusters; k++) {
      uint32_t c = n.firstCluster + k;
      setFat(c, k + 1 == n.numClusters ? 0xFFF : (uint16_t)(c + 1));
    }
  }
  for (unsigned f = 0; f < g.numFats; f++)
    std::memcpy(img + (size_t)(g.FirstFatSector() + f * g.sectorsPerFat) * bps, fat.data(), fat.size());

  // Directory entries
  auto times = [&](const Node &n, uint16_t &md, uint16_t &mt, uint16_t &cd, uint16_t &ct, uint8_t &cten,
                   uint16_t &ad) {
    uint64_t m = opt.nowTicks, c = 0, a = 0;
    if (n.item >= 0) {
      const NewItem &it = items[n.item];
      if (it.hasMTime) m = it.mTicks;
      c = it.hasCTime ? it.cTicks : m;
      a = it.hasATime ? it.aTicks : m;
    }
    else {
      c = a = m;
    }
    uint8_t dummy;
    TicksToDos(m, md, mt, dummy);
    TicksToDos(c, cd, ct, cten);
    uint16_t at;
    TicksToDos(a, ad, at, dummy);
  };
  auto writeShort = [&](uint8_t *e, const uint8_t *name11, uint8_t attr, uint8_t caseFlags, const Node &n,
                        uint32_t cluster, uint32_t size) {
    std::memcpy(e, name11, 11);
    e[11] = attr;
    e[12] = caseFlags;
    uint16_t md, mt, cd, ct, ad;
    uint8_t cten;
    times(n, md, mt, cd, ct, cten, ad);
    e[13] = cten;
    Set16(e + 14, ct);
    Set16(e + 16, cd);
    Set16(e + 18, ad);
    Set16(e + 20, 0);
    Set16(e + 22, mt);
    Set16(e + 24, md);
    Set16(e + 26, (uint16_t)cluster);
    Set32(e + 28, size);
  };

  for (size_t di = 0; di < nodes.size(); di++) {
    const Node &d = nodes[di];
    if (!d.isDir) continue;
    std::vector<uint8_t> buf;
    if (di == 0)
      buf.assign((size_t)g.rootEntries * 32, 0);
    else
      buf.assign((size_t)d.numClusters * cb, 0);
    size_t pos = 0;
    if (di == 0 && !opt.label.empty()) {
      uint8_t name[11];
      for (int k = 0; k < 11; k++) name[k] = k < (int)opt.label.size() ? (uint8_t)opt.label[k] : ' ';
      writeShort(&buf[pos], name, kAttrVolume, 0, d, 0, 0);
      pos += 32;
    }
    if (di != 0) {
      uint8_t dot[11], dotdot[11];
      std::memset(dot, ' ', 11);
      std::memset(dotdot, ' ', 11);
      dot[0] = '.';
      dotdot[0] = dotdot[1] = '.';
      writeShort(&buf[pos], dot, kAttrDir, 0, d, d.firstCluster, 0);
      pos += 32;
      writeShort(&buf[pos], dotdot, kAttrDir, 0, d, d.parent > 0 ? nodes[d.parent].firstCluster : 0, 0);
      pos += 32;
    }
    for (int ch : d.children) {
      const Node &c = nodes[ch];
      uint8_t name11[11];
      for (int k = 0; k < 11; k++) name11[k] = (uint8_t)c.short11[k];
      if (c.needLfn) {
        const uint8_t sum = LfnChecksum(name11);
        const unsigned cnt = (unsigned)(c.name.size() + 12) / 13;
        for (unsigned k = cnt; k >= 1; k--) {
          uint8_t *e = &buf[pos];
          e[0] = (uint8_t)(k | (k == cnt ? 0x40 : 0));
          e[11] = kAttrLfn;
          e[12] = 0;
          e[13] = sum;
          Set16(e + 26, 0);
          for (unsigned j = 0; j < 13; j++) {
            size_t idx = (k - 1) * 13 + j;
            uint16_t v = idx < c.name.size() ? (uint16_t)c.name[idx] : (idx == c.name.size() ? 0 : 0xFFFF);
            Set16(e + kLfnOffsets[j], v);
          }
          pos += 32;
        }
      }
      uint8_t attr = 0;
      uint32_t size = 0;
      if (c.item >= 0) {
        attr = items[c.item].attr & (kAttrReadOnly | kAttrHidden | kAttrSystem | kAttrArchive);
        if (!c.isDir) size = (uint32_t)items[c.item].data.size();
      }
      else if (!c.isDir) {
        attr = kAttrArchive;
      }
      if (c.isDir) attr |= kAttrDir;
      writeShort(&buf[pos], name11, attr, c.caseFlags, c, c.firstCluster, size);
      pos += 32;
    }
    uint8_t *dst =
        di == 0
            ? img + (size_t)g.FirstRootSector() * bps
            : img + ((size_t)g.FirstDataSector() + (size_t)(d.firstCluster - 2) * g.sectorsPerCluster) * bps;
    std::memcpy(dst, buf.data(), buf.size());
  }

  // File data
  for (const Node &n : nodes) {
    if (n.isDir || n.item < 0 || n.numClusters == 0) continue;
    const std::vector<uint8_t> &data = items[n.item].data;
    uint8_t *dst =
        img + ((size_t)g.FirstDataSector() + (size_t)(n.firstCluster - 2) * g.sectorsPerCluster) * bps;
    std::memcpy(dst, data.data(), data.size());
  }
  return true;
}

} // namespace fat12
