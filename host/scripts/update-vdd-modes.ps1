[CmdletBinding()]
param(
    [string]$SettingsPath = 'C:\VirtualDisplayDriver\vdd_settings.xml',
    [string]$SourceTemplate,
    [switch]$NoRestart
)
# Adds the modes from assets/vdd_settings.xml (16:10 tablet sizes included) to
# an existing VDD configuration without removing or overwriting user modes.
# The driver only reads vdd_settings.xml while the device starts, so the device
# is restarted afterwards; the running app reconnects and renegotiates by itself.
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty inside param() defaults.
if (!$SourceTemplate) { $SourceTemplate = Join-Path $PSScriptRoot '../assets/vdd_settings.xml' }

function Get-VddDevice {
    foreach ($device in @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue)) {
        $ids = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
        if (@($ids) -contains 'Root\MttVDD') { return $device }
    }
    return $null
}

function Get-TemplateModes([string]$path) {
    $modes = New-Object 'Collections.Generic.List[object]'
    if (!(Test-Path -LiteralPath $path)) { return $modes }
    $template = New-Object System.Xml.XmlDocument
    $template.Load($path)
    foreach ($resolution in $template.SelectNodes('/vdd_settings/resolutions/resolution')) {
        try {
            $width = [int]$resolution.SelectSingleNode('width').InnerText
            $height = [int]$resolution.SelectSingleNode('height').InnerText
            $refresh = [int]$resolution.SelectSingleNode('refresh_rate').InnerText
            if ($width -lt 2 -or $width -gt 16384 -or $height -lt 2 -or $height -gt 16384 -or $refresh -lt 2 -or $refresh -gt 1000) {
                throw 'out of range'
            }
            $modes.Add([pscustomobject]@{ Width = $width; Height = $height; Refresh = $refresh })
        } catch { Write-Warning '模板中存在无法解析的 resolution 条目，已跳过。' }
    }
    return $modes
}

function Get-BuiltinModes {
    return @(
        [pscustomobject]@{ Width = 2944; Height = 1840; Refresh = 60 },
        [pscustomobject]@{ Width = 2560; Height = 1600; Refresh = 60 },
        [pscustomobject]@{ Width = 1920; Height = 1200; Refresh = 60 },
        [pscustomobject]@{ Width = 1600; Height = 1000; Refresh = 60 },
        [pscustomobject]@{ Width = 1920; Height = 1080; Refresh = 60 },
        [pscustomobject]@{ Width = 1280; Height = 800; Refresh = 60 },
        [pscustomobject]@{ Width = 2560; Height = 1440; Refresh = 60 },
        [pscustomobject]@{ Width = 3840; Height = 2160; Refresh = 60 }
    )
}

