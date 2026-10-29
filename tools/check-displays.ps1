# Full display diagnostics: all adapters (attached or not), monitors, modes, and VDD config
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\check-displays.ps1
#   powershell -ExecutionPolicy Bypass -File .\check-displays.ps1 -Device "\\.\DISPLAY28"

[CmdletBinding()]
param(
    [string]$Device = $null
)

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class DispDiag {
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

    // Returns "deviceName\tdeviceString\tstateFlags"
    public static List<string> GetAdapters() {
        var list = new List<string>();
        for (uint i = 0; ; i++) {
            var dd = new DISPLAY_DEVICE();
            dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            if (!EnumDisplayDevicesW(null, i, ref dd, 0)) break;
            list.Add(dd.DeviceName + "\t" + dd.DeviceString + "\t" + dd.StateFlags);
        }
        return list;
    }

    // Returns "deviceName\tdeviceString\tstateFlags" for monitors under an adapter
    public static List<string> GetMonitors(string adapter) {
        var list = new List<string>();
        for (uint i = 0; ; i++) {
            var dd = new DISPLAY_DEVICE();
            dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            if (!EnumDisplayDevicesW(adapter, i, ref dd, 0)) break;
            list.Add(dd.DeviceName + "\t" + dd.DeviceString + "\t" + dd.StateFlags);
        }
        return list;
    }

    public static string DecodeFlags(int flags) {
        var sb = new StringBuilder();
        if ((flags & 0x1) != 0) sb.Append("ATTACHED ");
        if ((flags & 0x4) != 0) sb.Append("PRIMARY ");
        if ((flags & 0x8) != 0) sb.Append("MIRRORING ");
        if ((flags & 0x20) != 0) sb.Append("REMOVABLE ");
        if (sb.Length == 0) sb.Append("(not attached)");
        return sb.ToString().Trim();
    }
}
"@

$TAB = [char]9

Write-Host "==================================================="
Write-Host "  Full Display Diagnostics"
Write-Host "==================================================="
Write-Host ""

# ---------- 1. All adapters ----------
Write-Host "[1] ALL DISPLAY ADAPTERS (including not attached)"
Write-Host ""

$adapters = [DispDiag]::GetAdapters()
$attachedList = @()
$allList = @()

