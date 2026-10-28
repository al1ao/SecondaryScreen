[CmdletBinding()]
param(
    [ValidateRange(10,7200)][int]$DurationSec = 60,
    [ValidateSet('virtual','primary')][string]$Target = 'virtual',
    [ValidateSet('on','off')][string]$WgcThrottle = 'on',
    [ValidateSet(1,2)][int]$QueueDepth = 2,
    [ValidateRange(1024,65535)][int]$Port = 52140,
    [ValidateRange(1,5)][int]$SyntheticRuns = 3,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build-stage4/Release'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build-stage4/bench'),
    [string]$Name = 'benchmark'
)
$ErrorActionPreference = 'Stop'
$padHost = $null
$padProbe = $null
$padSynthetic = $null
try {
    if ($Name -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Name must be a simple file stem' }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
    if (Test-Path -LiteralPath (Join-Path $outputRoot "$Name-summary.json")) { throw 'Result already exists. Choose another -Name to preserve comparison evidence.' }
    if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) { throw "Port $Port already has a listener. Choose another -Port; existing host will not be stopped." }
    $hostExecutable = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'padscreen-host.exe')).Path
    $probeExecutable = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'padscreen-stage2-probe.exe')).Path
    $syntheticExecutable = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'padscreen-stage2-tests.exe')).Path
    $hostHash = (Get-FileHash -LiteralPath $hostExecutable -Algorithm SHA256).Hash
    $probeHash = (Get-FileHash -LiteralPath $probeExecutable -Algorithm SHA256).Hash
    $syntheticHash = (Get-FileHash -LiteralPath $syntheticExecutable -Algorithm SHA256).Hash
    # Run serially before starting the desktop pipeline to avoid GPU contention.
    $syntheticSamples = @()
    for ($run = 1; $run -le $SyntheticRuns; ++$run) {
        $syntheticLog = Join-Path $outputRoot "$Name-synthetic-$run.log"
        if (Test-Path -LiteralPath $syntheticLog) { throw "Synthetic evidence already exists: $syntheticLog" }
        $padSynthetic = Start-Process -FilePath $syntheticExecutable -WindowStyle Hidden -PassThru -RedirectStandardError $syntheticLog
        if (!$padSynthetic.WaitForExit(30000)) { throw 'Synthetic suite exceeded 30 seconds' }
        $padSynthetic.WaitForExit()
        $syntheticExit = $padSynthetic.ExitCode
        $syntheticText = Get-Content -LiteralPath $syntheticLog -Raw
        $sample = [ordered]@{ Run=$run; Exit=$syntheticExit }
        foreach ($field in @('encode_avg_ms','encode_p95_ms','encode_max_ms')) {
            $metric = [regex]::Match($syntheticText, "\b${field}=([0-9.]+)")
            if (!$metric.Success) { throw "Synthetic metric missing: $field; exit=$syntheticExit; log=$syntheticLog" }
            $sample[$field] = [double]::Parse($metric.Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture)
        }
        $sample['Passed'] = $syntheticExit -eq 0 -and $sample['encode_avg_ms'] -lt 2 -and $sample['encode_p95_ms'] -lt 6
        $syntheticSamples += [pscustomobject]$sample
        Write-Output ("BENCH synthetic " + ($sample | ConvertTo-Json -Compress))
    }
    $startedAt = (Get-Date).ToString('o')
    $hostLog = Join-Path $outputRoot "$Name-host.log"
    $probeLog = Join-Path $outputRoot "$Name-probe.log"
    $arguments = @('--port',"$Port",'--width','1920','--height','1200','--fps','60','--bitrate','10000','--show-stats',
        '--wgc-throttle',$WgcThrottle,'--queue-depth',"$QueueDepth")
    if ($Target -eq 'primary') { $arguments += '--capture-primary' }
    $padHost = Start-Process -FilePath $hostExecutable -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError $hostLog
    $listener = $null
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        $padHost.Refresh()
        if ($padHost.HasExited) { throw "Host exited before listen: $(Get-Content -LiteralPath $hostLog -Raw)" }
        $listener = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
        if ($listener) { break }
        Start-Sleep -Milliseconds 100
    }
    if (!$listener -or $listener.OwningProcess -ne $padHost.Id) { throw 'Listener PID differs from the benchmark host. Refusing to measure a different process.' }
    $padProbe = Start-Process -FilePath $probeExecutable -ArgumentList '--port',"$Port",'--seconds',"$DurationSec",'--host-pid',"$($padHost.Id)" -WindowStyle Hidden -PassThru -RedirectStandardError $probeLog
    Write-Output "BENCH start name=$Name target=$Target throttle=$WgcThrottle queue=$QueueDepth seconds=$DurationSec host_pid=$($padHost.Id) port=$Port"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (!$padProbe.HasExited) {
        if ($watch.Elapsed.TotalSeconds -gt $DurationSec + 30) { throw 'Probe exceeded duration plus timeout allowance' }
        Start-Sleep -Milliseconds 500
        $padProbe.Refresh()
    }
    $padProbe.WaitForExit()
    $probeExit = $padProbe.ExitCode
    $probeText = Get-Content -LiteralPath $probeLog -Raw
    $hostText = Get-Content -LiteralPath $hostLog -Raw
    $resultLine = @(Get-Content -LiteralPath $probeLog | Where-Object { $_ -match 'PROBE RESULT ' }) | Select-Object -Last 1
    if ($probeExit -ne 0 -or !$resultLine) { throw "Probe exit=$probeExit. Inspect $probeLog and $hostLog" }
    $result = [ordered]@{ AcceptanceProfile='stage4-fix-v2'; Name=$Name; Target=$Target; Throttle=$WgcThrottle; QueueDepth=$QueueDepth; DurationSec=$DurationSec; ProbeExit=$probeExit; HostPid=$padHost.Id; Port=$Port; StartedAt=$startedAt; FinishedAt=(Get-Date).ToString('o'); HostSHA256=$hostHash; ProbeSHA256=$probeHash; SyntheticSHA256=$syntheticHash; SyntheticSamples=$syntheticSamples }
    $result['SyntheticPassed'] = @($syntheticSamples | Where-Object { !$_.Passed }).Count -eq 0
    foreach ($match in [regex]::Matches($resultLine, '(\w+)=(-?[0-9]+(?:\.[0-9]+)?)')) {
        $result[$match.Groups[1].Value] = [double]::Parse($match.Groups[2].Value,[Globalization.CultureInfo]::InvariantCulture)
    }
    foreach ($field in @('seconds','frames','fps','handshake_ms','encoded_gaps','missing_frames','private_start','private_end','private_growth','host_cpu_single_core_pct','host_capture_to_receive_avg_ms')) {
        if (!$result.Contains($field)) { throw "Missing probe metric $field" }
    }
    $stats = @(Get-Content -LiteralPath $hostLog | Where-Object { $_ -match '^\[INFO\] fps=' })
    foreach ($field in @('encode','encode_p95_upper','encode_session_avg','encode_session_p95_upper','encode_samples','send','queue','raw_drop','encoded_drop','wgc_drop','wgc_throttled','wgc_received','wgc_copied')) {
        $pattern = if ($field -in @('encode','encode_p95_upper','encode_session_avg','encode_session_p95_upper','send')) { "\b${field}=([0-9.]+)ms" } else { "\b${field}=([0-9.]+)" }
        $values = @($stats | ForEach-Object { if ($_ -match $pattern) { [double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture) } })
        if (!$values.Count) { throw "Missing host metric $field" }
        $result["${field}_last"] = $values[-1]
        if ($field -in @('encode','send','queue')) { $result["${field}_window_mean"] = ($values | Measure-Object -Average).Average }
    }
    $slices = [regex]::Match($probeText, 'PROBE slices P=(\d+) B=(\d+) I=(\d+)')
    if (!$slices.Success) { throw 'No complete H264 slice distribution' }
    $result['P'] = [long]$slices.Groups[1].Value; $result['B'] = [long]$slices.Groups[2].Value; $result['I'] = [long]$slices.Groups[3].Value
    $result['Codec'] = if ($probeText -match 'PROBE ACK codec=2') { 2 } else { 1 }
    $result['CorrectnessPassed'] = $result['Codec'] -eq 2 -and $result['B'] -eq 0 -and $result['missing_frames'] -eq 0 -and $result['encoded_gaps'] -eq 0 -and $result['raw_drop_last'] -eq 0 -and $result['encoded_drop_last'] -eq 0 -and $result['fps'] -ge 59
    $result['PerformancePassed'] = $result['encode_window_mean'] -lt 5 -and $result['encode_session_p95_upper_last'] -lt 6 -and $result['private_growth'] -lt 20MB -and $result['handshake_ms'] -lt 5000
    $result['Exit'] = if ($result['CorrectnessPassed'] -and $result['PerformancePassed'] -and $result['SyntheticPassed']) { 0 } else { 1 }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputRoot "$Name-summary.json") -Encoding UTF8
    [pscustomobject]$result | Export-Csv -LiteralPath (Join-Path $outputRoot "$Name-summary.csv") -NoTypeInformation -Encoding UTF8
    Write-Output ($result | ConvertTo-Json -Depth 5 -Compress)
    if ($result['Exit'] -ne 0) {
        throw 'Benchmark acceptance target failed; retained measured result for review (ProbeExit is reported separately)'
    }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    if ($padSynthetic -and !$padSynthetic.HasExited) { Stop-Process -Id $padSynthetic.Id -ErrorAction SilentlyContinue }
    if ($padProbe -and !$padProbe.HasExited) { Stop-Process -Id $padProbe.Id -ErrorAction SilentlyContinue }
    if ($padHost -and !$padHost.HasExited) { Stop-Process -Id $padHost.Id -ErrorAction SilentlyContinue }
}
exit 0
