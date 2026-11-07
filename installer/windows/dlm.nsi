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
!include "LogicLib.nsh"

!define APPNAME    "dlm"
!define COMPANY    "dlm project"
!define UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\dlm"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin       ; per-machine: Program Files + HKLM
; The previous install dir is read from HKLM in .onInit, AFTER SetRegView 64
; (an InstallDirRegKey attribute would be read before it, in the 32-bit view).

!if "${ARCH64}" == "1"
  !define DEFAULT_INSTDIR "$PROGRAMFILES64\dlm"
!else
  !define DEFAULT_INSTDIR "$PROGRAMFILES\dlm"
!endif
InstallDir "${DEFAULT_INSTDIR}"

; ---- appearance ----------------------------------------------------------
!define MUI_ICON   "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
; The installer runs elevated; launching dlm-gui directly would run it (and
; the dlmd it spawns) as administrator. Go through explorer.exe instead, which
; starts it as the normal logged-in user.
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Launch dlm now"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchDlmAsUser

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

; ---- helpers -------------------------------------------------------------
; Stop a running dlm before files are replaced/removed (open .exe files can't
; be overwritten on Windows). The GUI goes first, since it would respawn the
; daemon; then ask the daemon to shut down cleanly ("dlm shutdown", which
; checkpoints active downloads), wait for it, and only then force-kill what is
; left (e.g. a daemon owned by another user, which "dlm shutdown" can't reach).
!macro StopDlm
  nsExec::Exec '"$SYSDIR\taskkill.exe" /IM dlm-gui.exe'
  Pop $0
  Sleep 1000
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM dlm-gui.exe'
  Pop $0
  ${If} ${FileExists} "$INSTDIR\bin\dlm.exe"
    nsExec::Exec '"$INSTDIR\bin\dlm.exe" shutdown'
    Pop $0
  ${EndIf}
  StrCpy $1 0
  ${Do}
    ; exit code 0 = a dlmd.exe process is still listed
    ; (the command after /C must not start with a quote: cmd would strip it)
    nsExec::Exec '"$SYSDIR\cmd.exe" /C tasklist /NH /FI "IMAGENAME eq dlmd.exe" | find /I "dlmd.exe"'
    Pop $0
    ${If} $0 != 0
      ${Break}
    ${EndIf}
    IntOp $1 $1 + 1
    ${If} $1 >= 20
      ${Break}
    ${EndIf}
    Sleep 500
  ${Loop}
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM dlmd.exe'
  Pop $0
  Sleep 500
!macroend

Function LaunchDlmAsUser
  SetOutPath "$INSTDIR\bin"
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\bin\dlm-gui.exe"'
FunctionEnd

; ---- install -------------------------------------------------------------
Function .onInit
  !if "${ARCH64}" == "1"
    ${IfNot} ${RunningX64}
      MessageBox MB_ICONSTOP "This is the 64-bit build of dlm; use the 32-bit installer on this system."
      Abort
    ${EndIf}
    SetRegView 64
  !endif
  ; per-machine install: shortcuts go to the all-users Start Menu / Desktop
  SetShellVarContext all
  ; upgrade: reuse the previous install dir, unless /D= was given
  ${If} $INSTDIR == "${DEFAULT_INSTDIR}"
    ReadRegStr $0 HKLM "Software\dlm" "InstallDir"
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "dlm (required)" SecCore
  SectionIn RO
  !insertmacro StopDlm

  ; older installers put the shortcuts in the installing admin's own profile;
  ; remove those so an upgrade doesn't leave duplicates behind
  SetShellVarContext current
  Delete "$DESKTOP\dlm.lnk"
  Delete "$SMPROGRAMS\dlm\dlm.lnk"
  Delete "$SMPROGRAMS\dlm\Uninstall dlm.lnk"
  RMDir  "$SMPROGRAMS\dlm"
  SetShellVarContext all

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
  SetShellVarContext all
FunctionEnd

Section "Uninstall"
  !insertmacro StopDlm

  ; shortcuts from older (per-user) installs, then the all-users ones
  SetShellVarContext current
  Delete "$DESKTOP\dlm.lnk"
  Delete "$SMPROGRAMS\dlm\dlm.lnk"
  Delete "$SMPROGRAMS\dlm\Uninstall dlm.lnk"
  RMDir  "$SMPROGRAMS\dlm"
  SetShellVarContext all

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
