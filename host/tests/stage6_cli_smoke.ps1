[CmdletBinding()]
param([string]$BuildDirectory=(Join-Path $PSScriptRoot '../build-stage6'))
$ErrorActionPreference='Stop'
$padBuild=(Resolve-Path -LiteralPath $BuildDirectory).Path
$padExe=Join-Path $padBuild 'Release/secondaryscreen-host.exe'
$padCases=@(
    @{Name='cap';Args=@('--width','320','--height','200','--fps','144','--max-fps','60');Width=320;Height=200;Fps=60},
    @{Name='unlimited';Args=@('--width','320','--height','200','--fps','90','--max-fps','0');Width=320;Height=200;Fps=90},
    @{Name='hello';Args=@('--max-fps','60');Width=320;Height=200;Fps=15},
    @{Name='default';Args=@();Width=320;Height=200;Fps=15}
)
function ReadExact($Stream,[int]$Length){
    $padBytes=New-Object byte[] $Length;$padOffset=0
    while($padOffset -lt $Length){$padCount=$Stream.Read($padBytes,$padOffset,$Length-$padOffset);if(!$padCount){throw 'Unexpected EOF'};$padOffset+=$padCount}
    return ,$padBytes
}
foreach($padCase in $padCases){
    # Primary debug target prevents modifying the user's display even with default auto-resize.
    $padListener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$padListener.Start()
    $padPort=$padListener.LocalEndpoint.Port;$padListener.Stop()
    $padOut=Join-Path $padBuild ("cli-"+$padCase.Name+'.stdout.txt');$padErr=Join-Path $padBuild ("cli-"+$padCase.Name+'.txt')
    $padArgs=@('--capture-primary','--disable-hardware','--port',"$padPort")+$padCase.Args
    if($padCase.Name -ne 'default'){$padArgs+='--no-auto-resize'}
    $padChild=Start-Process -FilePath $padExe -ArgumentList $padArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput $padOut -RedirectStandardError $padErr
    $padClient=$null
    try {
        for($padAttempt=0;$padAttempt -lt 50;$padAttempt++){
            $padClient=[Net.Sockets.TcpClient]::new()
            try{$padClient.Connect('127.0.0.1',$padPort);break}catch{$padClient.Dispose();$padClient=$null;Start-Sleep -Milliseconds 50}
            if($padChild.HasExited){throw "Host exited=$($padChild.ExitCode)"}
        }
        if(!$padClient){throw 'Host did not listen'}
        $padStream=$padClient.GetStream();$padStream.ReadTimeout=5000
        $padBuffer=[IO.MemoryStream]::new();$padWriter=[IO.BinaryWriter]::new($padBuffer)
        $padWriter.Write([uint32]0x31534450);$padWriter.Write([uint16]1);$padWriter.Write([uint16]0);$padWriter.Write([uint32]32)
        $padWriter.Write([uint16]1);$padWriter.Write([uint16]0)
        foreach($padValue in @(320,200,144,320,4096,4096,3)){$padWriter.Write([uint32]$padValue)}
        $padBytes=$padBuffer.ToArray();$padStream.Write($padBytes,0,$padBytes.Length);$padWriter.Dispose()
        $padHeader=ReadExact $padStream 12
        if([BitConverter]::ToUInt32($padHeader,0) -ne 0x31534450 -or [BitConverter]::ToUInt16($padHeader,4) -ne 2 -or [BitConverter]::ToUInt32($padHeader,8) -ne 32){throw 'Invalid ACK header'}
        $padAck=ReadExact $padStream 32
        $padFps=[BitConverter]::ToUInt16($padAck,6);$padWidth=[BitConverter]::ToUInt32($padAck,8);$padHeight=[BitConverter]::ToUInt32($padAck,12)
        if([BitConverter]::ToUInt16($padAck,2) -ne 0 -or $padAck[4] -ne 1 -or $padFps -ne $padCase.Fps -or $padWidth -ne $padCase.Width -or $padHeight -ne $padCase.Height){throw "Unexpected ACK ${padWidth}x${padHeight} fps=$padFps codec=$($padAck[4])"}
        $padFrame=ReadExact $padStream 12
        if([BitConverter]::ToUInt16($padFrame,4) -ne 16){throw 'Expected real VIDEO_FRAME'}
        $padData=ReadExact $padStream ([BitConverter]::ToUInt32($padFrame,8))
        if($padData[28] -ne 255 -or $padData[29] -ne 216 -or $padData[-2] -ne 255 -or $padData[-1] -ne 217){throw 'Expected complete real JPEG'}
        Write-Output "PASS real CLI $($padCase.Name): HELLO 320x200@144 -> ACK ${padWidth}x${padHeight} codec=1 fps=$padFps; full JPEG received; primary debug only"
    } finally {
        if($padClient){$padClient.Dispose()}
        if(!$padChild.HasExited){Stop-Process -Id $padChild.Id;$padChild.WaitForExit()}
    }
}
