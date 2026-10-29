[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$QtDirectory,
    [Parameter(Mandatory)][string]$PlatformTools,
    [Parameter(Mandatory)][string]$MakeNsis,
    [Parameter(Mandatory)][string]$CrtDirectory,
    [string]$CMake='cmake',
    [string]$BuildDirectory=(Join-Path $PSScriptRoot '../build-stage6'),
    [string]$Apk=(Join-Path $PSScriptRoot '../../android/app/build/outputs/apk/debug/app-debug.apk')
)
$ErrorActionPreference='Stop'
try {
    $padHostRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    $padBuild=(Resolve-Path -LiteralPath $BuildDirectory).Path
    $padQt=(Resolve-Path -LiteralPath $QtDirectory).Path
    # Always build current source; never copy build-v143 or stale stage-3 binaries.
    & $CMake -S $padHostRoot -B $padBuild -DSECONDARYSCREEN_BUILD_GUI=ON "-DCMAKE_PREFIX_PATH=$padQt"
    if($LASTEXITCODE){throw 'Configure failed'}
    & $CMake --build $padBuild --config Release
    if($LASTEXITCODE){throw 'Release build failed'}
    $padStamp=Get-Date -Format 'yyyyMMdd-HHmmss'
    $padDist=Join-Path $padBuild "dist/$padStamp"
    $padPayload=Join-Path $padDist 'payload'
    New-Item -ItemType Directory -Path $padPayload -Force | Out-Null
    foreach($padExe in @('SecondaryScreen.exe','secondaryscreen-host.exe')){Copy-Item -LiteralPath (Join-Path $padBuild "Release/$padExe") -Destination $padPayload}
    & (Join-Path $padQt 'bin/windeployqt.exe') --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime (Join-Path $padPayload 'SecondaryScreen.exe')
    if($LASTEXITCODE){throw 'Qt release deployment failed'}
    foreach($padDll in @('msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll','concrt140.dll')) {
        Copy-Item -LiteralPath (Join-Path $CrtDirectory $padDll) -Destination $padPayload
    }
    $padAdb=Join-Path $padPayload 'platform-tools'
    New-Item -ItemType Directory -Path $padAdb | Out-Null
    foreach($padFile in @('adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll','NOTICE.txt','source.properties')){Copy-Item -LiteralPath (Join-Path $PlatformTools $padFile) -Destination $padAdb}
    Copy-Item -LiteralPath $Apk -Destination (Join-Path $padPayload 'SecondaryScreen.apk')
    $padReadme=Get-Content -LiteralPath (Join-Path $padHostRoot '../README.md') -Raw -Encoding UTF8
    $padUserReadme=([regex]::Split($padReadme,'(?m)^## 构建与诊断'))[0]+"## 诊断`n`n安装目录 scripts/diagnose.ps1 可检查系统、显示器、adb 和端口；不改变显示设置或映射。许可证见 THIRD-PARTY-NOTICES.txt。`n"
    [IO.File]::WriteAllText((Join-Path $padPayload 'README.md'),$padUserReadme,[Text.UTF8Encoding]::new($false))
    $padScripts=Join-Path $padPayload 'scripts'
    New-Item -ItemType Directory -Path $padScripts | Out-Null
    foreach($padFile in @('install-vdd.ps1','uninstall-vdd.ps1','VddDeviceInstaller.cs','diagnose.ps1')){Copy-Item -LiteralPath (Join-Path $padHostRoot "scripts/$padFile") -Destination $padScripts}
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vdd-action.ps1') -Destination $padScripts
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'stop-owned-adb.ps1') -Destination $padScripts
    $padAssets=Join-Path $padPayload 'assets';New-Item -ItemType Directory -Path $padAssets | Out-Null
    Copy-Item -LiteralPath (Join-Path $padHostRoot 'assets/vdd_settings.xml') -Destination $padAssets
    $padLicense=Join-Path $padPayload 'licenses';New-Item -ItemType Directory -Path $padLicense | Out-Null
    Copy-Item -LiteralPath (Join-Path $padHostRoot 'third_party/nlohmann/LICENSE.MIT') -Destination (Join-Path $padLicense 'nlohmann-json-MIT.txt')
    Copy-Item -LiteralPath (Join-Path $padHostRoot 'third_party/qt-licenses') -Destination $padLicense -Recurse
    Copy-Item -LiteralPath (Join-Path $padQt 'sbom/qtbase-6.8.3.spdx.json') -Destination $padLicense
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD-PARTY-NOTICES.txt') -Destination $padPayload
    if(Get-ChildItem -LiteralPath $padPayload -Recurse -File | Where-Object {$_.Name -match '^(Qt6.+d|qwindowsd|msvcp\d+d|vcruntime\d+d)\.dll$'}){throw 'Debug DLL found in release payload'}
    $padManifest=[ordered]@{BuiltAt=(Get-Date).ToString('o');QtVersion='6.8.3';GitRevision=(& git -C $padHostRoot rev-parse HEAD);Source=@();Payload=@()}
    $padSources=@(Get-ChildItem (Join-Path $padHostRoot 'src'),(Join-Path $padHostRoot 'gui'),(Join-Path $padHostRoot 'cmake') -Recurse -File)
    $padSources+=Get-Item (Join-Path $padHostRoot 'CMakeLists.txt')
    $padSources+=Get-Item (Join-Path $padHostRoot 'third_party/nlohmann/json.hpp')
    foreach($padFile in $padSources){$padManifest.Source+=@{Path=$padFile.FullName.Substring($padHostRoot.Length+1);SHA256=(Get-FileHash -LiteralPath $padFile.FullName).Hash}}
    foreach($padFile in Get-ChildItem -LiteralPath $padPayload -Recurse -File){$padManifest.Payload+=@{Path=$padFile.FullName.Substring($padPayload.Length+1);SHA256=(Get-FileHash -LiteralPath $padFile.FullName).Hash}}
    $padManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $padPayload 'build-manifest.json') -Encoding UTF8
    # Generate exact owned-file deletion instructions; never recursively delete INSTDIR.
    $padDelete=New-Object 'Collections.Generic.List[string]'
    foreach($padFile in Get-ChildItem -LiteralPath $padPayload -Recurse -File){$padRelative=$padFile.FullName.Substring($padPayload.Length+1);$padDelete.Add('Delete "$INSTDIR\'+$padRelative+'"')}
    foreach($padDir in Get-ChildItem -LiteralPath $padPayload -Recurse -Directory | Sort-Object {$_.FullName.Length} -Descending){$padDelete.Add('RMDir "$INSTDIR\'+$padDir.FullName.Substring($padPayload.Length+1)+'"')}
    $padOwned=Join-Path $padDist 'owned-files.nsh';$padDelete | Set-Content -LiteralPath $padOwned -Encoding UTF8
    $padSetup=Join-Path $padDist 'SecondaryScreen-Setup.exe'
    & $MakeNsis /INPUTCHARSET UTF8 "/DPAYLOAD=$padPayload" "/DOWNED_FILES=$padOwned" "/DOUTPUT=$padSetup" (Join-Path $PSScriptRoot 'SecondaryScreen.nsi')
    if($LASTEXITCODE){throw 'NSIS failed'}
    if((Get-Item -LiteralPath $padSetup).Length -ge 60000000){throw 'Installer exceeds 60 MB'}
    [ordered]@{Installer=$padSetup;Bytes=(Get-Item -LiteralPath $padSetup).Length;SHA256=(Get-FileHash -LiteralPath $padSetup).Hash;Payload=$padPayload} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $padDist 'package-result.json') -Encoding UTF8
    Write-Output "Installer: $padSetup"
    Write-Output "Payload and matching source/binary hashes: $padPayload/build-manifest.json"
} catch {Write-Error $_ -ErrorAction Continue;exit 1}
