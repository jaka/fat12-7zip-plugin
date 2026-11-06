// plugin.cpp - 7-Zip archive format plugin "FAT12" (floppy disk images).
//
// Copy the built DLL into the "Formats" folder next to 7z.exe / 7zFM.exe.
// Supports list, extract, test, create and update.
#include "7z_iface.h"
#include "fat12.h"
#include "version.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" IMAGE_DOS_HEADER __ImageBase; // start of this DLL's image = its HINSTANCE
#define PLUGIN_EXPORT extern "C"
#else
#define PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace z7;

namespace {

// {B4F1A9C2-3E7D-4A55-8F21-6C0D9E3B7A14} - class id of this handler.
const GUID CLSID_FAT12 = {0xB4F1A9C2, 0x3E7D, 0x4A55, {0x8F, 0x21, 0x6C, 0x0D, 0x9E, 0x3B, 0x7A, 0x14}};
const GUID IID_IUnknown_ = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

const uint32_t kDefaultSizeKB = 1440;
const uint64_t kMaxImageBytes = 256u << 20;

bool SameGuid(const GUID &a, const GUID &b)
{
  return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// ------------------------------------------------------------ strings

#ifdef _WIN32
const wchar_t kOsSep = L'\\';
#else
const wchar_t kOsSep = L'/';
#endif

std::wstring ToW(const std::u16string &s)
{
  std::wstring r;
  if (sizeof(wchar_t) == 2) {
    for (char16_t c : s) r += (wchar_t)c;
  }
  else {
    for (size_t i = 0; i < s.size(); i++) {
      uint32_t c = s[i];
      if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
        c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
      r += (wchar_t)c;
    }
  }
  return r;
}

std::u16string FromW(const wchar_t *s)
{
  std::u16string r;
  if (!s) return r;
  for (; *s; s++) {
    uint32_t c = (uint32_t)*s;
    if (c >= 0x10000) {
      c -= 0x10000;
      r += (char16_t)(0xD800 + (c >> 10));
      r += (char16_t)(0xDC00 + (c & 0x3FF));
    }
    else {
      r += (char16_t)c;
    }
  }
  return r;
}

std::string Narrow(const std::u16string &s)
{
  std::string r;
  for (char16_t c : s) r += c < 0x80 ? (char)c : '?';
  return r;
}

bool EqNoCase(const wchar_t *a, const char *b)
{
  for (; *a && *b; a++, b++) {
    wchar_t x = *a;
    if (x >= L'A' && x <= L'Z') x = (wchar_t)(x + 32);
    if (x != (wchar_t)(unsigned char)*b) return false;
  }
  return *a == 0 && *b == 0;
}

// ------------------------------------------------------------ PROPVARIANT helpers

HRESULT PropStr(PROPVARIANT *v, const std::wstring &s)
{
  v->bstrVal = ::SysAllocString(s.c_str());
  if (!v->bstrVal) return E_OUTOFMEMORY;
  v->vt = VT_BSTR;
  return S_OK;
}
HRESULT PropBin(PROPVARIANT *v, const void *data, unsigned size)
{
  v->bstrVal = ::SysAllocStringByteLen((const char *)data, size);
  if (!v->bstrVal) return E_OUTOFMEMORY;
  v->vt = VT_BSTR;
  return S_OK;
}
void PropBool(PROPVARIANT *v, bool b)
{
  v->vt = VT_BOOL;
  v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}
void PropU32(PROPVARIANT *v, UInt32 x)
{
  v->vt = VT_UI4;
  v->ulVal = x;
}
void PropU64(PROPVARIANT *v, UInt64 x)
{
  v->vt = VT_UI8;
  v->uhVal.QuadPart = x;
}

// DOS times are local time; 7-Zip wants UTC FILETIMEs.
void PropDosTime(PROPVARIANT *v, uint16_t date, uint16_t time, uint8_t tenths)
{
  uint64_t t = fat12::DosToTicks(date, time, tenths);
  if (t == 0) return;
  FILETIME local, utc;
  local.dwLowDateTime = (DWORD)t;
  local.dwHighDateTime = (DWORD)(t >> 32);
  if (!::LocalFileTimeToFileTime(&local, &utc)) utc = local;
  v->vt = VT_FILETIME;
  v->filetime = utc;
}
uint64_t UtcToLocalTicks(const FILETIME &utc)
{
  FILETIME local;
  if (!::FileTimeToLocalFileTime(&utc, &local)) local = utc;
  return ((uint64_t)local.dwHighDateTime << 32) | local.dwLowDateTime;
}
uint64_t NowLocalTicks()
{
#ifdef _WIN32
  FILETIME ft;
  ::GetSystemTimeAsFileTime(&ft);
#else
  FILETIME ft;
  uint64_t t = ((uint64_t)::time(nullptr) + 11644473600ULL) * 10000000ULL;
  ft.dwLowDateTime = (DWORD)t;
  ft.dwHighDateTime = (DWORD)(t >> 32);
#endif
  return UtcToLocalTicks(ft);
}

// Minimal owning pointer for COM objects handed to us by 7-Zip.
template <class T>
struct ComPtr {
  T *p = nullptr;
  ~ComPtr()
  {
    if (p) p->Release();
  }
  T **operator&()
  {
    return &p;
  }
  T *operator->() const
  {
    return p;
  }
  explicit operator bool() const
  {
    return p != nullptr;
  }
};

HRESULT WriteAll(ISequentialOutStream *s, const uint8_t *data, size_t size)
{
  while (size) {
    UInt32 chunk = size > (1u << 20) ? (1u << 20) : (UInt32)size, done = 0;
    HRESULT hr = s->Write(data, chunk, &done);
    if (FAILED(hr)) return hr;
    if (done == 0) return E_FAIL;
    data += done;
    size -= done;
  }
  return S_OK;
}

// Reads up to `limit` bytes; *tooBig is set if the stream has more.
HRESULT ReadAll(ISequentialInStream *s, uint64_t limit, std::vector<uint8_t> &out, bool *tooBig)
{
  out.clear();
  *tooBig = false;
  std::vector<uint8_t> buf(1 << 16);
  for (;;) {
    UInt32 done = 0;
    HRESULT hr = s->Read(buf.data(), (UInt32)buf.size(), &done);
    if (FAILED(hr)) return hr;
    if (done == 0) return S_OK;
    if (out.size() + done > limit) {
      *tooBig = true;
      return S_OK;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + done);
  }
}

// ------------------------------------------------------------ host UI

#ifdef _WIN32
bool IsGuiHost()
{
  wchar_t path[MAX_PATH] = {0};
  ::GetModuleFileNameW(NULL, path, MAX_PATH);
  const wchar_t *name = path;
  for (const wchar_t *p = path; *p; p++)
    if (*p == L'\\' || *p == L'/') name = p + 1;
  return EqNoCase(name, "7zg.exe") || EqNoCase(name, "7zfm.exe");
}

// Finds the topmost visible, captioned top-level window of this process
// (EnumWindows walks in Z order) - in 7zG that is the "Compressing" progress
// window.  Hidden helper windows (IME etc.) are skipped.
BOOL CALLBACK FindOwnerProc(HWND w, LPARAM lp)
{
  DWORD pid = 0;
  ::GetWindowThreadProcessId(w, &pid);
  if (pid != ::GetCurrentProcessId() || !::IsWindowVisible(w)) return TRUE;
  if (((DWORD)::GetWindowLongW(w, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return TRUE;
  *reinterpret_cast<HWND *>(lp) = w;
  return FALSE; // stop: the first match is the topmost one
}

// Owner for our dialogs.  An owned window always stays above its owner, so
// the size dialog can never end up behind 7-Zip's progress window.  The
// progress window may appear a moment after UpdateItems() starts, so wait
// for it up to maxWaitMs.  Returns NULL if 7-Zip shows no window at all.
HWND OwnerWindow(DWORD maxWaitMs = 0)
{
  for (DWORD waited = 0;; waited += 50) {
    HWND found = NULL;
    ::EnumWindows(FindOwnerProc, reinterpret_cast<LPARAM>(&found));
    if (found || waited >= maxWaitMs) return found;
    ::Sleep(50);
  }
}

// ---- floppy size dialog
// A classic dialog built from an in-memory DLGTEMPLATE: works on every
// Windows version from 95/NT4 up (task dialogs would need Vista).

struct SizeChoice {
  uint32_t kb;
  const wchar_t *text;
};
// clang-format off
const SizeChoice kSizeChoices[] = {
  { 160, L"160 KB  -  5.25\" single-sided (DOS 1.0)"},
  { 180, L"180 KB  -  5.25\" single-sided, 9 sectors"},
  { 320, L"320 KB  -  5.25\" double-sided (DOS 1.1)"},
  { 360, L"360 KB  -  5.25\" DD"},
  { 720, L"720 KB  -  3.5\" DD"},
  {1200, L"1.2 MB  -  5.25\" HD"},
  {1440, L"1.44 MB  -  3.5\" HD (most common)"},
  {1680, L"1.68 MB  -  3.5\" DMF (Microsoft distribution format)"},
  {2880, L"2.88 MB  -  3.5\" ED"},
};
// clang-format on
const unsigned kNumSizeChoices = sizeof(kSizeChoices) / sizeof(kSizeChoices[0]);
const WORD kFirstRadioId = 1001;

class DlgTemplate {
public:
  void Header(DWORD style, DWORD exStyle, WORD numItems, short cx, short cy, const wchar_t *title,
              WORD pointSize, const wchar_t *font)
  {
    Dword(style);
    Dword(exStyle);
    Word(numItems);
    Word(0);
    Word(0);
    Word((WORD)cx);
    Word((WORD)cy);
    Word(0);
    Word(0); // no menu, default class
    Str(title);
    Word(pointSize);
    Str(font); // DS_SETFONT
  }
  // atom: 0x0080 = BUTTON, 0x0082 = STATIC
  void Item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD atom, const wchar_t *text)
  {
    if (w_.size() & 1) Word(0); // DWORD alignment
    Dword(style | WS_CHILD | WS_VISIBLE);
    Dword(0);
    Word((WORD)x);
    Word((WORD)y);
    Word((WORD)cx);
    Word((WORD)cy);
    Word(id);
    Word(0xFFFF);
    Word(atom);
    Str(text);
    Word(0); // no creation data
  }
  LPCDLGTEMPLATEW Get() const
  {
    return reinterpret_cast<LPCDLGTEMPLATEW>(w_.data());
  }

private:
  void Word(WORD v)
  {
    w_.push_back(v);
  }
  void Dword(DWORD v)
  {
    Word(LOWORD(v));
    Word(HIWORD(v));
  }
  void Str(const wchar_t *s)
  {
    for (; *s; s++) Word((WORD)*s);
    Word(0);
  }
  std::vector<WORD> w_; // heap memory: suitably aligned for DLGTEMPLATE
};

INT_PTR CALLBACK SizeDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM)
{
  switch (msg) {
    case WM_INITDIALOG: {
      WORD def = kFirstRadioId;
      for (unsigned i = 0; i < kNumSizeChoices; i++)
        if (kSizeChoices[i].kb == kDefaultSizeKB) def = (WORD)(kFirstRadioId + i);
      ::CheckRadioButton(dlg, kFirstRadioId, kFirstRadioId + kNumSizeChoices - 1, def);
      // Make sure we are on top and active (the owner keeps us above the
      // progress window; this also wins the race when the dialog appears).
      ::SetWindowPos(dlg, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
      ::SetForegroundWindow(dlg);
      ::SetFocus(::GetDlgItem(dlg, def));
      return FALSE; // we set the focus ourselves
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) {
        INT_PTR kb = (INT_PTR)kDefaultSizeKB;
        for (unsigned i = 0; i < kNumSizeChoices; i++)
          if (::IsDlgButtonChecked(dlg, kFirstRadioId + i) == BST_CHECKED) kb = (INT_PTR)kSizeChoices[i].kb;
        ::EndDialog(dlg, kb);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        ::EndDialog(dlg, 0);
        return TRUE;
      }
      break;
    default:
      break;
  }
  return FALSE;
}

// S_OK = chosen, E_ABORT = cancelled, S_FALSE = dialog could not be shown.
HRESULT AskForSize(uint32_t &kb)
{
  const short kW = 230, kMargin = 7, kRadioTop = 22, kRadioStep = 12;
  const short hintTop = (short)(kRadioTop + kNumSizeChoices * kRadioStep + 6);
  const short buttonsTop = (short)(hintTop + 22);
  const short kH = (short)(buttonsTop + 14 + kMargin);

  DlgTemplate t;
  // Owned by 7-Zip's progress window when there is one; otherwise topmost,
  // so the question can never be hidden behind other windows.
  const HWND owner = OwnerWindow(2000);
  t.Header(DS_SETFONT | DS_FIXEDSYS | DS_MODALFRAME | DS_CENTER | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION |
               WS_SYSMENU,
           owner ? 0 : (DWORD)WS_EX_TOPMOST, (WORD)(kNumSizeChoices + 4), kW, kH, L"FAT12 floppy image", 8,
           L"MS Shell Dlg");
  t.Item(SS_LEFT, kMargin, kMargin, kW - 2 * kMargin, 9, 0xFFFF, 0x0082,
         L"Choose the size of the new floppy image:");
  for (unsigned i = 0; i < kNumSizeChoices; i++)
    t.Item(BS_AUTORADIOBUTTON | (i == 0 ? (WS_GROUP | WS_TABSTOP) : 0), kMargin + 6,
           (short)(kRadioTop + i * kRadioStep), kW - 2 * kMargin - 6, 10, (WORD)(kFirstRadioId + i), 0x0080,
           kSizeChoices[i].text);
  t.Item(SS_LEFT | WS_GROUP, kMargin, hintTop, kW - 2 * kMargin, 18, 0xFFFF, 0x0082,
         L"Tip: type size=1.44m (or 1.2m, 720k, ...) in the Parameters box to skip this question.");
  t.Item(BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, kW - kMargin - 2 * 50 - 4, buttonsTop, 50, 14, IDOK,
         0x0080, L"OK");
  t.Item(BS_PUSHBUTTON | WS_TABSTOP, kW - kMargin - 50, buttonsTop, 50, 14, IDCANCEL, 0x0080, L"Cancel");

  // This DLL's own module handle (linker-provided; GetModuleHandleEx would need XP).
  const HINSTANCE self = reinterpret_cast<HINSTANCE>(&__ImageBase);
  INT_PTR r = ::DialogBoxIndirectParamW(self, t.Get(), owner, SizeDlgProc, 0);
  if (r == -1) return S_FALSE;
  if (r == 0) return E_ABORT;
  kb = (uint32_t)r;
  return S_OK;
}

void ReportError(const std::string &msg)
{
  std::string full = "FAT12: " + msg;
  if (IsGuiHost()) {
    const HWND owner = OwnerWindow();
    ::MessageBoxA(owner, full.c_str(), "FAT12 floppy image", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | (owner ? 0 : MB_TOPMOST));
  }
  else {
    // WriteFile rather than fprintf: keeps MinGW's ~35 KB printf out of the DLL.
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) {
      full = "\r\n" + full + "\r\n";
      DWORD written = 0;
      ::WriteFile(err, full.data(), (DWORD)full.size(), &written, NULL);
    }
  }
}
#else
void ReportError(const std::string &msg)
{
  std::fprintf(stderr, "\nFAT12: %s\n", msg.c_str());
  std::fflush(stderr);
}
#endif

HRESULT ErrorToHresult(const std::string &err)
{
  (void)err;
#ifdef _WIN32
  if (err.rfind("disk full", 0) == 0 || err.rfind("root directory full", 0) == 0)
    return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
#endif
  return E_FAIL;
}

// ------------------------------------------------------------ handler

// clang-format off
const PROPID kItemProps[] = {kpidPath, kpidIsDir, kpidSize, kpidPackSize, kpidMTime,
                             kpidCTime, kpidATime, kpidAttrib, kpidShortName};
const VARTYPE kItemPropTypes[] = {VT_BSTR, VT_BOOL, VT_UI8, VT_UI8, VT_FILETIME,
                                  VT_FILETIME, VT_FILETIME, VT_UI4, VT_BSTR};
const PROPID kArcProps[] = {kpidFileSystem, kpidVolumeName, kpidClusterSize, kpidSectorSize,
                            kpidTotalSize, kpidFreeSpace};
const VARTYPE kArcPropTypes[] = {VT_BSTR, VT_BSTR, VT_UI4, VT_UI4, VT_UI8, VT_UI8};
// clang-format on

class CHandler final : public IInArchive, public IOutArchive, public ISetProperties {
public:
  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
  {
    *out = nullptr;
    if (SameGuid(iid, IID_IUnknown_) || SameGuid(iid, IID_IInArchive))
      *out = static_cast<IInArchive *>(this);
    else if (SameGuid(iid, IID_IOutArchive))
      *out = static_cast<IOutArchive *>(this);
    else if (SameGuid(iid, IID_ISetProperties))
      *out = static_cast<ISetProperties *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++refs_;
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }

  // IInArchive
  HRESULT STDMETHODCALLTYPE Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *) override
  {
    try {
      Close();
      UInt64 size = 0;
      HRESULT hr = stream->Seek(0, kSeekEnd, &size);
      if (FAILED(hr)) return hr;
      if (size < 512 || size > kMaxImageBytes) return S_FALSE;
      hr = stream->Seek(0, kSeekSet, nullptr);
      if (FAILED(hr)) return hr;
      std::vector<uint8_t> data;
      bool tooBig;
      hr = ReadAll(stream, size, data, &tooBig);
      if (FAILED(hr)) return hr;
      if (!img_.Open(std::move(data))) return S_FALSE;
      opened_ = true;
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }
  HRESULT STDMETHODCALLTYPE Close() override
  {
    img_ = fat12::Image();
    opened_ = false;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNumberOfItems(UInt32 *n) override
  {
    *n = opened_ ? (UInt32)img_.Items().size() : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PROPID id, PROPVARIANT *v) override
  {
    if (!opened_ || index >= img_.Items().size()) return E_INVALIDARG;
    const fat12::Entry &e = img_.Items()[index];
    switch (id) {
      case kpidPath: {
        std::wstring p = ToW(e.path);
        for (wchar_t &c : p)
          if (c == L'/') c = kOsSep;
        return PropStr(v, p);
      }
      case kpidShortName:
        return PropStr(v, ToW(e.shortName));
      case kpidIsDir:
        PropBool(v, e.IsDir());
        break;
      case kpidSize:
        if (!e.IsDir()) PropU64(v, e.size);
        break;
      case kpidPackSize: {
        const uint64_t cb = img_.Geo().ClusterBytes();
        PropU64(v, e.IsDir() ? 0 : (e.size + cb - 1) / cb * cb);
        break;
      }
      case kpidAttrib:
        PropU32(v, e.attr);
        break;
      case kpidMTime:
        PropDosTime(v, e.mDate, e.mTime, 0);
        break;
      case kpidCTime:
        PropDosTime(v, e.cDate, e.cTime, e.cTenths);
        break;
      case kpidATime:
        PropDosTime(v, e.aDate, 0, 0);
        break;
      default:
        break;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode,
                                    IArchiveExtractCallback *cb) override
  {
    try {
      const bool all = numItems == (UInt32)(Int32)-1;
      if (!opened_) return all ? S_OK : E_INVALIDARG;
      if (all) numItems = (UInt32)img_.Items().size();
      UInt64 total = 0, done = 0;
      for (UInt32 i = 0; i < numItems; i++) {
        UInt32 idx = all ? i : indices[i];
        if (idx >= img_.Items().size()) return E_INVALIDARG;
        total += img_.Items()[idx].size;
      }
      HRESULT hr = cb->SetTotal(total);
      if (FAILED(hr)) return hr;
      std::vector<uint8_t> data;
      for (UInt32 i = 0; i < numItems; i++) {
        hr = cb->SetCompleted(&done);
        if (FAILED(hr)) return hr;
        const UInt32 idx = all ? i : indices[i];
        const fat12::Entry &e = img_.Items()[idx];
        const Int32 ask = testMode ? NAskMode::kTest : NAskMode::kExtract;
        ComPtr<ISequentialOutStream> out;
        hr = cb->GetStream(idx, &out, ask);
        if (FAILED(hr)) return hr;
        if (e.IsDir()) {
          if (FAILED(hr = cb->PrepareOperation(ask))) return hr;
          if (FAILED(hr = cb->SetOperationResult(NExtractResult::kOK))) return hr;
          continue;
        }
        if (!testMode && !out) continue; // skipped by the user
        if (FAILED(hr = cb->PrepareOperation(ask))) return hr;
        const bool ok = img_.ReadFile(idx, data);
        if (out) {
          hr = WriteAll(out.p, data.data(), data.size());
          if (FAILED(hr)) return hr;
        }
        done += e.size;
        hr = cb->SetOperationResult(ok ? NExtractResult::kOK : NExtractResult::kDataError);
        if (FAILED(hr)) return hr;
      }
      return cb->SetCompleted(&done);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }
  HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID id, PROPVARIANT *v) override
  {
    if (!opened_) return S_OK;
    const fat12::Geometry &g = img_.Geo();
    switch (id) {
      case kpidFileSystem:
        return PropStr(v, L"FAT12");
      case kpidVolumeName:
        if (!img_.VolumeLabel().empty()) return PropStr(v, ToW(img_.VolumeLabel()));
        break;
      case kpidClusterSize:
        PropU32(v, g.ClusterBytes());
        break;
      case kpidSectorSize:
        PropU32(v, g.bytesPerSector);
        break;
      case kpidTotalSize:
        PropU64(v, (UInt64)g.DataClusters() * g.ClusterBytes());
        break;
      case kpidFreeSpace:
        PropU64(v, img_.FreeBytes());
        break;
      case kpidPhySize:
        PropU64(v, g.ImageBytes());
        break;
      case kpidWarning:
        if (img_.HeadersWarning()) return PropStr(v, L"The file system structures are damaged");
        break;
      default:
        break;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UInt32 *n) override
  {
    *n = sizeof(kItemProps) / sizeof(kItemProps[0]);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPropertyInfo(UInt32 i, BSTR *name, PROPID *id, VARTYPE *vt) override
  {
    if (i >= sizeof(kItemProps) / sizeof(kItemProps[0])) return E_INVALIDARG;
    *name = nullptr;
    *id = kItemProps[i];
    *vt = kItemPropTypes[i];
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UInt32 *n) override
  {
    *n = sizeof(kArcProps) / sizeof(kArcProps[0]);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UInt32 i, BSTR *name, PROPID *id, VARTYPE *vt) override
  {
    if (i >= sizeof(kArcProps) / sizeof(kArcProps[0])) return E_INVALIDARG;
    *name = nullptr;
    *id = kArcProps[i];
    *vt = kArcPropTypes[i];
    return S_OK;
  }

  // ISetProperties:  -msize=1.44m  -mlabel=MYDISK  (also in the GUI "Parameters" box)
  HRESULT STDMETHODCALLTYPE SetProperties(const wchar_t *const *names, const PROPVARIANT *values,
                                          UInt32 num) override
  {
    sizeKB_ = 0;
    askSize_ = false;
    labelSet_ = false;
    for (UInt32 i = 0; i < num; i++) {
      const wchar_t *n = names[i];
      const PROPVARIANT &v = values[i];
      if (EqNoCase(n, "size")) {
        uint32_t kb = 0;
        fat12::Geometry g;
        if (v.vt == VT_UI4) {
          const std::string digits = std::to_string(v.ulVal); // to_wstring would pull in MinGW's printf
          if (!fat12::ParseSizeSpec(std::u16string(digits.begin(), digits.end()), kb)) return E_INVALIDARG;
        }
        else if (v.vt == VT_BSTR) {
          if (EqNoCase(v.bstrVal, "ask")) {
            askSize_ = true;
            continue;
          }
          if (!fat12::ParseSizeSpec(FromW(v.bstrVal), kb)) {
            ReportError("cannot parse size '" + Narrow(FromW(v.bstrVal)) +
                        "'; use e.g. size=1.44m, 1.2m, 720k, 360k, 2.88m or dmf");
            return E_INVALIDARG;
          }
        }
        else {
          return E_INVALIDARG;
        }
        if (!fat12::GeometryForSizeKB(kb, g)) {
          ReportError("unsupported disk size " + std::to_string(kb) + " KB");
          return E_INVALIDARG;
        }
        sizeKB_ = kb;
      }
      else if (EqNoCase(n, "label")) {
        labelSet_ = true;
        label_.clear();
        if (v.vt == VT_BSTR)
          label_ = FromW(v.bstrVal);
        else if (v.vt != VT_EMPTY)
          return E_INVALIDARG;
        if (label_.size() > 11) {
          ReportError("volume label longer than 11 characters");
          return E_INVALIDARG;
        }
        for (char16_t &c : label_) {
          if (c >= u'a' && c <= u'z') c = (char16_t)(c - 32);
          if (c >= 0x80 || c < 0x20 || std::strchr("\"*+,./:;<=>?[\\]|", (int)c)) {
            ReportError("volume label contains an invalid character");
            return E_INVALIDARG;
          }
        }
      }
      // Other names (x, mt, tm, tc, ...) are generic 7-Zip switches; ignore them.
    }
    return S_OK;
  }

  // IOutArchive
  HRESULT STDMETHODCALLTYPE GetFileTimeType(UInt32 *type) override
  {
    *type = kFileTimeType_DOS;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE UpdateItems(ISequentialOutStream *outStream, UInt32 numItems,
                                        IArchiveUpdateCallback *cb) override
  {
    try {
      return DoUpdate(outStream, numItems, cb);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

private:
  HRESULT GetProp(IArchiveUpdateCallback *cb, UInt32 i, PROPID id, PROPVARIANT &v)
  {
    std::memset(&v, 0, sizeof(v));
    v.vt = VT_EMPTY;
    return cb->GetProperty(i, id, &v);
  }

  HRESULT ChooseSize(uint32_t &kb)
  {
    if (sizeKB_) {
      kb = sizeKB_;
      return S_OK;
    }
    if (opened_ && !askSize_) {
      kb = 0;
      return S_OK;
    } // keep existing geometry
    kb = kDefaultSizeKB;
#ifdef _WIN32
    if (askSize_ || IsGuiHost()) {
      HRESULT hr = AskForSize(kb);
      if (hr == E_ABORT) return E_ABORT;
    }
#endif
    return S_OK;
  }

  HRESULT DoUpdate(ISequentialOutStream *outStream, UInt32 numItems, IArchiveUpdateCallback *cb)
  {
    fat12::BuildOptions opt;
    uint32_t kb = 0;
    HRESULT hr = ChooseSize(kb);
    if (hr != S_OK) return hr;
    if (kb) {
      if (!fat12::GeometryForSizeKB(kb, opt.geo)) return E_INVALIDARG;
    }
    else {
      opt.geo = img_.Geo();
      // The geometry comes from the old boot sector, which may claim far
      // more than the file holds; don't build a multi-GB image from it.
      if (opt.geo.ImageBytes() > kMaxImageBytes) {
        ReportError("the image's boot sector describes a disk larger than " +
                    std::to_string(kMaxImageBytes >> 20) + " MB; use -msize= to rebuild it");
        return E_INVALIDARG;
      }
    }
    if (labelSet_)
      opt.label = label_;
    else if (opened_)
      opt.label = img_.VolumeLabel();
    if (opened_) {
      opt.serial = img_.VolumeSerial();
      opt.bootSector = &img_.Raw();
    }
    opt.nowTicks = NowLocalTicks();
    const uint64_t capacity = opt.geo.ImageBytes();

    // Progress total: sizes of new data.
    UInt64 total = 0, done = 0;
    for (UInt32 i = 0; i < numItems; i++) {
      Int32 newData = 0, newProps = 0;
      UInt32 old = 0;
      if (FAILED(hr = cb->GetUpdateItemInfo(i, &newData, &newProps, &old))) return hr;
      if (newData) {
        PROPVARIANT v;
        if (FAILED(hr = GetProp(cb, i, kpidSize, v))) return hr;
        if (v.vt == VT_UI8) total += v.uhVal.QuadPart;
      }
    }
    cb->SetTotal(total);

    std::vector<fat12::NewItem> items;
    items.reserve(numItems);
    for (UInt32 i = 0; i < numItems; i++) {
      Int32 newData = 0, newProps = 0;
      UInt32 old = 0;
      if (FAILED(hr = cb->GetUpdateItemInfo(i, &newData, &newProps, &old))) return hr;
      if ((!newProps || !newData) && (!opened_ || old >= img_.Items().size())) return E_INVALIDARG;

      fat12::NewItem it;
      if (newProps) {
        PROPVARIANT v;
        if (FAILED(hr = GetProp(cb, i, kpidIsAnti, v))) return hr;
        if (v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE) continue; // deletion marker
        if (FAILED(hr = GetProp(cb, i, kpidPath, v))) return hr;
        if (v.vt != VT_BSTR) return E_INVALIDARG;
        it.path = FromW(v.bstrVal);
        ::SysFreeString(v.bstrVal);
        if (FAILED(hr = GetProp(cb, i, kpidIsDir, v))) return hr;
        it.isDir = v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
        if (FAILED(hr = GetProp(cb, i, kpidAttrib, v))) return hr;
        it.attr = v.vt == VT_UI4 ? (uint8_t)(v.ulVal & 0x27) : (it.isDir ? 0 : fat12::kAttrArchive);
        if (FAILED(hr = GetProp(cb, i, kpidMTime, v))) return hr;
        if (v.vt == VT_FILETIME) {
          it.hasMTime = true;
          it.mTicks = UtcToLocalTicks(v.filetime);
        }
        if (FAILED(hr = GetProp(cb, i, kpidCTime, v))) return hr;
        if (v.vt == VT_FILETIME) {
          it.hasCTime = true;
          it.cTicks = UtcToLocalTicks(v.filetime);
        }
        if (FAILED(hr = GetProp(cb, i, kpidATime, v))) return hr;
        if (v.vt == VT_FILETIME) {
          it.hasATime = true;
          it.aTicks = UtcToLocalTicks(v.filetime);
        }
      }
      else {
        const fat12::Entry &e = img_.Items()[old];
        it.path = e.path;
        it.isDir = e.IsDir();
        it.attr = e.attr & 0x27;
        it.hasMTime = true;
        it.mTicks = fat12::DosToTicks(e.mDate, e.mTime);
        it.hasCTime = e.cDate != 0;
        it.cTicks = fat12::DosToTicks(e.cDate, e.cTime, e.cTenths);
        it.hasATime = e.aDate != 0;
        it.aTicks = fat12::DosToTicks(e.aDate, 0);
        if (!it.mTicks) it.hasMTime = false;
        if (!it.cTicks) it.hasCTime = false;
        if (!it.aTicks) it.hasATime = false;
      }

      if (!it.isDir) {
        if (newData) {
          ComPtr<ISequentialInStream> in;
          hr = cb->GetStream(i, &in);
          if (hr == S_FALSE) continue; // file could not be opened and the user chose to skip it
          if (FAILED(hr)) return hr;
          if (in) {
            bool tooBig = false;
            if (FAILED(hr = ReadAll(in.p, capacity, it.data, &tooBig))) return hr;
            if (tooBig) {
              ReportError("file does not fit on a " + std::to_string(capacity / 1024) +
                          " KB disk: " + Narrow(it.path));
              return ErrorToHresult("disk full");
            }
          }
          done += it.data.size();
        }
        else if (!img_.ReadFile(old, it.data)) {
          // Copying it would replace the original with a silently truncated
          // file.  7-Zip writes to a temporary file, so aborting here leaves
          // the original image untouched.
          ReportError("damaged file in the image: " + Narrow(it.path) +
                      "\nThe image was not changed. Extract what you can, or delete that file first.");
          return E_FAIL;
        }
      }
      if (newData) {
        if (FAILED(hr = cb->SetOperationResult(kUpdateResult_OK))) return hr;
        if (FAILED(hr = cb->SetCompleted(&done))) return hr;
      }
      items.push_back(std::move(it));
    }

    std::vector<uint8_t> image;
    std::string err;
    if (!fat12::Build(opt, items, image, &err)) {
      ReportError(err);
      return ErrorToHresult(err);
    }
    return WriteAll(outStream, image.data(), image.size());
  }

  std::atomic<ULONG> refs_{0};
  fat12::Image img_;
  bool opened_ = false;
  uint32_t sizeKB_ = 0;
  bool askSize_ = false;
  bool labelSet_ = false;
  std::u16string label_;
};

UInt32 WINAPI IsArcFat12(const Byte *p, size_t size)
{
  return fat12::IsArc(p, size);
}

} // namespace

// ------------------------------------------------------------ DLL exports

PLUGIN_EXPORT HRESULT WINAPI CreateObject(const GUID *clsid, const GUID *iid, void **out)
{
  *out = nullptr;
  if (!SameGuid(*clsid, CLSID_FAT12)) return CLASS_E_CLASSNOTAVAILABLE;
  const bool in = SameGuid(*iid, IID_IInArchive), outArc = SameGuid(*iid, IID_IOutArchive);
  if (!in && !outArc) return E_NOINTERFACE;
  CHandler *h = new (std::nothrow) CHandler();
  if (!h) return E_OUTOFMEMORY;
  if (in)
    *out = static_cast<IInArchive *>(h);
  else
    *out = static_cast<IOutArchive *>(h);
  h->AddRef();
  return S_OK;
}

PLUGIN_EXPORT HRESULT WINAPI GetNumberOfFormats(UInt32 *num)
{
  *num = 1;
  return S_OK;
}

PLUGIN_EXPORT HRESULT WINAPI GetHandlerProperty2(UInt32 index, PROPID id, PROPVARIANT *v)
{
  if (index != 0) return E_INVALIDARG;
  switch (id) {
    case NHandlerPropID::kName:
      return PropStr(v, L"f12");
    case NHandlerPropID::kClassID:
      return PropBin(v, &CLSID_FAT12, sizeof(GUID));
    case NHandlerPropID::kExtension:
      return PropStr(v, L"img ima vfd flp");
    case NHandlerPropID::kUpdate:
      PropBool(v, true);
      break;
    case NHandlerPropID::kKeepName:
      PropBool(v, false);
      break;
    case NHandlerPropID::kSignature: {
      static const Byte sig[2] = {0x55, 0xAA};
      return PropBin(v, sig, 2);
    }
    case NHandlerPropID::kSignatureOffset:
      PropU32(v, 0x1FE);
      break;
    case NHandlerPropID::kFlags:
      PropU32(v, NArcInfoFlags::kMTime | NArcInfoFlags::kMTime_Default | NArcInfoFlags::kCTime |
                     NArcInfoFlags::kATime);
      break;
    case NHandlerPropID::kTimeFlags:
      // supported precision: DOS (2 s); default precision: DOS
      PropU32(v, (1u << kFileTimeType_DOS) | (kFileTimeType_DOS << 27));
      break;
    default:
      break;
  }
  return S_OK;
}

PLUGIN_EXPORT HRESULT WINAPI GetHandlerProperty(PROPID id, PROPVARIANT *v)
{
  return GetHandlerProperty2(0, id, v);
}

PLUGIN_EXPORT HRESULT WINAPI GetIsArc(UInt32 index, Func_IsArc *isArc)
{
  *isArc = nullptr;
  if (index != 0) return E_INVALIDARG;
  *isArc = IsArcFat12;
  return S_OK;
}

PLUGIN_EXPORT HRESULT WINAPI GetModuleProp(PROPID id, PROPVARIANT *v)
{
  switch (id) {
    // 0 = IUnknown without virtual destructor (7-Zip 23+ ABI, and all Windows builds)
    case NModulePropID::kInterfaceType:
      PropU32(v, 0);
      break;
    case NModulePropID::kVersion:
      PropU32(v, ((uint32_t)FAT12_VER_MAJOR << 16) | FAT12_VER_MINOR);
      break;
    default:
      break;
  }
  return S_OK;
}