try {
    $administrator = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (!$administrator) { throw '需要管理员权限。请以管理员身份打开 PowerShell，再运行 scripts\update-vdd-modes.ps1。' }
    if (!(Test-Path -LiteralPath $SettingsPath)) { throw "找不到 VDD 配置：$SettingsPath。请先安装 VDD 组件。" }
    $device = Get-VddDevice
    if (!$device) { throw '找不到 Root\MttVDD 虚拟显示设备。请先通过安装包或 scripts/install-vdd.ps1 安装 VDD。' }

    $wanted = @(Get-TemplateModes $SourceTemplate)
    if ($wanted.Count -eq 0) {
        Write-Output "未找到模式模板 $SourceTemplate，使用内置模式列表。"
        $wanted = @(Get-BuiltinModes)
    }

    $document = New-Object System.Xml.XmlDocument
    $document.Load($SettingsPath)
    if ($null -eq $document.DocumentElement -or $document.DocumentElement.Name -ne 'vdd_settings') {
        throw 'vdd_settings.xml 结构不符（根元素不是 vdd_settings），拒绝修改。'
    }
    $resolutions = $document.DocumentElement.SelectSingleNode('resolutions')
    if ($null -eq $resolutions) {
        $resolutions = $document.CreateElement('resolutions')
        [void]$document.DocumentElement.AppendChild($resolutions)
    }
    $existing = @{}
    foreach ($resolution in $resolutions.SelectNodes('resolution')) {
        try {
            $width = [int]$resolution.SelectSingleNode('width').InnerText
            $height = [int]$resolution.SelectSingleNode('height').InnerText
            if ($width -ge 2 -and $height -ge 2) { $existing[('{0}x{1}' -f $width, $height)] = $true }
        } catch { Write-Warning '忽略无法解析的 resolution 条目。' }
    }

    $added = New-Object 'Collections.Generic.List[string]'
    foreach ($mode in $wanted) {
        $key = '{0}x{1}' -f $mode.Width, $mode.Height
        if ($existing.ContainsKey($key)) { continue }
        $entry = $document.CreateElement('resolution')
        foreach ($name in @('width', 'height', 'refresh_rate')) {
            $child = $document.CreateElement($name)
            if ($name -eq 'width') { $child.InnerText = [string]$mode.Width }
            elseif ($name -eq 'height') { $child.InnerText = [string]$mode.Height }
            else { $child.InnerText = [string]$mode.Refresh }
            [void]$entry.AppendChild($child)
        }
        [void]$resolutions.AppendChild($entry)
        $existing[$key] = $true
        $added.Add($key)
    }

    if ($added.Count -eq 0) {
        Write-Output '所有模板模式均已存在，无需修改配置；仍会重启设备以加载当前配置（-NoRestart 可跳过）。'
    } else {
        $backup = Join-Path ([IO.Path]::GetDirectoryName($SettingsPath)) ('vdd_settings.backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.xml')
        Copy-Item -LiteralPath $SettingsPath -Destination $backup
        $document.Save($SettingsPath)
        Write-Output ('已添加分辨率：' + ($added -join ', '))
        Write-Output "原配置备份：$backup"
    }

    if ($NoRestart) {
        Write-Output '已跳过设备重启（-NoRestart）。新分辨率会在设备下次启动或重启 Windows 后生效。'
        exit 0
    }

    Write-Output "正在重启虚拟显示设备 $($device.InstanceId)…"
    $pnputil = Join-Path $env:windir 'System32\pnputil.exe'
    & $pnputil /restart-device "$($device.InstanceId)" | Out-Host
    if ($LASTEXITCODE -notin @(0, 3010)) {
        Write-Warning "pnputil 重启失败（退出码 $LASTEXITCODE），改用 Disable/Enable-PnpDevice。"
        try {
            Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false
            Start-Sleep -Milliseconds 800
        } finally {
            try { Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop }
            catch { Write-Warning "重新启用虚拟显示设备失败，请在设备管理器中手动启用：$($_.Exception.Message)" }
        }
    }
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        Start-Sleep -Milliseconds 500
        $current = Get-PnpDevice -InstanceId $device.InstanceId -ErrorAction SilentlyContinue
        if ($current -and $current.Status -eq 'OK') { break }
    }

    if (!('SecondaryScreenModes' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class SecondaryScreenModes {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsW(string device, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplayDevicesW(string device, uint num, ref DISPLAY_DEVICE dd, uint flags);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DISPLAY_DEVICE {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }
    public static string FindVirtualDisplay(string instanceId) {
        string fallback = null;
        for (uint i = 0; ; ++i) {
            var dd = new DISPLAY_DEVICE(); dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            if (!EnumDisplayDevicesW(null, i, ref dd, 0)) break;
            if ((dd.StateFlags & 0x1) == 0) continue;
            if (!String.IsNullOrEmpty(instanceId) && String.Equals(dd.DeviceID, instanceId, StringComparison.OrdinalIgnoreCase))
                return dd.DeviceName;
            string name = dd.DeviceString ?? "";
            bool named = name.IndexOf("Virtual", StringComparison.OrdinalIgnoreCase) >= 0 ||
                         name.IndexOf("VDD", StringComparison.OrdinalIgnoreCase) >= 0 ||
                         name.IndexOf("Indirect", StringComparison.OrdinalIgnoreCase) >= 0;
            if (named && fallback == null) fallback = dd.DeviceName;
        }
        return fallback;
    }
    public static List<string> Modes(string device) {
        var list = new List<string>();
        for (int i = 0; ; ++i) {
            var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            if (!EnumDisplaySettingsW(device, i, ref dm)) break;
            list.Add(dm.dmPelsWidth + "x" + dm.dmPelsHeight + "@" + dm.dmDisplayFrequency);
        }
        return list;
    }
}
'@
    }

    Start-Sleep -Seconds 2
    $virtual = [SecondaryScreenModes]::FindVirtualDisplay("$($device.InstanceId)")
    if (!$virtual) {
        Write-Warning '未找到活动的虚拟显示器。请确认 Windows 显示设置中已启用它并选择“扩展这些显示器”。'
        exit 0
    }
    $available = @([SecondaryScreenModes]::Modes($virtual))
    $missing = New-Object 'Collections.Generic.List[string]'
    foreach ($mode in $wanted) {
        $name = '{0}x{1}' -f $mode.Width, $mode.Height
        if (!($available | Where-Object { $_ -match ('^' + [regex]::Escape($name) + '@') })) { $missing.Add($name) }
    }
    if ($missing.Count -eq 0) {
        Write-Output '验证通过：模板中的分辨率均已出现在虚拟屏模式列表中。'
    } else {
        Write-Warning ('以下分辨率尚未出现：' + ($missing -join ', ') + '。请重启 Windows 后再次运行本脚本验证。')
    }
    Write-Output "虚拟显示设备：$virtual"
    $wide = @($available | Where-Object { $_ -match '^(2944x1840|2560x1600|1920x1200|1600x1000|1280x800)@' })
    Write-Output ('16:10 模式：' + ($wide -join ', '))
    exit 0
} catch {
    Write-Error "更新 VDD 模式失败：$($_.Exception.Message)" -ErrorAction Continue
    exit 1
}
