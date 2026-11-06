; fat12.nsi - NSIS installer for the 7-Zip FAT12 plugin (Windows 2000 and later).
;
; Copies fat12.dll into the Formats folder of every 7-Zip it finds: the x64 DLL
; into 64-bit 7-Zip, the x86 DLL into 32-bit 7-Zip.  The bitness of a 7-Zip
; folder is read from the PE header of its 7z.dll, so a mismatched DLL is never
; installed.  Also registers an uninstaller in Add/Remove Programs.
;
; Build (needs NSIS 3.x; on Linux: apt install nsis):
;   make installer                      (builds the DLLs first)
;   makensis installer/fat12.nsi        (DLLs already in dist/x86, dist/x64)
; Silent install / uninstall: setup.exe /S, uninstall.exe /S

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel admin
ManifestSupportedOS all

!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh
!include WinMessages.nsh

!ifndef DIST
  !define DIST "../dist"
!endif
!searchparse /file "../src/version.h" `#define FAT12_VER_STR "` VERSION `"`

!define PRODUCT  "7-Zip FAT12 Plugin"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
!define MACHINE_X86 332     ; IMAGE_FILE_MACHINE_I386
!define MACHINE_X64 34404   ; IMAGE_FILE_MACHINE_AMD64

Name "${PRODUCT} ${VERSION}"
OutFile "${DIST}/fat12-7zip-plugin-${VERSION}-setup.exe"
InstallDir "$PROGRAMFILES\${PRODUCT}"   ; holds only the uninstaller
BrandingText "${PRODUCT} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey ProductName "${PRODUCT}"
VIAddVersionKey ProductVersion "${VERSION}"
VIAddVersionKey FileVersion "${VERSION}"
VIAddVersionKey FileDescription "${PRODUCT} Setup"
VIAddVersionKey LegalCopyright ""

Var Dir64     ; 64-bit 7-Zip folder
Var Dir32     ; 32-bit 7-Zip folder

; ---------------------------------------------------------------- pages ----

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This will install the FAT12 floppy image plugin (fat12.dll) into 7-Zip.$\r$\n$\r$\n7-Zip will then be able to create and update .img, .ima, .vfd and .flp floppy images.$\r$\n$\r$\nClose 7-Zip before continuing."
!insertmacro MUI_PAGE_WELCOME

!define MUI_COMPONENTSPAGE_NODESC
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ComponentsLeave
!insertmacro MUI_PAGE_COMPONENTS

!define MUI_PAGE_HEADER_TEXT "64-bit 7-Zip"
!define MUI_PAGE_HEADER_SUBTEXT "Choose the folder of your 64-bit 7-Zip."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Setup will copy the 64-bit fat12.dll into the Formats subfolder of this folder. It must be the folder that contains 7z.exe and 7z.dll of a 64-bit 7-Zip."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "64-bit 7-Zip folder"
!define MUI_DIRECTORYPAGE_VARIABLE $Dir64
!define MUI_PAGE_CUSTOMFUNCTION_PRE Dir64Pre
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE Dir64Leave
!insertmacro MUI_PAGE_DIRECTORY

!define MUI_PAGE_HEADER_TEXT "32-bit 7-Zip"
!define MUI_PAGE_HEADER_SUBTEXT "Choose the folder of your 32-bit 7-Zip."
!define MUI_DIRECTORYPAGE_TEXT_TOP "Setup will copy the 32-bit fat12.dll into the Formats subfolder of this folder. It must be the folder that contains 7z.exe and 7z.dll of a 32-bit 7-Zip."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "32-bit 7-Zip folder"
!define MUI_DIRECTORYPAGE_VARIABLE $Dir32
!define MUI_PAGE_CUSTOMFUNCTION_PRE Dir32Pre
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE Dir32Leave
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TEXT "The plugin has been installed.$\r$\n$\r$\nTo check, run 7z i: the format list should include f12 with C (can create) in the flags column.$\r$\n$\r$\nIn the 7-Zip GUI, choose Add to archive and set Archive format to f12 to create a floppy image."
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

; -------------------------------------------------------------- helpers ----

; GetPEMachine <file> <outvar>: IMAGE_FILE_HEADER.Machine of a PE file, 0 if
; the file is missing or not a PE image.
!macro GetPEMachine FILE OUT
  Push "${FILE}"
  Call GetPEMachine
  Pop ${OUT}