foreach ($a in $adapters) {
    $p = $a -split "`t"
    $name = $p[0]
    $desc = $p[1]
    $flags = [int]$p[2]
    $flagText = [DispDiag]::DecodeFlags($flags)
    $isAttached = ($flags -band 0x1) -ne 0

    $mark = if ($isAttached) { "[ON ]" } else { "[OFF]" }
    Write-Host ("  {0} {1}" -f $mark, $name)
    Write-Host ("        desc : {0}" -f $desc)
    Write-Host ("        flags: 0x{0:X}  {1}" -f $flags, $flagText)

    $allList += [PSCustomObject]@{ Name=$name; Desc=$desc; Flags=$flags; Attached=$isAttached }

    # monitors under this adapter
    $mons = [DispDiag]::GetMonitors($name)
    foreach ($m in $mons) {
        $mp = $m -split "`t"
        if ($mp[1]) {
            Write-Host ("        monitor: {0}  `"{1}`"" -f $mp[0], $mp[1])
        }
    }
    Write-Host ""
}

# ---------- 2. Modes per attached adapter ----------
Write-Host "==================================================="
Write-Host "[2] MODES PER ATTACHED ADAPTER"
Write-Host "==================================================="
Write-Host ""

$summary = @()

foreach ($ad in $allList) {
    if (-not $ad.Attached) { continue }

    Write-Host ("--- {0}  `"{1}`" ---" -f $ad.Name, $ad.Desc)
    $modes = [DispDiag]::GetModes($ad.Name)

    if ($modes.Count -eq 0) {
        Write-Host "  (no modes returned)"
        Write-Host ""
        continue
    }

    $grouped = @{}
    foreach ($m in $modes) {
        if ($m -match '^(\d+)x(\d+)@(\d+)$') {
            $key = "$($Matches[1])x$($Matches[2])"
            if (-not $grouped.ContainsKey($key)) { $grouped[$key] = @() }
            $grouped[$key] += [int]$Matches[3]
        }
    }

    $sorted = $grouped.Keys | Sort-Object {
        if ($_ -match '^(\d+)x(\d+)$') { [int]$Matches[1] * [int]$Matches[2] } else { 0 }
    }

    Write-Host ("  {0,-14} {1,-8} {2}" -f "RESOLUTION", "RATIO", "REFRESH")
    Write-Host ("  {0,-14} {1,-8} {2}" -f ("-"*14), ("-"*8), ("-"*28))

    foreach ($key in $sorted) {
        if ($key -match '^(\d+)x(\d+)$') {
            $w = [int]$Matches[1]
            $h = [int]$Matches[2]
            $ratio = [math]::Round($w / $h, 3)
            $hz = ($grouped[$key] | Sort-Object) -join ", "
            $note = ""
            if ([math]::Abs($ratio - 1.6) -lt 0.01) { $note = "  <-- 16:10" }
            elseif ([math]::Abs($ratio - 1.7778) -lt 0.01) { $note = "  <-- 16:9" }
            Write-Host ("  {0,-14} {1,-8} {2}{3}" -f $key, $ratio, $hz, $note)
        }
    }

    $summary += [PSCustomObject]@{
        Device = $ad.Name
        Desc = $ad.Desc
        Modes = $sorted
        Grouped = $grouped
    }
    Write-Host ""
}

# ---------- 3. VDD config ----------
Write-Host "==================================================="
Write-Host "[3] VDD CONFIG FILE"
Write-Host "==================================================="
Write-Host ""

$cfgPaths = @(
    "C:\VirtualDisplayDriver\vdd_settings.xml",
    "$env:ProgramData\VirtualDisplayDriver\vdd_settings.xml"
)

$found = $false
foreach ($cp in $cfgPaths) {
    if (Test-Path $cp) {
        Write-Host "  Found: $cp"
        Write-Host ""
        $content = Get-Content $cp -Raw -ErrorAction SilentlyContinue
        $res = [regex]::Matches($content, '<width>(\d+)</width>\s*<height>(\d+)</height>\s*<refresh_rate>(\d+)</refresh_rate>')
        Write-Host "  Configured resolutions:"
        foreach ($r in $res) {
            Write-Host ("    {0}x{1} @ {2}Hz" -f $r.Groups[1].Value, $r.Groups[2].Value, $r.Groups[3].Value)
        }
        $glob = [regex]::Matches($content, '<g_refresh_rate>(\d+)</g_refresh_rate>')
        if ($glob.Count -gt 0) {
            $hzList = @()
            foreach ($g in $glob) { $hzList += $g.Groups[1].Value }
            Write-Host ""
            Write-Host ("  Global refresh rates: {0}" -f ($hzList -join ", "))
        }
        $found = $true
        break
    }
}

if (-not $found) {
    Write-Host "  VDD config not found (may not be installed)"
}
Write-Host ""

# ---------- 4. Summary ----------
Write-Host "==================================================="
Write-Host "[4] SUMMARY"
Write-Host "==================================================="
Write-Host ""

$vddAttached = $allList | Where-Object { $_.Desc -match "Virtual Display Driver|MttVDD|VDD" -and $_.Attached }
$vddAny = $allList | Where-Object { $_.Desc -match "Virtual Display Driver|MttVDD|VDD" }

Write-Host "  Total adapters      : $($allList.Count)"
Write-Host "  Attached to desktop : $(($allList | Where-Object { $_.Attached }).Count)"
Write-Host ""

if ($vddAny) {
    Write-Host "  VDD adapters found:"
    foreach ($v in $vddAny) {
        $st = if ($v.Attached) { "ATTACHED" } else { "NOT ATTACHED" }
        Write-Host ("    {0}  {1}  [{2}]" -f $v.Name, $v.Desc, $st)
    }
} else {
    Write-Host "  No VDD adapter found in display adapter list"
}
Write-Host ""

if ($summary.Count -gt 0) {
    Write-Host "  16:10 modes per attached adapter:"
    foreach ($s in $summary) {
        $wide = @()
        foreach ($k in $s.Modes) {
            if ($k -match '^(\d+)x(\d+)$') {
                $rr = [int]$Matches[1] / [int]$Matches[2]
                if ([math]::Abs($rr - 1.6) -lt 0.01) { $wide += $k }
            }
        }
        if ($wide.Count -gt 0) {
            Write-Host ("    {0} `"{1}`":" -f $s.Device, $s.Desc)
            foreach ($w in $wide) {
                Write-Host ("      {0} @ {1} Hz" -f $w, (($s.Grouped[$w] | Sort-Object) -join ", "))
            }
        }
    }
} else {
    Write-Host "  No attached adapter with modes"
}
Write-Host ""
Write-Host "Done."
