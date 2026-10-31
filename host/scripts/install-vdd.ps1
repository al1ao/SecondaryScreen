[CmdletBinding()]
param(
    [switch]$DownloadOnly,
    [switch]$UseSecondaryScreenConfig,
    [string]$CacheDirectory
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty inside param() defaults.
if (!$CacheDirectory) { $CacheDirectory = Join-Path $PSScriptRoot '../.tools/vdd-25.7.23' }
try {
    $administrator = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (!$DownloadOnly -and !$administrator) { throw 'Administrator privileges required. Open PowerShell as administrator and run host\scripts\install-vdd.ps1 again. Use -DownloadOnly to verify the package without installing.' }
    if (![Environment]::Is64BitOperatingSystem -or ![Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
        throw 'This pinned driver package requires Windows x64 and 64-bit PowerShell. ARM64 installation is not implemented.'
    }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $version = '25.7.23'
    $url = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip'
    $expectedHash = 'E24210692B442B39AF763536330CE78B423F19342B7A7792C26DE3944E418B3A'
    New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
    $archive = Join-Path $CacheDirectory 'driver.zip'
    if (!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expectedHash) {
        throw "Driver archive SHA256 mismatch. Remove only '$archive', then retry the official download. Nothing has been installed."
    }
    $package = Join-Path $CacheDirectory 'package'
    Expand-Archive -LiteralPath $archive -DestinationPath $package -Force
    $source = Join-Path $package 'VirtualDisplayDriver'
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $source 'mttvdd.cat')
    if ($signature.Status -ne 'Valid') { throw "Driver catalog signature is not valid: $($signature.Status). Do not enable test signing or disable signature enforcement." }
    foreach ($name in @('MttVDD.inf', 'MttVDD.dll', 'mttvdd.cat', 'vdd_settings.xml')) {
        if (!(Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) { throw "Official package is missing $name" }
    }
    [xml]$settings = Get-Content -LiteralPath (Join-Path $source 'vdd_settings.xml') -Raw
    if ($settings.DocumentElement.Name -ne 'vdd_settings') { throw 'Unexpected upstream configuration schema; do not install with an invented XML format.' }
    Write-Output "Verified official VDD $version archive and signed catalog. Package: $source"
    if ($DownloadOnly) { Write-Output 'DownloadOnly: no driver, system configuration, certificate or display topology changed.'; exit 0 }

    # Root-enumerated VDD needs a device instance; pnputil /add-driver alone only
    # stages an INF and cannot reliably create that instance on a clean machine.
    $existing = @(Get-PnpDevice -Class Display | Where-Object {
        $ids = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
        @($ids) -contains 'Root\MttVDD'
    })
    if ($existing.Count) { throw 'MttVDD is already installed. No second instance was created. Inspect Device Manager and enable/extend the existing virtual display.' }
    $destination = 'C:\VirtualDisplayDriver'
    $manifest = Join-Path $destination 'secondaryscreen-install.json'
    foreach ($name in @('MttVDD.inf', 'MttVDD.dll', 'mttvdd.cat', 'secondaryscreen-install.json')) {
        if (Test-Path -LiteralPath (Join-Path $destination $name)) { throw "Existing file '$destination\$name'. Refusing to overwrite an installation. Inspect or uninstall it first." }
    }
    $config = Join-Path $destination 'vdd_settings.xml'
    if ($UseSecondaryScreenConfig -and (Test-Path -LiteralPath $config)) { throw 'A VDD settings file already exists. Refusing to overwrite it; back it up and merge desired modes manually.' }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    $owned = @()
    foreach ($name in @('MttVDD.inf', 'MttVDD.dll', 'mttvdd.cat')) {
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $destination $name)
        $owned += @{ Name = $name; Hash = (Get-FileHash -LiteralPath (Join-Path $destination $name)).Hash }
    }
    if (!(Test-Path -LiteralPath $config)) {
        # TODO(design): keep the verified release's default XML and EDID. Custom
        # 1920x1200 mode configuration is optional and documented separately.
        $configSource = Join-Path $source 'vdd_settings.xml'
        if ($UseSecondaryScreenConfig) { $configSource = Join-Path $PSScriptRoot '../assets/vdd_settings.xml' }
        Copy-Item -LiteralPath $configSource -Destination $config
        $owned += @{ Name = 'vdd_settings.xml'; Hash = (Get-FileHash -LiteralPath $config).Hash }
    }
    if (!('SecondaryScreen.VddDeviceInstaller' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'VddDeviceInstaller.cs') }
    $installed = [SecondaryScreen.VddDeviceInstaller]::Install((Join-Path $destination 'MttVDD.inf'))
    $driver = $null
    for ($attempt = 0; $attempt -lt 10; ++$attempt) {
        $driver = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -eq $installed.InstanceId } | Select-Object -First 1
        if ($driver -and $driver.InfName) { break }
        Start-Sleep -Milliseconds 500
    }
    @{ Version = $version; InstanceId = $installed.InstanceId; InfName = $driver.InfName; Files = $owned } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
    $device = Get-PnpDevice -Class Display -InstanceId $installed.InstanceId
    $device | Format-Table FriendlyName,Status,InstanceId
    if ($device.Status -ne 'OK') { throw 'VDD device exists but is not OK. Keep the install manifest, inspect Device Manager error code and reboot if requested; use uninstall-vdd.ps1 to remove this installation.' }
    Write-Output "Installed signed VDD. Reboot required: $($installed.RebootRequired)."
    Write-Output 'Open Settings > System > Display, enable the new display and choose Extend these displays. This script does not set desktop topology or position.'
} catch {
    Write-Error "VDD installation failed: $($_.Exception.Message). Check administrator privileges, network access, driver signature and Device Manager. No automatic reboot or topology change was requested." -ErrorAction Continue
    exit 1
}
