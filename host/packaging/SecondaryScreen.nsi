Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!ifndef PAYLOAD
 !error "Define PAYLOAD, OWNED_FILES and OUTPUT at build time"
!endif
Name "SecondaryScreen"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\SecondaryScreen"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\SecondaryScreen.exe"
!define MUI_FINISHPAGE_RUN_TEXT "运行 SecondaryScreen"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD}\THIRD-PARTY-NOTICES.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"
Function .onInit
 ${IfNot} ${RunningX64}
  MessageBox MB_ICONSTOP "SecondaryScreen 需要 64 位 Windows。"
  Abort
 ${EndIf}
 SetShellVarContext current
 SetRegView 64
 ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "InstallLocation"
 StrCmp $0 "" new_install
 MessageBox MB_ICONSTOP "已安装 SecondaryScreen。请先卸载旧版，可选择保留配置。"
 Abort
 new_install:
FunctionEnd
Section "主程序、Qt 和 Android Platform Tools（必需）" Main
 SectionIn RO
 IfFileExists "$INSTDIR\install-state.ini" 0 fresh
  MessageBox MB_ICONSTOP "请先卸载现有版本，再安装此版本；用户配置可选择保留。"
  Abort
 fresh:
 SetOutPath "$INSTDIR"
 File /r "${PAYLOAD}\*.*"
 WriteINIStr "$INSTDIR\install-state.ini" "Install" "Root" "$INSTDIR"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "DisplayName" "SecondaryScreen"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "DisplayVersion" "0.7.0"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "InstallLocation" "$INSTDIR"
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
 WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "NoModify" 1
 WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "NoRepair" 1
 CreateDirectory "$SMPROGRAMS\SecondaryScreen"
 CreateShortcut "$SMPROGRAMS\SecondaryScreen\SecondaryScreen.lnk" "$INSTDIR\SecondaryScreen.exe"
 CreateShortcut "$SMPROGRAMS\SecondaryScreen\卸载 SecondaryScreen.lnk" "$INSTDIR\Uninstall.exe"
 CreateShortcut "$DESKTOP\SecondaryScreen.lnk" "$INSTDIR\SecondaryScreen.exe"
 WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd
Section /o "虚拟显示器驱动 VDD（需要管理员权限）" VDD
 nsExec::ExecToLog '$\"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe$\" -NoProfile -ExecutionPolicy Bypass -File $\"$INSTDIR\scripts\vdd-action.ps1$\" -Action install'
 Pop $0
 ${If} $0 == 0
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "OwnedVdd" 1
 ${Else}
  MessageBox MB_ICONEXCLAMATION "VDD 安装未完成。主程序已安装；可稍后运行安装目录 scripts 中的 vdd-action.ps1 重试。"
 ${EndIf}
SectionEnd
Section /o "登录 Windows 时自动启动" Startup
 WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "SecondaryScreen" '$\"$INSTDIR\SecondaryScreen.exe$\"'
SectionEnd
Function un.onInit
 SetShellVarContext current
 SetRegView 64
 ReadINIStr $0 "$INSTDIR\install-state.ini" "Install" "Root"
 ReadRegStr $1 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "InstallLocation"
 StrCmp $0 "$INSTDIR" 0 unsafe
 StrCmp $1 "$INSTDIR" 0 unsafe
 Return
 unsafe:
 MessageBox MB_ICONSTOP "无法确认安装目录所有权。为保护文件，已停止卸载。"
 Abort
FunctionEnd
Section "Uninstall"
 ExecWait '$\"$INSTDIR\SecondaryScreen.exe$\" --quit' $0
 ${If} $0 != 0
  MessageBox MB_ICONSTOP "SecondaryScreen 尚未退出。请关闭程序后重试；没有删除安装文件。"
  Abort
 ${EndIf}
 ReadRegDWORD $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen" "OwnedVdd"
 ${If} $0 == 1
  MessageBox MB_YESNO|MB_DEFBUTTON2 "同时卸载本安装程序安装的 VDD 驱动吗？" /SD IDNO IDNO keep_driver
  nsExec::ExecToLog '$\"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe$\" -NoProfile -ExecutionPolicy Bypass -File $\"$INSTDIR\scripts\vdd-action.ps1$\" -Action uninstall'
  Pop $0
  ${If} $0 != 0
   MessageBox MB_ICONSTOP "驱动卸载未完成；保留安装文件以便重试。"
   Abort
  ${EndIf}
 keep_driver:
 ${EndIf}
 nsExec::ExecToLog '$\"$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe$\" -NoProfile -ExecutionPolicy Bypass -File $\"$INSTDIR\scripts\stop-owned-adb.ps1$\" -InstallDirectory $\"$INSTDIR$\"'
 Pop $0
 ${If} $0 != 0
  MessageBox MB_ICONSTOP "无法释放本安装目录的 adb。请关闭相关进程后重试；安装文件已保留。"
  Abort
 ${EndIf}
 DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "SecondaryScreen"
 DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SecondaryScreen"
 Delete "$DESKTOP\SecondaryScreen.lnk"
 Delete "$SMPROGRAMS\SecondaryScreen\SecondaryScreen.lnk"
 Delete "$SMPROGRAMS\SecondaryScreen\卸载 SecondaryScreen.lnk"
 RMDir "$SMPROGRAMS\SecondaryScreen"
 MessageBox MB_YESNO|MB_DEFBUTTON1 "保留用户配置和日志吗？" /SD IDNO IDYES keep_config
  StrCmp "$APPDATA" "" unsafe_data
  GetFullPathName $0 "$APPDATA\SecondaryScreen"
  GetFullPathName $1 "$APPDATA"
  ${GetParent} "$0" $2
  StrCmp $1 $2 0 unsafe_data
  RMDir /r "$0"
  Goto keep_config
 unsafe_data:
  MessageBox MB_ICONEXCLAMATION "无法确认用户配置路径，已保留。"
 keep_config:
 !include "${OWNED_FILES}"
 ; Runtime extraction of the pinned four-file VDD package, never arbitrary paths.
 Delete "$INSTDIR\vdd-cache\package\VirtualDisplayDriver\MttVDD.inf"
 Delete "$INSTDIR\vdd-cache\package\VirtualDisplayDriver\MttVDD.dll"
 Delete "$INSTDIR\vdd-cache\package\VirtualDisplayDriver\mttvdd.cat"
 Delete "$INSTDIR\vdd-cache\package\VirtualDisplayDriver\vdd_settings.xml"
 RMDir "$INSTDIR\vdd-cache\package\VirtualDisplayDriver"
 RMDir "$INSTDIR\vdd-cache\package"
 RMDir "$INSTDIR\vdd-cache"
 Delete "$INSTDIR\install-state.ini"
 Delete "$INSTDIR\Uninstall.exe"
 RMDir "$INSTDIR"
SectionEnd
