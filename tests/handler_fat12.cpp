// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 jaka
// handler_fat12.cpp - drives the plugin (fat12.so) the way 7-Zip's File
// Manager does, through 7-Zip's own interface headers.
//
// The 7z command line creates a fresh handler for every update
// (CreateObject with IID_IOutArchive).  The File Manager instead keeps the
// handler that opened the image, asks *it* for IOutArchive, calls
// SetProperties(NULL, NULL, 0), runs UpdateItems with the old items mapped
// by index, then Close()s and re-Open()s the same object.  None of that is
// covered by run_plugin_tests.sh, so this test does it directly.  Using
// 7-Zip's headers (not src/7z_iface.h) also checks that our copy of the ABI
// matches.
//
// Build (see build_and_test_linux.sh):
//   g++ -std=c++17 -I<7zip-src>/CPP tests/handler_fat12.cpp src/fat12.cpp
//       <7zip-src>/CPP/Common/MyWindows.cpp -ldl
// With UBSan, add -fno-sanitize=vptr: the plugin declares its own copy of the
// interfaces (src/7z_iface.h), so their RTTI types differ from 7-Zip's even
// though the vtables match, which is all COM needs.
// Run:   handler_fat12 path/to/fat12.so
#include "Common/MyInitGuid.h" // defines the IIDs below in this file

#include "../src/fat12.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/PropID.h"
#include "Common/MyCom.h"
#include "Common/MyWindows.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const std::string &what)
{
  std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what.c_str());
  if (!cond) g_failures++;
}

std::string Narrow(const std::u16string &s)
{
  return std::string(s.begin(), s.end());
}
std::wstring Wide(const std::string &s)
{
  return std::wstring(s.begin(), s.end());
}

// ------------------------------------------------------------ COM objects

Z7_CLASS_IMP_IInStream(CMemInStream)
    std::vector<uint8_t> data_;
UInt64 pos_ = 0;

public:
explicit CMemInStream(std::vector<uint8_t> d) : data_(std::move(d))
{
}
};

Z7_COM7F_IMF(CMemInStream::Read(void *data, UInt32 size, UInt32 *processed))
{
  const UInt64 left = pos_ < data_.size() ? data_.size() - pos_ : 0;
  const UInt32 n = (UInt32)(size < left ? size : left);
  if (n) std::memcpy(data, data_.data() + pos_, n);
  pos_ += n;
  if (processed) *processed = n;
  return S_OK;
}

Z7_COM7F_IMF(CMemInStream::Seek(Int64 offset, UInt32 origin, UInt64 *newPos))
{
  Int64 base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? (Int64)pos_
                                                                         : (Int64)data_.size();
  if (origin > STREAM_SEEK_END || base + offset < 0) return STG_E_INVALIDFUNCTION;
  pos_ = (UInt64)(base + offset);
  if (newPos) *newPos = pos_;
  return S_OK;
}

Z7_CLASS_IMP_COM_1(CMemOutStream, ISequentialOutStream)
public:
std::vector<uint8_t> data;
}
;

Z7_COM7F_IMF(CMemOutStream::Write(const void *p, UInt32 size, UInt32 *processed))
{
  data.insert(data.end(), (const uint8_t *)p, (const uint8_t *)p + size);
  if (processed) *processed = size;
  return S_OK;
}

Z7_CLASS_IMP_COM_1(COpenCallback, IArchiveOpenCallback)
}
;

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *, const UInt64 *))
{
  return S_OK;
}
Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *, const UInt64 *))
{
  return S_OK;
}

// One output item, as 7-Zip's CArchiveUpdateCallback describes it.
struct Op {
  bool newData = false, newProps = false;
  UInt32 oldIndex = (UInt32)-1;
  std::string path; // for newProps
  bool isDir = false;
  std::string data; // for newData
};

