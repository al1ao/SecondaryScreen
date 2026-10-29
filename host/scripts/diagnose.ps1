[CmdletBinding()]
param([ValidateRange(1,65535)][int]$Port=51900)
$ErrorActionPreference='Continue'
Write-Output 'SecondaryScreen 环境诊断（不修改显示设置或 USB 映射）'
Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version
Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate
Get-PnpDevice -Class Display | Select-Object FriendlyName,Status
$padLocal=Join-Path $PSScriptRoot '../platform-tools/adb.exe'
$padAdb=if(Test-Path -LiteralPath $padLocal){$padLocal}else{(Get-Command adb -ErrorAction SilentlyContinue).Source}
if($padAdb){& $padAdb devices -l;& $padAdb reverse --list}else{Write-Output '未找到 adb；请使用包含 platform-tools 的安装包。'}
Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue | Select-Object LocalAddress,LocalPort,OwningProcess
Get-Process -Name dwm,explorer -ErrorAction SilentlyContinue | Select-Object ProcessName,SessionId
Write-Output 'WGC 需要可访问桌面的交互会话；会话编号本身不能判断权限。'
