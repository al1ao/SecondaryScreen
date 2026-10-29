# Check virtual display supported modes (resolution + refresh rate)
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\check-vdd-modes.ps1
#   powershell -ExecutionPolicy Bypass -File .\check-vdd-modes.ps1 -Device "\\.\DISPLAY28"

[CmdletBinding()]
param(
    [string]$Device = $null
)

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class DisplayModes {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern bool EnumDisplayDevicesW(string dev, uint num, ref DISPLAY_DEVICE dd, uint flags);

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmFormName;
        public short dmLogPixels;
        public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }

    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct DISPLAY_DEVICE {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
    }

    public static List<string> GetModes(string device) {
        var list = new List<string>();
        int i = 0;
        while (true) {
            var dm = new DEVMODE();
            dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            if (!EnumDisplaySettingsW(device, i, ref dm)) break;
            list.Add(dm.dmPelsWidth + "x" + dm.dmPelsHeight + "@" + dm.dmDisplayFrequency);
            i++;
        }
        return list;
    }

    public static List<string> GetDevices() {
        var list = new List<string>();
        for (uint i = 0; ; i++) {
            var dd = new DISPLAY_DEVICE();
            dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            if (!EnumDisplayDevicesW(null, i, ref dd, 0)) break;
            if ((dd.StateFlags & 0x1) != 0) {
                list.Add(dd.DeviceName + "\t" + dd.DeviceString);
            }
        }
        return list;
    }
}
"@

Write-Host "==============================================="
Write-Host "  Virtual Display Mode Check"
Write-Host "==============================================="
Write-Host ""

Write-Host "[Active displays]"
$devices = [DisplayModes]::GetDevices()
foreach ($d in $devices) {
    $parts = $d -split "`t"
    Write-Host ("  {0}  `"{1}`"" -f $parts[0], $parts[1])
}
Write-Host ""

if (-not $Device) {
    $vdd = $devices | Where-Object { $_ -match "Virtual Display|VDD|MttVDD" }
    if ($vdd) {
        $Device = (@($vdd)[0] -split "`t")[0]
        Write-Host "Auto-selected virtual display: $Device"
    } else {
        Write-Host "No virtual display found; querying primary"
        $Device = $null
    }
} else {
    Write-Host "Using device: $Device"
}
Write-Host ""

$modes = [DisplayModes]::GetModes($Device)
Write-Host "[Supported modes] total: $($modes.Count)"
Write-Host ""

if ($modes.Count -eq 0) {
    Write-Host "  ERROR: no modes returned"
    exit 1
}

$grouped = @{}
foreach ($m in $modes) {
    if ($m -match '^(\d+)x(\d+)@(\d+)$') {
        $key = "$($Matches[1])x$($Matches[2])"
        if (-not $grouped.ContainsKey($key)) { $grouped[$key] = @() }
        $grouped[$key] += [int]$Matches[3]
    }
}

Write-Host ("{0,-14} {1,-8} {2}" -f "RESOLUTION", "RATIO", "REFRESH RATES")
Write-Host ("{0,-14} {1,-8} {2}" -f ("-"*14), ("-"*8), ("-"*34))

$sorted = $grouped.Keys | Sort-Object {
    if ($_ -match '^(\d+)x(\d+)$') { [int]$Matches[1] * [int]$Matches[2] } else { 0 }
}

foreach ($key in $sorted) {
    if ($key -match '^(\d+)x(\d+)$') {
        $w = [int]$Matches[1]
        $h = [int]$Matches[2]
        $ratio = [math]::Round($w / $h, 3)
        $hz = ($grouped[$key] | Sort-Object) -join ", "
        $note = ""
        if ([math]::Abs($ratio - 1.6) -lt 0.01) { $note = "  <-- 16:10 matches tablet" }
        elseif ([math]::Abs($ratio - 1.7778) -lt 0.01) { $note = "  <-- 16:9" }
        Write-Host ("{0,-14} {1,-8} {2}{3}" -f $key, $ratio, $hz, $note)
    }
}

Write-Host ""
Write-Host "==============================================="
Write-Host "  Key checks"
Write-Host "==============================================="
Write-Host ""

if ($grouped.ContainsKey("2944x1840")) {
    Write-Host "  [OK] 2944x1840 available (tablet native)"
    Write-Host "       refresh: $(($grouped['2944x1840'] | Sort-Object) -join ', ') Hz"
} else {
    Write-Host "  [NO] 2944x1840 NOT available (tablet native missing)"
}

if ($grouped.ContainsKey("2560x1600")) { Write-Host "  [OK] 2560x1600 available (16:10 alt)" } else { Write-Host "  [--] 2560x1600 missing" }
if ($grouped.ContainsKey("1920x1200")) { Write-Host "  [OK] 1920x1200 available (16:10 current default)" } else { Write-Host "  [--] 1920x1200 missing" }

Write-Host ""
Write-Host "[All 16:10 modes]"
$wide = @()
foreach ($k in $sorted) {
    if ($k -match '^(\d+)x(\d+)$') {
        $r = [int]$Matches[1] / [int]$Matches[2]
        if ([math]::Abs($r - 1.6) -lt 0.01) { $wide += $k }
    }
}
if ($wide.Count -gt 0) {
    foreach ($w in $wide) {
        Write-Host ("  {0} @ {1} Hz" -f $w, (($grouped[$w] | Sort-Object) -join ', '))
    }
} else {
    Write-Host "  NONE"
}

Write-Host ""
Write-Host "[Highest refresh rate]"
$maxHz = 0
$maxMode = ""
foreach ($key in $grouped.Keys) {
    foreach ($hz in $grouped[$key]) {
        if ($hz -gt $maxHz) { $maxHz = $hz; $maxMode = "$key@$hz" }
    }
}
Write-Host "  $maxMode"
Write-Host ""