Op Keep(UInt32 old)
{
  Op o;
  o.oldIndex = old;
  return o;
}
Op Rename(UInt32 old, const std::string &path)
{
  Op o;
  o.newProps = true;
  o.oldIndex = old;
  o.path = path;
  return o;
}
// Edit in the File Manager: new data, same index and name (7-Zip's
// CAgent::UpdateOneFile).
Op Edit(UInt32 old, const std::string &path, const std::string &data)
{
  Op o;
  o.newData = o.newProps = true;
  o.oldIndex = old;
  o.path = path;
  o.data = data;
  return o;
}
Op AddFile(const std::string &path, const std::string &data)
{
  Op o;
  o.newData = o.newProps = true;
  o.path = path;
  o.data = data;
  return o;
}
Op AddDir(const std::string &path)
{
  Op o;
  o.newData = o.newProps = true;
  o.path = path;
  o.isDir = true;
  return o;
}

Z7_CLASS_IMP_COM_1(CSeqInStream, ISequentialInStream)
std::string data_;
size_t pos_ = 0;

public:
explicit CSeqInStream(std::string d) : data_(std::move(d))
{
}
}
;

Z7_COM7F_IMF(CSeqInStream::Read(void *data, UInt32 size, UInt32 *processed))
{
  const size_t n = std::min<size_t>(size, data_.size() - pos_);
  std::memcpy(data, data_.data() + pos_, n);
  pos_ += n;
  if (processed) *processed = (UInt32)n;
  return S_OK;
}

class CUpdateCallback Z7_final : public IArchiveUpdateCallback, public CMyUnknownImp {
  Z7_COM_UNKNOWN_IMP_1(IArchiveUpdateCallback)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveUpdateCallback)
public:
  std::vector<Op> ops;
};

Z7_COM7F_IMF(CUpdateCallback::SetTotal(UInt64))
{
  return S_OK;
}
Z7_COM7F_IMF(CUpdateCallback::SetCompleted(const UInt64 *))
{
  return S_OK;
}
Z7_COM7F_IMF(CUpdateCallback::GetUpdateItemInfo(UInt32 i, Int32 *newData, Int32 *newProps, UInt32 *old))
{
  if (i >= ops.size()) return E_INVALIDARG;
  if (newData) *newData = ops[i].newData;
  if (newProps) *newProps = ops[i].newProps;
  if (old) *old = ops[i].oldIndex;
  return S_OK;
}
Z7_COM7F_IMF(CUpdateCallback::GetProperty(UInt32 i, PROPID id, PROPVARIANT *v))
{
  if (i >= ops.size()) return E_INVALIDARG;
  const Op &o = ops[i];
  v->vt = VT_EMPTY;
  switch (id) {
    case kpidPath:
      v->bstrVal = ::SysAllocString(Wide(o.path).c_str());
      v->vt = VT_BSTR;
      break;
    case kpidIsDir:
      v->vt = VT_BOOL;
      v->boolVal = o.isDir ? VARIANT_TRUE : VARIANT_FALSE;
      break;
    case kpidIsAnti:
      v->vt = VT_BOOL;
      v->boolVal = VARIANT_FALSE;
      break;
    case kpidSize:
      v->vt = VT_UI8;
      v->uhVal.QuadPart = o.data.size();
      break;
    case kpidAttrib:
      v->vt = VT_UI4;
      v->ulVal = o.isDir ? 0x10 : 0x20;
      break;
    case kpidMTime: // 2020-06-15 12:00:00 UTC
      v->vt = VT_FILETIME;
      v->filetime.dwLowDateTime = 0x4F6B0000u;
      v->filetime.dwHighDateTime = 0x01D64306u;
      break;
    default:
      break;
  }
  return S_OK;
}
Z7_COM7F_IMF(CUpdateCallback::GetStream(UInt32 i, ISequentialInStream **s))
{
  *s = nullptr;
  if (i >= ops.size() || !ops[i].newData) return E_INVALIDARG;
  if (ops[i].isDir) return S_OK;
  CMyComPtr<ISequentialInStream> p = new CSeqInStream(ops[i].data);
  *s = p.Detach();
  return S_OK;
}
Z7_COM7F_IMF(CUpdateCallback::SetOperationResult(Int32))
{
  return S_OK;
}

