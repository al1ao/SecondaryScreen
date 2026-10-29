[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstallDirectory)
$ErrorActionPreference='Stop'
try {
    $padRoot=(Resolve-Path -LiteralPath $InstallDirectory).Path
    $padMarker=Join-Path $padRoot 'install-state.ini'
    if(!(Test-Path -LiteralPath $padMarker)){throw 'Missing install ownership marker'}
    $padRecorded=@(Get-Content -LiteralPath $padMarker | Where-Object {$_ -match '^Root='})
    if($padRecorded.Count -ne 1 -or [IO.Path]::GetFullPath($padRecorded[0].Substring(5)) -ne [IO.Path]::GetFullPath($padRoot)){throw 'Install ownership marker differs'}
    $padExecutable=[IO.Path]::GetFullPath((Join-Path $padRoot 'platform-tools/adb.exe'))
    $padOwned=@(Get-CimInstance Win32_Process -Filter "Name='adb.exe'" | Where-Object {$_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $padExecutable})
    foreach($padProcess in $padOwned){
        $padCurrent=Get-CimInstance Win32_Process -Filter "ProcessId=$($padProcess.ProcessId)" -ErrorAction SilentlyContinue
        if($padCurrent -and $padCurrent.ExecutablePath -and [IO.Path]::GetFullPath($padCurrent.ExecutablePath) -eq $padExecutable -and $padCurrent.CreationDate -eq $padProcess.CreationDate){
            Stop-Process -Id $padProcess.ProcessId -ErrorAction Stop
            Wait-Process -Id $padProcess.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
            Write-Output "Stopped adb owned by this install: PID=$($padProcess.ProcessId)"
        }
    }
    # Never run global `adb kill-server`; an Android Studio/PATH server is unrelated.
    exit 0
}catch{Write-Error $_ -ErrorAction Continue;exit 1}
