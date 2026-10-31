# SecondaryScreen adb startup diagnostic / adb 启动失败诊断
# Collects adb processes, port 5037 owners, adb version and a manual
# `adb start-server` attempt into adb-diagnosis.txt next to this script.
# Read-only, except an optional kill of stale adb.exe processes
# (prompted interactively, or -Force for unattended runs).
# 用法：双击 adb-diagnose.bat，或 powershell -File tools\adb-diagnose.ps1 [-Force]
# 注意：报告包含本机路径（用户名已做替换处理）；分享前请打开报告自行检查。
[CmdletBinding()]
param([switch]$Force)
$ErrorActionPreference = 'Continue'
$report = Join-Path $PSScriptRoot 'adb-diagnosis.txt'
$lines = New-Object 'Collections.Generic.List[string]'
function Mask([string]$text) {
    if ([string]::IsNullOrEmpty($env:USERNAME)) { return $text }
    return ($text -replace [regex]::Escape($env:USERNAME), '<user>')
}
function W([string]$text) { $m = Mask $text; $lines.Add($m); Write-Host $m }

W "=== adb diagnosis $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="
W 'NOTE: this report contains local paths (username masked). Review before sharing.'
W "OS: $([Environment]::OSVersion.VersionString)  Admin: $(([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"

# 安装位置：注册表（当前用户与机器级）
$install = $null
foreach ($root in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')) {
    Get-ChildItem -Path $root -ErrorAction SilentlyContinue | ForEach-Object {
        $p = Get-ItemProperty -Path $_.PSPath -ErrorAction SilentlyContinue
        if ($p -and $p.DisplayName -eq 'SecondaryScreen' -and $p.InstallLocation) { $install = $p.InstallLocation }
    }
}
W "InstallLocation(registry): $install"

# 找到实际使用的 adb：安装目录优先，其次 PATH
$candidates = @()
if ($install) { $candidates += (Join-Path $install 'platform-tools\adb.exe') }
$candidates += (Join-Path $env:LOCALAPPDATA 'Programs\SecondaryScreen\platform-tools\adb.exe')
$pathAdb = Get-Command adb.exe -ErrorAction SilentlyContinue
if ($pathAdb) { $candidates += $pathAdb.Source }
$adb = $null
foreach ($c in $candidates) { if ($c -and (Test-Path -LiteralPath $c)) { $adb = $c; break } }
W "adb used: $adb"

if ($adb) {
    W "adb file version: $((Get-Item -LiteralPath $adb).VersionInfo.FileVersion)"
    W 'platform-tools dir:'
    Get-ChildItem -LiteralPath (Split-Path $adb) -ErrorAction SilentlyContinue | ForEach-Object { W ('  ' + $_.Name) }
}

W ''
W '--- SecondaryScreen processes ---'
$appProcs = Get-Process -Name SecondaryScreen -ErrorAction SilentlyContinue
if ($appProcs) { $appProcs | ForEach-Object { W ("pid={0} path={1} start={2}" -f $_.Id, $_.Path, $_.StartTime) } } else { W '(none)' }
W '--- adb processes ---'
$adbProcs = Get-Process -Name adb -ErrorAction SilentlyContinue
if ($adbProcs) { $adbProcs | ForEach-Object { W ("pid={0} path={1} start={2}" -f $_.Id, $_.Path, $_.StartTime) } } else { W '(none)' }
W '--- port 5037 ---'
try {
    $conns = Get-NetTCPConnection -LocalPort 5037 -ErrorAction SilentlyContinue
    if ($conns) {
        foreach ($c in $conns) {
            $owner = Get-Process -Id $c.OwningProcess -ErrorAction SilentlyContinue
            W ("{0}:{1} {2} pid={3} owner={4} path={5}" -f $c.LocalAddress, $c.LocalPort, $c.State, $c.OwningProcess, $owner.ProcessName, $owner.Path)
        }
    } else { W '(port 5037 not in use)' }
} catch { W ('Get-NetTCPConnection failed: ' + $_.Exception.Message) }
( netstat -ano | Select-String ':5037' ) | ForEach-Object { W ('netstat: ' + $_.Line.Trim()) }

if (!$adb) {
    W '!! adb.exe not found; cannot continue'
    $lines | Set-Content -LiteralPath $report -Encoding UTF8
    Write-Host "Report saved to $report"
    exit 0
}

W ''
W '--- adb version ---'
& $adb version 2>&1 | ForEach-Object { W $_ }

# 残留的 adb 服务是 5037 冲突最常见的来源；结束前征求同意（-Force 跳过）。
if ($adbProcs) {
    $kill = [bool]$Force
    if (!$kill) {
        $answer = Read-Host '发现 adb 进程。结束所有 adb.exe 以排除残留服务？会中断 Android Studio 等正在使用 adb 的工具 [y/N]'
        $kill = $answer -match '^(y|Y)'
    }
    if ($kill) {
        W '--- kill every adb.exe ---'
        taskkill /F /IM adb.exe 2>&1 | ForEach-Object { W $_ }
        Start-Sleep -Seconds 1
    } else {
        W '(keep existing adb processes; start-server test may observe the old daemon)'
    }
}

function Run-Adb([string]$arguments, [int]$timeoutMs) {
    $outFile = Join-Path $env:TEMP 'adb-diag-out.txt'
    $errFile = Join-Path $env:TEMP 'adb-diag-err.txt'
    Remove-Item -LiteralPath $outFile, $errFile -ErrorAction SilentlyContinue
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $adb -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $finished = $p.WaitForExit($timeoutMs)
    $watch.Stop()
    if (!$finished) {
        try { $p.Kill() } catch { }
        W ("adb {0} -> TIMEOUT after {1} ms (killed)" -f $arguments, $watch.ElapsedMilliseconds)
    } else {
        W ("adb {0} -> exit={1} in {2} ms" -f $arguments, $p.ExitCode, $watch.ElapsedMilliseconds)
    }
    foreach ($f in @($outFile, $errFile)) {
        if (Test-Path -LiteralPath $f) { Get-Content -LiteralPath $f -ErrorAction SilentlyContinue | ForEach-Object { if ($_ -ne '') { W ('  | ' + $_) } } }
    }
}

W ''
W '--- adb start-server (30s budget) ---'
Run-Adb 'start-server' 30000
W '--- adb devices -l (15s budget) ---'
Run-Adb 'devices -l' 15000

W ''
W '--- adb processes after test ---'
$adbProcs = Get-Process -Name adb -ErrorAction SilentlyContinue
if ($adbProcs) { $adbProcs | ForEach-Object { W ("pid={0} path={1}" -f $_.Id, $_.Path) } } else { W '(none)' }
W '--- port 5037 after test ---'
( netstat -ano | Select-String ':5037' ) | ForEach-Object { W ('netstat: ' + $_.Line.Trim()) }
W ''
W '=== done ==='

$lines | Set-Content -LiteralPath $report -Encoding UTF8
Write-Host "Report saved to $report"