// ------------------------------------------------------------ plugin access

typedef HRESULT (*CreateObjectFn)(const GUID *clsid, const GUID *iid, void **out);
typedef HRESULT (*GetHandlerProperty2Fn)(UInt32 index, PROPID id, PROPVARIANT *v);

CreateObjectFn g_createObject;
GUID g_clsid;

bool LoadPlugin(const char *path)
{
  void *lib = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "dlopen %s: %s\n", path, ::dlerror());
    return false;
  }
  g_createObject = (CreateObjectFn)::dlsym(lib, "CreateObject");
  auto getProp = (GetHandlerProperty2Fn)::dlsym(lib, "GetHandlerProperty2");
  if (!g_createObject || !getProp) return false;
  PROPVARIANT v;
  v.vt = VT_EMPTY;
  if (getProp(0, NArchive::NHandlerPropID::kClassID, &v) != S_OK || v.vt != VT_BSTR ||
      ::SysStringByteLen(v.bstrVal) != sizeof(GUID))
    return false;
  std::memcpy(&g_clsid, v.bstrVal, sizeof(GUID));
  ::SysFreeString(v.bstrVal);
  return true;
}

// Paths of all items of an open handler, by index.
std::vector<std::string> ListPaths(IInArchive *arc)
{
  std::vector<std::string> paths;
  UInt32 n = 0;
  if (arc->GetNumberOfItems(&n) != S_OK) return paths;
  for (UInt32 i = 0; i < n; i++) {
    PROPVARIANT v;
    v.vt = VT_EMPTY;
    std::string p;
    if (arc->GetProperty(i, kpidPath, &v) == S_OK && v.vt == VT_BSTR) {
      for (const wchar_t *c = v.bstrVal; *c; c++) p += (char)(*c == L'\\' ? '/' : *c);
      ::SysFreeString(v.bstrVal);
    }
    paths.push_back(p);
  }
  return paths;
}

UInt32 IndexOf(const std::vector<std::string> &paths, const std::string &p)
{
  for (size_t i = 0; i < paths.size(); i++)
    if (paths[i] == p) return (UInt32)i;
  return (UInt32)-1;
}

// The File Manager sequence on an already open handler.  Returns the
// UpdateItems result; the new image is in `out`.
HRESULT FmUpdate(IInArchive *arc, const std::vector<Op> &ops, std::vector<uint8_t> &out)
{
  CMyComPtr<IOutArchive> outArc;
  HRESULT hr = arc->QueryInterface(IID_IOutArchive, (void **)&outArc);
  if (hr != S_OK || !outArc) return hr == S_OK ? E_NOINTERFACE : hr;
  UInt32 timeType = 99;
  if ((hr = outArc->GetFileTimeType(&timeType)) != S_OK) return hr;
  CMyComPtr<ISetProperties> setProps;
  outArc.QueryInterface(IID_ISetProperties, &setProps);
  if (setProps && (hr = setProps->SetProperties(NULL, NULL, 0)) != S_OK) return hr;
  CUpdateCallback *cbSpec = new CUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> cb = cbSpec;
  cbSpec->ops = ops;
  CMemOutStream *osSpec = new CMemOutStream;
  CMyComPtr<ISequentialOutStream> os = osSpec;
  hr = outArc->UpdateItems(os, (UInt32)ops.size(), cb);
  out = osSpec->data;
  return hr;
}

HRESULT Reopen(IInArchive *arc, const std::vector<uint8_t> &img)
{
  arc->Close();
  CMyComPtr<IInStream> s = new CMemInStream(img);
  CMyComPtr<IArchiveOpenCallback> cb = new COpenCallback;
  const UInt64 maxCheck = 1 << 20;
  return arc->Open(s, &maxCheck, cb);
}

