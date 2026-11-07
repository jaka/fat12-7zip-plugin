@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem Copyright (C) 2026 jaka
rem Build fat12.dll with Visual Studio, no CMake needed.
rem Run from an "x86 Native Tools Command Prompt for VS 2017" (or x64 for 64-bit 7-Zip).
rem Windows XP support needs the v141_xp toolset ("Windows XP support for C++"
rem in the VS 2017 installer) and the Windows 7.1A SDK it brings: run first
rem    set INCLUDE=%ProgramFiles(x86)%\Microsoft SDKs\Windows\7.1A\Include;%INCLUDE%
rem    set LIB=%ProgramFiles(x86)%\Microsoft SDKs\Windows\7.1A\Lib;%LIB%
rem (use ...\7.1A\Lib\x64 for 64-bit) and pass /D_USING_V110_SDK71_ via CL.
rem For XP, building with MinGW (make on Linux) is usually simpler.
setlocal
rem XP x86 = 5.01, XP x64 = 5.02
set XPVER=5.01
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" set XPVER=5.02
set RCDEFS=
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" set RCDEFS=/D_WIN64
if not exist out mkdir out
rc /nologo %RCDEFS% /fo out\fat12.res src\fat12.rc
if errorlevel 1 exit /b 1
rem /O1 /Gy /Gw + /OPT:REF,ICF: optimize for size and drop unreferenced code.
cl /nologo /O1 /Gy /Gw /MT /EHsc /W4 /utf-8 /std:c++17 /LD ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0500 ^
   src\plugin.cpp src\fat12.cpp out\fat12.res ^
   /Fo:out\ /Fe:out\fat12.dll ^
   /link /DEF:src\fat12.def oleaut32.lib user32.lib /SUBSYSTEM:WINDOWS,%XPVER% /OPT:REF /OPT:ICF
if errorlevel 1 exit /b 1
echo.
echo Built out\fat12.dll - copy it to "%%ProgramFiles%%\7-Zip\Formats\" (create the folder).
