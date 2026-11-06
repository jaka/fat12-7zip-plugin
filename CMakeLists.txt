# Windows build of the FAT12 7-Zip format plugin (MSVC or MinGW-w64).
# MinGW x86 DLL: Windows 2000 and later; MSVC (v141_xp) / x64: XP and later.
# On Linux, `make` cross-compiles with the same MinGW flags as below.
#   MSVC: XP needs the "v141_xp" toolset (VS 2017, "Windows XP support for C++"):
#     cmake -B build -A Win32 -T v141_xp     (32-bit 7-Zip)
#     cmake -B build -A x64   -T v141_xp     (64-bit 7-Zip)
#   MinGW-w64:
#     cmake -B build -G "MinGW Makefiles"
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.15)
project(fat12_7z_plugin CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (MinGW Makefiles, Ninja) build unoptimized without this.
get_property(MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Optimize Release for size: the plugin is I/O-bound, and most of the DLL is
# static C++ runtime that only section-level dead-code removal can drop.
string(REPLACE "-O3" "-Os" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "/O2" "/O1" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

if(NOT WIN32)
  message(FATAL_ERROR "This CMake file builds the Windows DLL. For Linux tests see tests/build_and_test_linux.sh")
endif()

# Static runtime: the DLL has no dependency on VC++/MinGW runtime DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(fat12 SHARED src/plugin.cpp src/fat12.cpp src/xp_compat.cpp src/fat12.def src/fat12.rc)
set_target_properties(fat12 PROPERTIES PREFIX "" OUTPUT_NAME "fat12")
target_compile_definitions(fat12 PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0500)
target_link_libraries(fat12 PRIVATE oleaut32 user32)

# Minimum OS version in the PE header: x86 = 5.00 (Windows 2000), x64 = 5.02 (XP x64).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(XP_MINOR 02)
else()
  set(XP_MINOR 00)   # Windows 2000
endif()

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  # rc.exe does not predefine _WIN64; the version resource uses it for "x64".
  set_source_files_properties(src/fat12.rc PROPERTIES COMPILE_DEFINITIONS _WIN64)
endif()

if(MSVC)
  target_compile_options(fat12 PRIVATE /W4 /utf-8 /EHsc /Gy /Gw)
  target_link_options(fat12 PRIVATE $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
  if(CMAKE_GENERATOR_TOOLSET MATCHES "_xp")
    target_link_options(fat12 PRIVATE /SUBSYSTEM:WINDOWS,5.${XP_MINOR})
  else()
    message(WARNING "Not using an *_xp toolset: the DLL will NOT load on Windows XP. Use -T v141_xp.")
  endif()
elseif(MINGW)
  # Keep in sync with the Windows DLL flags in the Makefile.
  target_compile_options(fat12 PRIVATE -Wall -Wextra -Wno-non-virtual-dtor -ffunction-sections -fdata-sections)
  target_link_options(fat12 PRIVATE -static -static-libgcc -static-libstdc++
    -Wl,--enable-stdcall-fixup,--gc-sections $<$<NOT:$<CONFIG:Debug>>:-s>
    -Wl,--major-os-version,5,--minor-os-version,${XP_MINOR}
    -Wl,--major-subsystem-version,5,--minor-subsystem-version,${XP_MINOR})
endif()