// Contents of an image as path -> data ("<dir>" for folders), read by the
// core library; also checks it is clean.
std::map<std::string, std::string> Contents(const std::vector<uint8_t> &img, bool &clean, std::u16string *label)
{
  std::map<std::string, std::string> m;
  fat12::Image im;
  clean = im.Open(img) && !im.HeadersWarning();
  if (label) *label = im.VolumeLabel();
  for (size_t i = 0; i < im.Items().size(); i++) {
    const fat12::Entry &e = im.Items()[i];
    std::vector<uint8_t> d;
    if (!e.IsDir() && !im.ReadFile(i, d)) clean = false;
    m[Narrow(e.path)] = e.IsDir() ? "<dir>" : std::string(d.begin(), d.end());
  }
  return m;
}

std::string Describe(const std::map<std::string, std::string> &m)
{
  std::string s;
  for (const auto &kv : m) s += (s.empty() ? "" : ", ") + kv.first + (kv.second == "<dir>" ? "/" : "");
  return s;
}

std::vector<uint8_t> SourceImage()
{
  fat12::BuildOptions o;
  fat12::GeometryForSizeKB(720, o.geo);
  o.label = u"MYDISK";
  o.nowTicks = fat12::DosToTicks(0x5021, 0);
  std::vector<fat12::NewItem> items(3);
  items[0].path = u"a.txt";
  items[0].data = {'a', 'l', 'p', 'h', 'a'};
  items[1].path = u"dir/b.txt";
  items[1].data = {'b', 'e', 't', 'a'};
  items[2].path = u"empty";
  items[2].isDir = true;
  std::vector<uint8_t> img;
  std::string err;
  if (!fat12::Build(o, items, img, &err)) std::fprintf(stderr, "build: %s\n", err.c_str());
  return img;
}

// ------------------------------------------------------------ tests

void TestFileManagerUpdates()
{
  std::printf("file manager updates on the open handler\n");
  const std::vector<uint8_t> src = SourceImage();
  CMyComPtr<IInArchive> arc;
  Check(g_createObject(&g_clsid, &IID_IInArchive, (void **)&arc) == S_OK && arc, "CreateObject(IInArchive)");
  if (!arc) return;
  Check(Reopen(arc, src) == S_OK, "open 720 KB image");
  std::vector<std::string> paths = ListPaths(arc);
  Check(paths.size() == 4, "4 items listed (" + std::to_string(paths.size()) + ")");

  // Add a file and a folder, keep everything else (drag and drop / F5).
  std::vector<Op> ops;
  for (UInt32 i = 0; i < paths.size(); i++) ops.push_back(Keep(i));
  ops.push_back(AddFile("dir/new.txt", "gamma"));
  ops.push_back(AddDir("newdir"));
  std::vector<uint8_t> img;
  HRESULT hr = FmUpdate(arc, ops, img);
  Check(hr == S_OK, "add: UpdateItems on the open handler (hr=" + std::to_string(hr) + ")");
  bool clean = false;
  std::u16string label;
  std::map<std::string, std::string> c = Contents(img, clean, &label);
  Check(clean && img.size() == 737280 && label == u"MYDISK", "add: clean image, 720 KB and label kept");
  Check(c["a.txt"] == "alpha" && c["dir/b.txt"] == "beta" && c["dir/new.txt"] == "gamma" && c["empty"] == "<dir>" &&
            c["newdir"] == "<dir>" && c.size() == 6,
        "add: contents " + Describe(c));

  // The File Manager then closes and reopens the same object.
  Check(Reopen(arc, img) == S_OK, "reopen the same handler on the new image");
  paths = ListPaths(arc);
  Check(paths.size() == 6 && IndexOf(paths, "dir/new.txt") != (UInt32)-1, "reopened handler lists the new file");

  // Delete a.txt (Del).
  ops.clear();
  for (UInt32 i = 0; i < paths.size(); i++)
    if (paths[i] != "a.txt") ops.push_back(Keep(i));
  hr = FmUpdate(arc, ops, img);
  c = Contents(img, clean, nullptr);
  Check(hr == S_OK && clean && !c.count("a.txt") && c.size() == 5, "delete: " + Describe(c));

  // Rename dir/b.txt (F2): new properties, old data.
  Check(Reopen(arc, img) == S_OK, "reopen after delete");
  paths = ListPaths(arc);
  ops.clear();
  for (UInt32 i = 0; i < paths.size(); i++)
    ops.push_back(paths[i] == "dir/b.txt" ? Rename(i, "dir/renamed.txt") : Keep(i));
  hr = FmUpdate(arc, ops, img);
  c = Contents(img, clean, nullptr);
  Check(hr == S_OK && clean && !c.count("dir/b.txt") && c["dir/renamed.txt"] == "beta", "rename: " + Describe(c));

  // New folder (F7) inside an existing folder.
  Check(Reopen(arc, img) == S_OK, "reopen after rename");
  paths = ListPaths(arc);
  ops.clear();
  for (UInt32 i = 0; i < paths.size(); i++) ops.push_back(Keep(i));
  ops.push_back(AddDir("dir/sub"));
  hr = FmUpdate(arc, ops, img);
  c = Contents(img, clean, nullptr);
  Check(hr == S_OK && clean && c["dir/sub"] == "<dir>", "new folder: " + Describe(c));
}

