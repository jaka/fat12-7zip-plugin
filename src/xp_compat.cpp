// xp_compat.cpp - Windows XP support for MinGW-w64 builds.
//
// GCC 13's libgcc (gthr-win32.o, pulled in by C++ exception handling)
// imports kernel32!GetThreadId, which exists only on Vista / Server 2003
// and later.  The XP loader refuses DLLs with unresolved imports, so we
// provide the import slot ourselves: the linker resolves __imp_GetThreadId
// here instead of importing it from kernel32.
//
// Not needed (and not compiled) for MSVC builds.
#if defined(_WIN32) && defined(__GNUC__)

#include <windows.h>

namespace {

struct ThreadBasicInformation {
  LONG exitStatus;
  PVOID tebBaseAddress;
  HANDLE uniqueProcess; // CLIENT_ID
  HANDLE uniqueThread;
  ULONG_PTR affinityMask;
  LONG priority;
  LONG basePriority;
};

typedef DWORD(WINAPI *GetThreadIdFn)(HANDLE);
typedef LONG(NTAPI *NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);

DWORD WINAPI GetThreadIdShim(HANDLE thread)
{
  // Prefer the real function when the OS has it.
  HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
  GetThreadIdFn real = k32 ? (GetThreadIdFn)(void *)::GetProcAddress(k32, "GetThreadId") : NULL;
  if (real) return real(thread);

  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  NtQueryInformationThreadFn query =
      ntdll ? (NtQueryInformationThreadFn)(void *)::GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
  if (!query) {
    ::SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
  }
  ThreadBasicInformation info;
  if (query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), NULL) < 0) {
    ::SetLastError(ERROR_INVALID_HANDLE);
    return 0;
  }
  return (DWORD)(ULONG_PTR)info.uniqueThread;
}

} // namespace

// Import-table slot names: x64 = __imp_GetThreadId, x86 = __imp__GetThreadId@4.
#ifdef _WIN64
extern "C" void *xp_imp_GetThreadId __asm__("__imp_GetThreadId");
void *xp_imp_GetThreadId = (void *)&GetThreadIdShim;
#else
extern "C" void *xp_imp_GetThreadId __asm__("__imp__GetThreadId@4");
void *xp_imp_GetThreadId = (void *)&GetThreadIdShim;
#endif

#endif
