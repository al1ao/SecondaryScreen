[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('install','uninstall')][string]$Action)
$ErrorActionPreference='Stop'
try {
    $padScript=Join-Path $PSScriptRoot "$Action-vdd.ps1"
    if(!(Test-Path -LiteralPath $padScript -PathType Leaf)){throw 'Missing packaged VDD helper'}
    $padArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"{0}"' -f $padScript))
    if($Action -eq 'install'){
        $padArguments+=@('-CacheDirectory',('"{0}"' -f (Join-Path $PSScriptRoot '../vdd-cache')))
        if(!(Test-Path -LiteralPath 'C:\VirtualDisplayDriver\vdd_settings.xml')){$padArguments+='-UsePadScreenConfig'}
    }
    $padHelper=Start-Process -FilePath "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $padArguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    exit $padHelper.ExitCode
} catch { Write-Error $_ -ErrorAction Continue;exit 1 }