void TestRefusedUpdates()
{
  std::printf("refused updates leave no partial output\n");
  CMyComPtr<IInArchive> arc;
  if (g_createObject(&g_clsid, &IID_IInArchive, (void **)&arc) != S_OK || !arc) return Check(false, "CreateObject");

  // A file that does not fit: refused, nothing usable written.
  std::vector<uint8_t> src = SourceImage();
  Check(Reopen(arc, src) == S_OK, "open 720 KB image");
  std::vector<std::string> paths = ListPaths(arc);
  std::vector<Op> ops;
  for (UInt32 i = 0; i < paths.size(); i++) ops.push_back(Keep(i));
  ops.push_back(AddFile("big.bin", std::string(800000, 'x')));
  std::vector<uint8_t> img;
  std::printf("  (the plugin reports the next errors on stderr)\n");
  std::fflush(stdout);
  HRESULT hr = FmUpdate(arc, ops, img);
  Check(hr != S_OK && img.empty(), "disk full is refused, no output written");

  // Folder SUB pointing at cluster 0: listing incomplete, update refused.
  const uint32_t rootOff = (1 + 2 * 3) * 512; // 720 KB: 1 reserved + 2 FATs of 3 sectors
  for (uint32_t o = rootOff; o < rootOff + 112 * 32; o += 32)
    if (std::memcmp(&src[o], "DIR        ", 11) == 0) src[o + 26] = src[o + 27] = 0;
  Check(Reopen(arc, src) == S_OK, "open image with a damaged folder");
  paths = ListPaths(arc);
  ops.clear();
  for (UInt32 i = 0; i < paths.size(); i++) ops.push_back(Keep(i));
  ops.push_back(AddFile("x.txt", "x"));
  hr = FmUpdate(arc, ops, img);
  Check(hr != S_OK && img.empty(), "update of an incompletely listed image is refused");
}

// Root directory order and first clusters of an image, e.g. "IO.SYS@2".
std::vector<std::string> Layout(const std::vector<uint8_t> &img)
{
  std::vector<std::string> v;
  fat12::Image im;
  if (!im.Open(img)) return v;
  for (const fat12::Entry &e : im.Items())
    if (e.parent < 0) v.push_back(Narrow(e.path) + "@" + std::to_string(e.firstCluster));
  return v;
}

// The first two raw root directory entries, as the MS-DOS boot sector
// checks them ("IO      SYS MSDOS   SYS").
std::string FirstRootEntries(const std::vector<uint8_t> &img)
{
  if (img.size() < 512) return "";
  const size_t bps = img[11] | (img[12] << 8);
  const size_t root = (size_t)((img[14] | (img[15] << 8)) + img[16] * (img[22] | (img[23] << 8))) * bps;
  if (root + 64 > img.size()) return "";
  return std::string((const char *)&img[root], 11) + " " + std::string((const char *)&img[root + 32], 11);
}

