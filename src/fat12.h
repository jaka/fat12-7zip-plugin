// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 jaka
// fat12.h - portable FAT12 floppy image reader/writer (no OS dependencies).
//
// Used by the 7-Zip plugin (plugin.cpp) and by the Linux test harness.
// Paths use '/' as separator internally and are UTF-16 (std::u16string).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fat12 {

// ---------------------------------------------------------------- geometry

struct Geometry {
  uint16_t bytesPerSector = 512;
  uint8_t sectorsPerCluster = 1;
  uint16_t reservedSectors = 1;
  uint8_t numFats = 2;
  uint16_t rootEntries = 224;
  uint32_t totalSectors = 2880;
  uint8_t media = 0xF0;
  uint16_t sectorsPerFat = 9;
  uint16_t sectorsPerTrack = 18;
  uint16_t numHeads = 2;
  uint32_t hiddenSectors = 0;

  uint32_t RootDirSectors() const
  {
    return (rootEntries * 32u + bytesPerSector - 1) / bytesPerSector;
  }
  uint32_t FirstFatSector() const
  {
    return reservedSectors;
  }
  uint32_t FirstRootSector() const
  {
    return reservedSectors + numFats * sectorsPerFat;
  }
  uint32_t FirstDataSector() const
  {
    return FirstRootSector() + RootDirSectors();
  }
  uint32_t ClusterBytes() const
  {
    return (uint32_t)bytesPerSector * sectorsPerCluster;
  }
  uint32_t DataClusters() const
  {
    if (totalSectors <= FirstDataSector()) return 0;
    return (totalSectors - FirstDataSector()) / sectorsPerCluster;
  }
  uint64_t ImageBytes() const
  {
    return (uint64_t)totalSectors * bytesPerSector;
  }
};

// Standard PC floppy formats, by size in KiB: 160, 180, 320, 360, 720,
// 1200, 1440, 1680 (DMF), 2880.  Other sizes (up to ~32 MiB) get a computed
// generic FAT12 layout.  Returns false if the size cannot hold FAT12.
bool GeometryForSizeKB(uint32_t sizeKB, Geometry &g);

// Parses a user size spec into KiB: "1.44", "1,44", "1.44m", "1.2M", "2.88",
// "720", "720k", "360KB", "dmf".  Floppy "megabytes" are 1000 KiB, so
// 1.44 -> 1440.  Plain integers >= 100 are KiB, smaller ones are MB.
bool ParseSizeSpec(const std::u16string &spec, uint32_t &kb);

// ---------------------------------------------------------------- times

// DOS timestamps are local time without a zone.  These helpers convert
// between DOS date/time and a FILETIME-style tick count (100 ns since
// 1601-01-01) *in the same (local) time base*; the caller converts to UTC.
uint64_t DosToTicks(uint16_t date, uint16_t time, uint8_t tenthsOf10ms = 0);
void TicksToDos(uint64_t ticks, uint16_t &date, uint16_t &time, uint8_t &tenths);

// ---------------------------------------------------------------- reading

enum Attr : uint8_t {
  kAttrReadOnly = 0x01,
  kAttrHidden = 0x02,
  kAttrSystem = 0x04,
  kAttrVolume = 0x08,
  kAttrDir = 0x10,
  kAttrArchive = 0x20,
  kAttrLfn = 0x0F
};

struct Entry {
  std::u16string path;      // full path, '/' separated
  std::u16string shortName; // 8.3 name as stored ("README.TXT")
  int parent = -1;          // index of parent dir entry, -1 = root
  uint8_t attr = 0;
  uint32_t size = 0;
  uint16_t firstCluster = 0;
  uint16_t cDate = 0, cTime = 0;
  uint8_t cTenths = 0;
  uint16_t aDate = 0;
  uint16_t mDate = 0, mTime = 0;
  bool IsDir() const
  {
    return (attr & kAttrDir) != 0;
  }
};

class Image {
public:
  // Parses the image.  On failure returns false and fills *err.
  // Images whose cluster count exceeds the FAT12 limit are rejected.
  bool Open(std::vector<uint8_t> data, std::string *err = nullptr);

  const Geometry &Geo() const
  {
    return geo_;
  }
  const std::vector<Entry> &Items() const
  {
    return items_;
  }
  const std::u16string &VolumeLabel() const
  {
    return label_;
  }
  uint32_t VolumeSerial() const
  {
    return serial_;
  }
  uint64_t FreeBytes() const;
  bool HeadersWarning() const
  {
    return warning_;
  } // non-fatal structural damage
  // Part of the directory tree could not be read (broken, looped or
  // cross-linked folders, or a parse limit hit), so files may be missing from
  // Items().  Rebuilding the image from Items() would drop them.
  bool ListingIncomplete() const
  {
    return incomplete_;
  }

  // Reads the contents of file item `index`.  Returns false on a broken
  // cluster chain (data read so far is still returned in `out`).
  bool ReadFile(size_t index, std::vector<uint8_t> &out) const;

  // Raw access, used when copying the boot sector on update.
  const std::vector<uint8_t> &Raw() const
  {
    return data_;
  }

private:
  uint16_t FatEntry(uint32_t cluster) const;
  bool ReadChain(uint16_t first, uint64_t maxBytes, std::vector<uint8_t> &out) const;
  void Incomplete()
  {
    warning_ = incomplete_ = true;
  }
  void ParseDir(const uint8_t *p, size_t numEntries, int parent, const std::u16string &prefix, int depth);

  std::vector<uint8_t> data_;
  Geometry geo_;
  std::vector<Entry> items_;
  std::u16string label_;
  uint32_t serial_ = 0;
  bool warning_ = false;
  bool incomplete_ = false; // implies warning_
  // Parse budgets: keep work and memory proportional to the input size even
  // for hostile images (cross-linked directories, deep long paths).
  std::vector<bool> dirSeen_;  // directory first clusters already parsed
  uint64_t dirBytesLeft_ = 0;  // directory bytes we may still parse
  uint64_t pathCharsLeft_ = 0; // path characters we may still store
};

// Quick check for 7-Zip signature probing.  0 = no, 1 = yes, 2 = need more.
unsigned IsArc(const uint8_t *p, size_t size);

// ---------------------------------------------------------------- writing

struct NewItem {
  std::u16string path; // '/' or '\\' separated; parents are auto-created
  bool isDir = false;
  uint8_t attr = 0; // RO/hidden/system/archive bits; dir bit is implied
  bool hasMTime = false, hasCTime = false, hasATime = false;
  uint64_t mTicks = 0, cTicks = 0, aTicks = 0; // local-time ticks, see above
  std::vector<uint8_t> data;
};

struct BuildOptions {
  Geometry geo;
  std::u16string label; // empty = no volume label
  uint32_t serial = 0;
  uint64_t nowTicks = 0;                            // used for items without times
  const std::vector<uint8_t> *bootSector = nullptr; // keep boot code from old image
};

// Builds a complete image.  Returns false and sets *err when the items do not
// fit (disk full, root directory full, bad names, ...).
bool Build(const BuildOptions &opt, const std::vector<NewItem> &items, std::vector<uint8_t> &out,
           std::string *err);

// Exposed for tests.
std::u16string MakeShortName(const std::u16string &longName, const std::vector<std::u16string> &existing,
                             bool *needLfn, uint8_t *caseFlags);

} // namespace fat12