!macroend
Function GetPEMachine
  Exch $0           ; file
  Push $1
  Push $2
  Push $3
  Push $4
  StrCpy $1 0       ; result
  ClearErrors
  FileOpen $2 $0 r
  ${IfNot} ${Errors}
    FileSeek $2 60 SET          ; IMAGE_DOS_HEADER.e_lfanew
    FileReadByte $2 $3
    FileReadByte $2 $4
    IntOp $4 $4 << 8
    IntOp $3 $3 | $4
    FileReadByte $2 $4
    IntOp $4 $4 << 16
    IntOp $3 $3 | $4
    FileReadByte $2 $4
    IntOp $4 $4 << 24
    IntOp $3 $3 | $4
    FileSeek $2 $3 SET
    FileReadByte $2 $3          ; "PE\0\0"
    FileReadByte $2 $4
    ${If} $3 = 80
    ${AndIf} $4 = 69
      FileReadByte $2 $3
      FileReadByte $2 $4
      ${If} $3 = 0
      ${AndIf} $4 = 0
        FileReadByte $2 $3
        FileReadByte $2 $4
        IntOp $4 $4 << 8
        IntOp $1 $3 | $4
      ${EndIf}
    ${EndIf}
    ${If} ${Errors}
      StrCpy $1 0
    ${EndIf}
    FileClose $2
  ${EndIf}
  StrCpy $0 $1
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Exch $0
FunctionEnd

; Bitness of the 7-Zip in a folder: machine of 7z.dll, else of 7z.exe.
Function Get7zMachine  ; in: $R0 = folder, out: $R1 = machine or 0
  !insertmacro GetPEMachine "$R0\7z.dll" $R1
  ${If} $R1 = 0
    !insertmacro GetPEMachine "$R0\7z.exe" $R1
  ${EndIf}
FunctionEnd

; Assign a candidate folder to $Dir64 / $Dir32 by its 7-Zip bitness.
Function ProbeDir  ; in: $R0 = folder
  ${If} $R0 == ""
    Return
  ${EndIf}
  Call Get7zMachine
  ${If} $R1 = ${MACHINE_X64}
  ${AndIf} $Dir64 == ""
    StrCpy $Dir64 $R0
  ${ElseIf} $R1 = ${MACHINE_X86}
  ${AndIf} $Dir32 == ""
    StrCpy $Dir32 $R0
  ${EndIf}
FunctionEnd

!macro ProbeReg ROOT
  ReadRegStr $R0 ${ROOT} "Software\7-Zip" "Path64"
  Call ProbeDir
  ReadRegStr $R0 ${ROOT} "Software\7-Zip" "Path"
  Call ProbeDir
  ReadRegStr $R0 ${ROOT} "Software\7-Zip" "Path32"
  Call ProbeDir
!macroend

Function Detect7Zip
  ${If} ${RunningX64}
    SetRegView 64
    !insertmacro ProbeReg HKLM
    !insertmacro ProbeReg HKCU
    SetRegView default   ; never SetRegView 32/64 on Windows 2000: unsupported flags
  ${EndIf}
  !insertmacro ProbeReg HKLM
  !insertmacro ProbeReg HKCU
  ${If} ${RunningX64}
    StrCpy $R0 "$PROGRAMFILES64\7-Zip"
    Call ProbeDir
  ${EndIf}
  StrCpy $R0 "$PROGRAMFILES32\7-Zip"
  Call ProbeDir
FunctionEnd

; Check a 7-Zip folder against the expected machine; Abort keeps the page open.
!macro CheckDir DIR MACHINE BITS
  StrCpy $R0 ${DIR}
  Call Get7zMachine
  ${If} $R1 = 0
    MessageBox MB_YESNO|MB_ICONEXCLAMATION "No 7-Zip was found in$\r$\n${DIR}$\r$\n$\r$\nInstall the ${BITS}-bit plugin there anyway?" IDYES +2
    Abort
  ${ElseIf} $R1 <> ${MACHINE}
    MessageBox MB_OK|MB_ICONSTOP "The 7-Zip in$\r$\n${DIR}$\r$\nis not ${BITS}-bit. 7-Zip can only load a plugin of its own bitness."
    Abort
  ${EndIf}
!macroend

; Copy fat12.dll into <7-Zip>\Formats.  A DLL that a running 7-Zip has loaded
; can't be overwritten, but it can be renamed; move it out of Formats (7-Zip
; tries to load every file there) and delete it at the next reboot.
!macro InstallDll ARCH DIR
  DetailPrint "Installing ${ARCH} plugin into ${DIR}\Formats"
  CreateDirectory "${DIR}\Formats"
  Delete "${DIR}\Formats\fat12.dll"
  ${If} ${FileExists} "${DIR}\Formats\fat12.dll"
    Delete "${DIR}\fat12.dll.old"
    Rename "${DIR}\Formats\fat12.dll" "${DIR}\fat12.dll.old"
    Delete /REBOOTOK "${DIR}\fat12.dll.old"
  ${EndIf}
  SetOutPath "${DIR}\Formats"
  File "/oname=fat12.dll" "${DIST}/${ARCH}/fat12.dll"