std::string Join(const std::vector<std::string> &v)
{
  std::string s;
  for (const std::string &x : v) s += (s.empty() ? "" : " ") + x;
  return s;
}

// A DOS system disk as SYS writes it: IO.SYS first in the root directory
// and at cluster 2, MSDOS.SYS second.  Updates must keep files that are
// already on the disk where they are (7-Zip passes items sorted by name).
void TestKeepsPositions()
{
  std::printf("updates keep existing files in place (DOS system disk)\n");
  fat12::BuildOptions o;
  fat12::GeometryForSizeKB(1440, o.geo);
  o.nowTicks = fat12::DosToTicks(0x5021, 0);
  o.label = u"BOOTDISK"; // must not take root entry 0 or 1
  std::vector<fat12::NewItem> sys(3);
  const char *names[3] = {"IO.SYS", "MSDOS.SYS", "COMMAND.COM"};
  const size_t sizes[3] = {40000, 38000, 54000};
  for (int i = 0; i < 3; i++) {
    sys[i].path = std::u16string(names[i], names[i] + std::strlen(names[i]));
    sys[i].data.assign(sizes[i], (uint8_t)('a' + i));
    sys[i].attr = i < 2 ? 0x07 : 0x20; // IO.SYS, MSDOS.SYS: read-only, hidden, system
  }
  std::vector<uint8_t> img;
  std::string err;
  if (!fat12::Build(o, sys, img, &err)) return Check(false, "build system disk: " + err);
  const std::string before = Join(Layout(img));
  Check(before == "IO.SYS@2 MSDOS.SYS@81 COMMAND.COM@156", "system disk: " + before);
  const std::string sysEntries = "IO      SYS MSDOS   SYS";
  Check(FirstRootEntries(img) == sysEntries, "system disk: root entries 0, 1: " + FirstRootEntries(img));

  CMyComPtr<IInArchive> arc;
  if (g_createObject(&g_clsid, &IID_IInArchive, (void **)&arc) != S_OK || !arc) return Check(false, "CreateObject");

  // Add files that sort before and after the system files.
  Check(Reopen(arc, img) == S_OK, "open system disk");
  std::vector<std::string> paths = ListPaths(arc);
  std::vector<Op> ops;
  ops.push_back(AddFile("AUTOEXEC.BAT", "@echo off"));
  for (UInt32 i = 0; i < paths.size(); i++) ops.push_back(Keep(i));
  ops.push_back(AddFile("ZZZ.TXT", "z"));
  HRESULT hr = FmUpdate(arc, ops, img);
  std::string got = Join(Layout(img));
  Check(hr == S_OK && got == "IO.SYS@2 MSDOS.SYS@81 COMMAND.COM@156 AUTOEXEC.BAT@262 ZZZ.TXT@263",
        "add: existing files keep entry and cluster, new ones go last: " + got);
  Check(FirstRootEntries(img) == sysEntries, "add: root entries 0, 1: " + FirstRootEntries(img));

  // Edit AUTOEXEC.BAT in the File Manager (it grows by a cluster).
  Check(Reopen(arc, img) == S_OK, "reopen before edit");
  paths = ListPaths(arc);
  ops.clear();
  for (UInt32 i = 0; i < paths.size(); i++)
    ops.push_back(paths[i] == "AUTOEXEC.BAT" ? Edit(i, paths[i], std::string(600, 'x')) : Keep(i));
  hr = FmUpdate(arc, ops, img);
  got = Join(Layout(img));
  Check(hr == S_OK && got == "IO.SYS@2 MSDOS.SYS@81 COMMAND.COM@156 AUTOEXEC.BAT@262 ZZZ.TXT@264", "edit: " + got);
  Check(FirstRootEntries(img) == sysEntries, "edit: root entries 0, 1: " + FirstRootEntries(img));
  {
    fat12::Image im;
    Check(im.Open(img) && im.VolumeLabel() == u"BOOTDISK", "edit: label kept");
  }

  // Rename COMMAND.COM (sorts differently now): stays in its place.
  Check(Reopen(arc, img) == S_OK, "reopen after add");
  paths = ListPaths(arc);
  ops.clear();
  for (UInt32 i = 0; i < paths.size(); i++)
    ops.push_back(paths[i] == "COMMAND.COM" ? Rename(i, "A.COM") : Keep(i));
  hr = FmUpdate(arc, ops, img);
  got = Join(Layout(img));
  Check(hr == S_OK && got == "IO.SYS@2 MSDOS.SYS@81 A.COM@156 AUTOEXEC.BAT@262 ZZZ.TXT@264", "rename: " + got);
  Check(FirstRootEntries(img) == sysEntries, "rename: root entries 0, 1: " + FirstRootEntries(img));

  // Delete a file after the system files: they do not move.
  Check(Reopen(arc, img) == S_OK, "reopen after rename");
  paths = ListPaths(arc);
  ops.clear();
  for (UInt32 i = 0; i < paths.size(); i++)
    if (paths[i] != "A.COM") ops.push_back(Keep(i));
  hr = FmUpdate(arc, ops, img);
  got = Join(Layout(img));
  Check(hr == S_OK && got == "IO.SYS@2 MSDOS.SYS@81 AUTOEXEC.BAT@156 ZZZ.TXT@158", "delete: " + got);
  Check(FirstRootEntries(img) == sysEntries, "delete: root entries 0, 1: " + FirstRootEntries(img));
}

