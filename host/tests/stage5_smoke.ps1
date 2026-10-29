[CmdletBinding()]
param([Parameter(Mandatory)][string]$Setup,[Parameter(Mandatory)][string]$Payload,
    [string]$BuildDirectory=(Join-Path $PSScriptRoot '../build-stage5'),[switch]$InstallationOnly)
$ErrorActionPreference='Stop'
$padGui=$null
function Check([bool]$Value,[string]$Message){if(!$Value){throw $Message}}
function HiddenRun([string]$Exe,[string[]]$Arguments,[int]$TimeoutMs=30000){
    $padChild=Start-Process -FilePath $Exe -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if(!$padChild.WaitForExit($TimeoutMs)){Stop-Process -Id $padChild.Id;throw "Owned test process timed out: $Exe"}
    $padChild.WaitForExit();Check ($padChild.ExitCode -eq 0) "Process exit=$($padChild.ExitCode): $Exe"
}
try {
    $padBuild=(Resolve-Path -LiteralPath $BuildDirectory).Path
    $padRoot=(Resolve-Path -LiteralPath $Payload).Path
    $padInstall=Join-Path $padBuild 'install smoke'
    $padFixture=Join-Path $padBuild ('simulated-adb-integration-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
    $padEvidence=Join-Path $padBuild 'evidence'
    New-Item -ItemType Directory -Path $padEvidence -Force | Out-Null
    $padUser=Join-Path $env:APPDATA 'SecondaryScreen'
    $padRegistry='HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/SecondaryScreen'
    $padDesktop=Join-Path ([Environment]::GetFolderPath('Desktop')) 'SecondaryScreen.lnk'
    $padMenu=Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs/SecondaryScreen'
    foreach($padPath in @($padInstall,$padFixture,$padUser,$padRegistry,$padDesktop,$padMenu)){Check (!(Test-Path -LiteralPath $padPath)) "Existing user data/install found; refusing test: $padPath"}
    Check (!(Get-Process -Name SecondaryScreen -ErrorAction SilentlyContinue)) 'Existing GUI found; refusing to stop it'
    Check (!(Get-ItemProperty 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Run' -Name SecondaryScreen -ErrorAction SilentlyContinue)) 'Existing startup entry found'
    Check (!(Get-NetTCPConnection -LocalPort 52150 -State Listen -ErrorAction SilentlyContinue)) 'Integration port occupied'
    Check (!(Get-NetTCPConnection -LocalPort 52151 -State Listen -ErrorAction SilentlyContinue)) 'Owned adb test port occupied'
    HiddenRun (Resolve-Path -LiteralPath $Setup).Path @('/S',"/D=$padInstall")
    Check (Test-Path -LiteralPath (Join-Path $padInstall 'Uninstall.exe')) 'Uninstaller missing'
    $padManifest=Get-Content -LiteralPath (Join-Path $padInstall 'build-manifest.json') -Raw | ConvertFrom-Json
    foreach($padFile in $padManifest.Payload){Check ((Get-FileHash -LiteralPath (Join-Path $padInstall $padFile.Path)).Hash -eq $padFile.SHA256) "Installed hash differs: $($padFile.Path)"}
    Write-Output 'PASS installed payload matches manifest, including bundled adb/APK/Qt; no debug DLLs'
    New-Item -ItemType Directory -Path $padUser | Out-Null
    $padConfig=[ordered]@{version=1;display=@{width=1920;height=1200;fps=60;primary=$false;autoResize=$false;monitorId=''};encoder=@{bitrateKbps=10000};connection=@{port=52150;deviceSerial='';autoReverse=$true;foreground=$false};app=@{autoStart=$false}}
    $padConfig | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $padUser 'config.json') -Encoding UTF8
    $padGui=Start-Process -FilePath (Join-Path $padInstall 'SecondaryScreen.exe') -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 5
    Check (!$padGui.HasExited) 'Installed tray failed to stay alive'
    HiddenRun (Join-Path $padInstall 'SecondaryScreen.exe') @('--quit') 20000
    Check ($padGui.WaitForExit(5000)) 'QLocalServer quit did not release owner'
    $padGui.WaitForExit()
    Copy-Item -LiteralPath (Join-Path $padUser 'logs/secondaryscreen.log') -Destination (Join-Path $padEvidence 'installed-gui.txt')
    Write-Output 'PASS installed GUI start and single-instance --quit; actual adb status retained in installed-gui.txt'
    HiddenRun (Join-Path $padInstall 'platform-tools/adb.exe') @('-P','52151','start-server')
    $padOwnedAdb=Get-NetTCPConnection -LocalPort 52151 -State Listen -ErrorAction SilentlyContinue
    Check ([bool]$padOwnedAdb) 'Isolated owned adb daemon did not start'
    $padOtherAdb=@(Get-CimInstance Win32_Process -Filter "Name='adb.exe'" | Where-Object {$_.ExecutablePath -and $_.ExecutablePath -ne (Join-Path $padInstall 'platform-tools/adb.exe')})
    HiddenRun (Join-Path $padInstall 'Uninstall.exe') @('/S')
    for($padAttempt=0;$padAttempt -lt 100 -and (Test-Path -LiteralPath $padInstall);++$padAttempt){Start-Sleep -Milliseconds 100}
    foreach($padPath in @($padInstall,$padUser,$padRegistry,$padDesktop,$padMenu)){Check (!(Test-Path -LiteralPath $padPath)) "Uninstall residue: $padPath"}
    Check (!(Get-ItemProperty 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Run' -Name SecondaryScreen -ErrorAction SilentlyContinue)) 'Startup residue'
    Check (!(Get-NetTCPConnection -LocalPort 52151 -State Listen -ErrorAction SilentlyContinue)) 'Owned adb daemon survived uninstall'
    foreach($padOther in $padOtherAdb){Check ([bool](Get-CimInstance Win32_Process -Filter "ProcessId=$($padOther.ProcessId)" | Where-Object {$_.CreationDate -eq $padOther.CreationDate})) 'Unrelated adb process was stopped'}
    Write-Output 'PASS uninstall stops own-path adb daemon on isolated port and preserves other-path adb processes'
    Write-Output 'PASS current-user install/uninstall in workspace path with spaces, files/config/logs/shortcuts/registry removed; no VDD selected'
    if($InstallationOnly){Write-Output 'InstallationOnly: virtual-display GUI video integration not requested; hardware CTest is separate';exit 0}
    Copy-Item -LiteralPath $padRoot -Destination $padFixture -Recurse
    Copy-Item -LiteralPath (Join-Path $padBuild 'Release/secondaryscreen-stage5-tests.exe') -Destination (Join-Path $padFixture 'platform-tools/adb.exe') -Force
    Set-Content -LiteralPath (Join-Path $padFixture 'SIMULATED-ADB-NOT-DISTRIBUTABLE.txt') -Value 'USB readiness fixture only. Release payload elsewhere remains unmodified.'
    New-Item -ItemType Directory -Path $padUser | Out-Null
    $padConfig | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $padUser 'config.json') -Encoding UTF8
    $padGui=Start-Process -FilePath (Join-Path $padFixture 'SecondaryScreen.exe') -WindowStyle Hidden -PassThru
    $padListening=$false
    for($padAttempt=0;$padAttempt -lt 80;++$padAttempt){
        $padConnection=Get-NetTCPConnection -LocalPort 52150 -State Listen -ErrorAction SilentlyContinue
        if($padConnection -and $padConnection.OwningProcess -eq $padGui.Id){$padListening=$true;break}
        Start-Sleep -Milliseconds 100
    }
    Check $padListening 'GUI HostService did not start with simulated USB readiness'
    $padProbe=Start-Process -FilePath (Join-Path $padBuild 'Release/secondaryscreen-stage2-probe.exe') -ArgumentList '--port','52150','--seconds','10','--host-pid',"$($padGui.Id)" -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $padEvidence 'gui-real-h264-probe.txt')
    if(!$padProbe.WaitForExit(20000)){Stop-Process -Id $padProbe.Id;throw 'Owned integration probe timed out'}
    $padProbe.WaitForExit();Check ($padProbe.ExitCode -eq 0) "Real WGC/MF probe failed: $($padProbe.ExitCode)"
    HiddenRun (Join-Path $padFixture 'SecondaryScreen.exe') @('--quit') 20000
    Check ($padGui.WaitForExit(5000)) 'Integration GUI failed to exit'
    $padGui.WaitForExit()
    $padLog=Get-Content -LiteralPath (Join-Path $padUser 'logs/secondaryscreen.log') -Raw
    Check ($padLog -match 'Tray state=2') 'Real H264 session did not update UI to connected'
    Copy-Item -LiteralPath (Join-Path $padUser 'logs/secondaryscreen.log') -Destination (Join-Path $padEvidence 'gui-real-h264-host.txt')
    # Only this test created the directory; validate the absolute named target before cleanup.
    $padUserAbsolute=[IO.Path]::GetFullPath($padUser)
    Check ([IO.Path]::GetDirectoryName($padUserAbsolute) -eq [IO.Path]::GetFullPath($env:APPDATA)) 'Unexpected test AppData target'
    Remove-Item -LiteralPath $padUserAbsolute -Recurse -Force
    Write-Output 'PASS simulated-adb readiness + real GUI HostService/WGC/MF/TCP: codec=2, independent slice validation, green UI state; NOT Android or physical USB validation'
} catch {Write-Error $_ -ErrorAction Continue;exit 1} finally {
    if($padGui -and !$padGui.HasExited){Stop-Process -Id $padGui.Id -ErrorAction SilentlyContinue}
}