!macroend

; ------------------------------------------------------------- sections ----

Section "Plugin for 64-bit 7-Zip" SecX64
  ${If} ${Silent}
    !insertmacro CheckDir $Dir64 ${MACHINE_X64} 64
  ${EndIf}
  !insertmacro InstallDll x64 $Dir64
  WriteRegStr HKLM "${UNINSTKEY}" "Dir64" $Dir64
SectionEnd

Section "Plugin for 32-bit 7-Zip" SecX86
  ${If} ${Silent}
    !insertmacro CheckDir $Dir32 ${MACHINE_X86} 32
  ${EndIf}
  !insertmacro InstallDll x86 $Dir32
  WriteRegStr HKLM "${UNINSTKEY}" "Dir32" $Dir32
SectionEnd

Section "-Uninstaller"
  SetOutPath $INSTDIR
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" 700
SectionEnd

; ------------------------------------------------------------ callbacks ----

Function .onInit
  StrCpy $Dir64 ""
  StrCpy $Dir32 ""
  Call Detect7Zip

  ${IfNot} ${RunningX64}
    ; no 64-bit 7-Zip can run here
    SectionSetFlags ${SecX64} ${SF_RO}
    StrCpy $Dir64 ""
  ${EndIf}

  ${If} $Dir64 == ""
  ${AndIf} $Dir32 == ""
    ${If} ${Silent}
      MessageBox MB_OK|MB_ICONSTOP "7-Zip was not found." /SD IDOK
      SetErrorLevel 2
      Quit
    ${EndIf}
    ; nothing detected: offer the plugin matching the OS, let the user pick the folder
    ${If} ${RunningX64}
      !insertmacro UnselectSection ${SecX86}
      StrCpy $Dir64 "$PROGRAMFILES64\7-Zip"
    ${Else}
      StrCpy $Dir32 "$PROGRAMFILES32\7-Zip"
    ${EndIf}
  ${Else}
    ; install into every 7-Zip found
    ${If} $Dir64 == ""
      !insertmacro UnselectSection ${SecX64}
    ${EndIf}
    ${If} $Dir32 == ""
      !insertmacro UnselectSection ${SecX86}
    ${EndIf}
  ${EndIf}
  ${If} $Dir64 == ""
    StrCpy $Dir64 "$PROGRAMFILES64\7-Zip"
  ${EndIf}
  ${If} $Dir32 == ""
    StrCpy $Dir32 "$PROGRAMFILES32\7-Zip"
  ${EndIf}
FunctionEnd

Function ComponentsLeave
  ${IfNot} ${SectionIsSelected} ${SecX64}
  ${AndIfNot} ${SectionIsSelected} ${SecX86}
    MessageBox MB_OK|MB_ICONEXCLAMATION "Select at least one component."
    Abort
  ${EndIf}
FunctionEnd

Function Dir64Pre
  ${IfNot} ${SectionIsSelected} ${SecX64}
    Abort   ; skip page
  ${EndIf}
FunctionEnd

Function Dir64Leave
  !insertmacro CheckDir $Dir64 ${MACHINE_X64} 64
FunctionEnd

Function Dir32Pre
  ${IfNot} ${SectionIsSelected} ${SecX86}
    Abort
  ${EndIf}
FunctionEnd

Function Dir32Leave
  !insertmacro CheckDir $Dir32 ${MACHINE_X86} 32
FunctionEnd

; ------------------------------------------------------------ uninstall ----

!macro UninstallDll VALUE
  ReadRegStr $R0 HKLM "${UNINSTKEY}" "${VALUE}"
  ${If} $R0 != ""
    DetailPrint "Removing $R0\Formats\fat12.dll"
    Delete /REBOOTOK "$R0\Formats\fat12.dll"
    Delete /REBOOTOK "$R0\fat12.dll.old"
    RMDir "$R0\Formats"    ; only if empty
  ${EndIf}
!macroend

Section "Uninstall"
  !insertmacro UninstallDll Dir64
  !insertmacro UninstallDll Dir32
  DeleteRegKey HKLM "${UNINSTKEY}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