void TestCreateNew()
{
  std::printf("new image (Add to archive dialog)\n");
  CMyComPtr<IOutArchive> out;
  Check(g_createObject(&g_clsid, &IID_IOutArchive, (void **)&out) == S_OK && out, "CreateObject(IOutArchive)");
  if (!out) return;
  CMyComPtr<ISetProperties> setProps;
  out.QueryInterface(IID_ISetProperties, &setProps);
  const wchar_t *names[2] = {L"size", L"label"};
  PROPVARIANT values[2];
  values[0].vt = values[1].vt = VT_BSTR;
  values[0].bstrVal = ::SysAllocString(L"360k");
  values[1].bstrVal = ::SysAllocString(L"games");
  Check(setProps && setProps->SetProperties(names, values, 2) == S_OK, "SetProperties size=360k label=games");
  ::SysFreeString(values[0].bstrVal);
  ::SysFreeString(values[1].bstrVal);
  CUpdateCallback *cbSpec = new CUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> cb = cbSpec;
  cbSpec->ops = {AddFile("readme.txt", "hello"), AddDir("docs")};
  CMemOutStream *osSpec = new CMemOutStream;
  CMyComPtr<ISequentialOutStream> os = osSpec;
  const HRESULT hr = out->UpdateItems(os, 2, cb);
  bool clean = false;
  std::u16string label;
  std::map<std::string, std::string> c = Contents(osSpec->data, clean, &label);
  Check(hr == S_OK && clean && osSpec->data.size() == 368640 && label == u"GAMES" && c["readme.txt"] == "hello" &&
            c["docs"] == "<dir>",
        "360 KB image, label GAMES: " + Describe(c));
}

} // namespace

int main(int argc, char **argv)
{
  if (argc < 2) {
    std::fprintf(stderr, "usage: handler_fat12 path/to/fat12.so\n");
    return 2;
  }
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!LoadPlugin(argv[1])) {
    std::printf("SOME HANDLER TESTS FAILED (cannot load %s)\n", argv[1]);
    return 1;
  }
  TestFileManagerUpdates();
  TestRefusedUpdates();
  TestKeepsPositions();
  TestCreateNew();
  std::printf(g_failures ? "SOME HANDLER TESTS FAILED\n" : "ALL HANDLER TESTS PASSED\n");
  return g_failures ? 1 : 0;
}
