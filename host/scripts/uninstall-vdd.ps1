[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
try {
    $administrator = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (!$administrator) { throw 'Administrator privileges required. Open PowerShell as administrator and run host\scripts\uninstall-vdd.ps1 again.' }
    $destination = 'C:\VirtualDisplayDriver'
    $manifest = Join-Path $destination 'padscreen-install.json'
    if (!(Test-Path -LiteralPath $manifest)) { throw 'No PadScreen VDD install manifest found. Use the original installer or Device Manager; no unrelated display driver will be removed.' }
    $state = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $device = Get-PnpDevice -InstanceId $state.InstanceId -ErrorAction SilentlyContinue
    $inf = $state.InfName
    if ($device) {
        $ids = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds').Data
        if (@($ids) -notcontains 'Root\MttVDD') { throw 'Recorded instance no longer has the VDD hardware ID. Refusing removal.' }
        $driver = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -eq $device.InstanceId } | Select-Object -First 1
        if ($driver -and $driver.InfName) { $inf = $driver.InfName }
        if (!('PadScreen.VddDeviceInstaller' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'VddDeviceInstaller.cs') }
        [PadScreen.VddDeviceInstaller]::Remove($device.InstanceId)
    }
    if ($inf -notmatch '^oem\d+\.inf$') { throw 'Cannot identify the exact published VDD INF. Keep the manifest and inspect pnputil /enum-drivers for MttVDD.inf. No broad driver removal was attempted.' }
    $publishedInf = Join-Path $env:windir "INF\$inf"
    if (Test-Path -LiteralPath $publishedInf) {
        $content = Get-Content -LiteralPath $publishedInf -Raw
        if ($content -notmatch 'Root\\MttVDD' -or $content -notmatch 'MikeTheTech') { throw 'Published INF identity differs from the expected MttVDD package. Refusing deletion.' }
        & "$env:windir\System32\pnputil.exe" /delete-driver $inf /uninstall
        if ($LASTEXITCODE -notin @(0,3010)) { throw "pnputil failed with exit $LASTEXITCODE; keep manifest and inspect Device Manager." }
    }
    $remaining = Get-PnpDevice -InstanceId $state.InstanceId -ErrorAction SilentlyContinue
    if ($remaining -and $remaining.Status -eq 'OK') { throw 'VDD is still active; keep manifest and retry after reboot.' }
    # Remove only unchanged files created by this installer. Never recursively
    # delete the VDD directory or overwrite/remove user-owned settings.
    foreach ($file in $state.Files) {
        if ($file.Name -notin @('MttVDD.inf','MttVDD.dll','mttvdd.cat','vdd_settings.xml')) { throw 'Unexpected file name in install manifest.' }
        $path = Join-Path $destination $file.Name
        if (Test-Path -LiteralPath $path) {
            if ((Get-FileHash -LiteralPath $path).Hash -eq $file.Hash) { Remove-Item -LiteralPath $path }
            else { Write-Warning "Preserving modified user file: $path" }
        }
    }
    Remove-Item -LiteralPath $manifest
    Write-Output 'Recorded VDD device and driver package removed. Modified/pre-existing configuration files are preserved; restart Windows if requested by pnputil.'
    Get-PnpDevice -Class Display | Format-Table FriendlyName,Status,InstanceId
} catch {
    Write-Error "VDD uninstallation failed: $($_.Exception.Message)" -ErrorAction Continue
    exit 1
}
