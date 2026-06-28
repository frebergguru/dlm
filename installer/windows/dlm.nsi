; dlm Windows installer (NSIS / Modern UI 2).
; Driven by build.sh, which passes: VERSION, ARCH (x64|x86), ARCH64 (1|0),
; STAGE (staged payload dir), SRC (source tree), ICON (.ico), OUTFILE.
;
; Provides: Program Files install, Start Menu entry, optional Desktop icon,
; an uninstaller, and a proper Add/Remove Programs (Apps & features) entry.

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!define APPNAME    "dlm"
!define COMPANY    "dlm project"
!define UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\dlm"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin       ; per-machine: Program Files + HKLM
InstallDirRegKey HKLM "Software\dlm" "InstallDir"

!if "${ARCH64}" == "1"
  InstallDir "$PROGRAMFILES64\dlm"
!else
  InstallDir "$PROGRAMFILES\dlm"
!endif

; ---- appearance ----------------------------------------------------------
!define MUI_ICON   "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\dlm-gui.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch dlm now"

; ---- pages ---------------------------------------------------------------
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---- install -------------------------------------------------------------
Function .onInit
  !if "${ARCH64}" == "1"
    ${IfNot} ${RunningX64}
      MessageBox MB_ICONSTOP "This is the 64-bit build of dlm; use the 32-bit installer on this system."
      Abort
    ${EndIf}
    SetRegView 64
  !endif
FunctionEnd

Section "dlm (required)" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"

  ; Start Menu (working dir = bin so GTK resolves its data next to the exe)
  CreateDirectory "$SMPROGRAMS\dlm"
  SetOutPath "$INSTDIR\bin"
  CreateShortCut "$SMPROGRAMS\dlm\dlm.lnk" "$INSTDIR\bin\dlm-gui.exe" "" "$INSTDIR\bin\dlm-gui.exe" 0
  CreateShortCut "$SMPROGRAMS\dlm\Uninstall dlm.lnk" "$INSTDIR\uninstall.exe"

  ; record + uninstaller
  WriteRegStr HKLM "Software\dlm" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Add/Remove Programs (Apps & features)
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayName"     "dlm — download manager"
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKLM "${UNINSTKEY}" "Publisher"       "${COMPANY}"
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayIcon"     "$INSTDIR\bin\dlm-gui.exe,0"
  WriteRegStr   HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKLM "${UNINSTKEY}" "UninstallString"      '"$INSTDIR\uninstall.exe"'
  WriteRegStr   HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
  SetOutPath "$INSTDIR\bin"
  CreateShortCut "$DESKTOP\dlm.lnk" "$INSTDIR\bin\dlm-gui.exe" "" "$INSTDIR\bin\dlm-gui.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore}    "The dlm application, GTK runtime and Start Menu entry (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Place a dlm shortcut on the Desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---- uninstall -----------------------------------------------------------
Function un.onInit
  !if "${ARCH64}" == "1"
    SetRegView 64
  !endif
FunctionEnd

Section "Uninstall"
  Delete "$DESKTOP\dlm.lnk"
  Delete "$SMPROGRAMS\dlm\dlm.lnk"
  Delete "$SMPROGRAMS\dlm\Uninstall dlm.lnk"
  RMDir  "$SMPROGRAMS\dlm"

  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\share"
  Delete "$INSTDIR\uninstall.exe"
  RMDir  "$INSTDIR"

  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\dlm"
SectionEnd
