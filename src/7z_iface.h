// 7z_iface.h - the subset of the 7-Zip plugin ABI used by this plugin.
//
// Mirrors CPP/7zip/IStream.h, IProgress.h and Archive/IArchive.h from the
// 7-Zip source (checked against 7-Zip 26.00).  The vtable layouts and GUIDs
// must match exactly, so don't reorder methods.
#pragma once

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500 // Windows 2000
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // must precede the other Windows headers

#include <oleauto.h>
#include <propidl.h>
#else
// Linux build (used for testing against 7-Zip for Linux): use 7-Zip's own
// Windows compatibility layer so BSTR/PROPVARIANT/IUnknown match the host.
#include "Common/MyWindows.h"
#endif

#include <cstddef>
#include <cstdint>

namespace z7 {

typedef uint8_t Byte;
typedef uint32_t UInt32;
typedef int32_t Int32;
typedef uint64_t UInt64;
typedef int64_t Int64;

// {23170F69-40C1-278A-0000-00gg00ss0000}
#define Z7_IID(name, group, sub) \
  static const GUID name = {0x23170F69, 0x40C1, 0x278A, {0, 0, 0, (group), 0, (sub), 0, 0}};

Z7_IID(IID_IProgress, 0, 0x05)
Z7_IID(IID_ISequentialInStream, 3, 0x01)
Z7_IID(IID_ISequentialOutStream, 3, 0x02)
Z7_IID(IID_IInStream, 3, 0x03)
Z7_IID(IID_IOutStream, 3, 0x04)
Z7_IID(IID_ISetProperties, 6, 0x03)
Z7_IID(IID_IArchiveOpenCallback, 6, 0x10)
Z7_IID(IID_IArchiveExtractCallback, 6, 0x20)
Z7_IID(IID_IInArchive, 6, 0x60)
Z7_IID(IID_IArchiveUpdateCallback, 6, 0x80)
Z7_IID(IID_IOutArchive, 6, 0xA0)

#define Z7_M(sig) virtual HRESULT STDMETHODCALLTYPE sig = 0;

struct IProgress : public IUnknown {
  Z7_M(SetTotal(UInt64 total))
  Z7_M(SetCompleted(const UInt64 *completeValue))
};

struct ISequentialInStream : public IUnknown {
  Z7_M(Read(void *data, UInt32 size, UInt32 *processedSize))
};
struct ISequentialOutStream : public IUnknown {
  Z7_M(Write(const void *data, UInt32 size, UInt32 *processedSize))
};
struct IInStream : public ISequentialInStream {
  Z7_M(Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
};
struct IOutStream : public ISequentialOutStream {
  Z7_M(Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
  Z7_M(SetSize(UInt64 newSize))
};

struct IArchiveOpenCallback : public IUnknown {
  Z7_M(SetTotal(const UInt64 *files, const UInt64 *bytes))
  Z7_M(SetCompleted(const UInt64 *files, const UInt64 *bytes))
};

struct IArchiveExtractCallback : public IProgress {
  Z7_M(GetStream(UInt32 index, ISequentialOutStream **outStream, Int32 askExtractMode))
  Z7_M(PrepareOperation(Int32 askExtractMode))
  Z7_M(SetOperationResult(Int32 opRes))
};

struct IInArchive : public IUnknown {
  Z7_M(Open(IInStream *stream, const UInt64 *maxCheckStartPosition, IArchiveOpenCallback *openCallback))
  Z7_M(Close())
  Z7_M(GetNumberOfItems(UInt32 *numItems))
  Z7_M(GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
  Z7_M(Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode,
               IArchiveExtractCallback *extractCallback))
  Z7_M(GetArchiveProperty(PROPID propID, PROPVARIANT *value))
  Z7_M(GetNumberOfProperties(UInt32 *numProps))
  Z7_M(GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
  Z7_M(GetNumberOfArchiveProperties(UInt32 *numProps))
  Z7_M(GetArchivePropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
};

struct IArchiveUpdateCallback : public IProgress {
  Z7_M(GetUpdateItemInfo(UInt32 index, Int32 *newData, Int32 *newProps, UInt32 *indexInArchive))
  Z7_M(GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
  Z7_M(GetStream(UInt32 index, ISequentialInStream **inStream))
  Z7_M(SetOperationResult(Int32 operationResult))
};

struct IOutArchive : public IUnknown {
  Z7_M(UpdateItems(ISequentialOutStream *outStream, UInt32 numItems, IArchiveUpdateCallback *updateCallback))
  Z7_M(GetFileTimeType(UInt32 *type))
};

struct ISetProperties : public IUnknown {
  Z7_M(SetProperties(const wchar_t *const *names, const PROPVARIANT *values, UInt32 numProps))
};

#undef Z7_M

// ---- constants (PropID.h / IArchive.h)
enum {
  kpidPath = 3,
  kpidIsDir = 6,
  kpidSize = 7,
  kpidPackSize = 8,
  kpidAttrib = 9,
  kpidCTime = 10,
  kpidATime = 11,
  kpidMTime = 12,
  kpidIsAnti = 21,
  kpidFileSystem = 24,
  kpidPhySize = 44,
  kpidShortName = 50,
  kpidSectorSize = 52,
  kpidTotalSize = 56,
  kpidFreeSpace = 57,
  kpidClusterSize = 58,
  kpidVolumeName = 59,
  kpidWarning = 73
};

namespace NHandlerPropID {
enum {
  kName = 0,
  kClassID,
  kExtension,
  kAddExtension,
  kUpdate,
  kKeepName,
  kSignature,
  kMultiSignature,
  kSignatureOffset,
  kAltStreams,
  kNtSecure,
  kFlags,
  kTimeFlags
};
}
namespace NArcInfoFlags {
const UInt32 kCTime = 1 << 14, kATime = 1 << 16, kMTime = 1 << 18, kMTime_Default = 1 << 19;
}
const UInt32 kFileTimeType_DOS = 2;
namespace NAskMode {
enum { kExtract = 0,
       kTest,
       kSkip };
}
namespace NExtractResult {
enum { kOK = 0,
       kUnsupportedMethod,
       kDataError };
}
const Int32 kUpdateResult_OK = 0;
const UInt32 kSeekSet = 0, kSeekEnd = 2;
const UInt32 kIsArc_No = 0, kIsArc_Yes = 1, kIsArc_NeedMore = 2;
namespace NModulePropID {
enum { kInterfaceType = 0,
       kVersion = 1 };
}

typedef UInt32(WINAPI *Func_IsArc)(const Byte *p, size_t size);

} // namespace z7
