# All files, ADB replies and socket operations are owned fixtures. No device,
# live forward or actual TCP socket is touched.
param([Parameter(Mandatory=$true)][string]$ManagerPath)
$ErrorActionPreference='Stop'
$ManagerPath=[IO.Path]::GetFullPath($ManagerPath)
function Assert([bool]$Condition,[string]$Text) { if (-not $Condition) { throw $Text } }
function Rejects([scriptblock]$Action,[string]$Pattern='.') {
    $failure=$null; try { & $Action } catch { $failure=$_ }
    Assert ($null -ne $failure) 'Expected manager failure.'
    Assert ($failure.Exception.Message -match $Pattern) "Unexpected failure: $failure"
}
$work=Join-Path ([IO.Path]::GetTempPath()) ('opencode/vd-mixedvr-manager-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$savedLocal=$env:LOCALAPPDATA
$savedFake=$env:VDHS_FAKE_ADB_ROOT
$env:LOCALAPPDATA=Join-Path $work 'profile'
$env:VDHS_FAKE_ADB_ROOT=$work
$adb=Join-Path $work 'adb.ps1'
$utf8=[Text.UTF8Encoding]::new($false)
$fixture=@'
[IO.File]::AppendAllText((Join-Path $env:VDHS_FAKE_ADB_ROOT 'adb.log'),(($args -join ' ')+"`n"))
$global:LASTEXITCODE=0
if ($args[0] -eq 'devices') { 'List of devices attached'; 'FIXTURE-HEADSET device'; return }
if ($args[2] -eq 'get-state') {
    if (Test-Path (Join-Path $env:VDHS_FAKE_ADB_ROOT 'offline')) { $global:LASTEXITCODE=1; return }
    'device'; return
}
if ($args[2] -eq 'exec-out') { 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'; return }
if ($args[2] -eq 'forward') {
    if ($args[3] -eq '--list') {
        $path=Join-Path $env:VDHS_FAKE_ADB_ROOT 'forward.txt'
        if (Test-Path $path) { [IO.File]::ReadAllText($path) }
        return
    }
    if (Test-Path (Join-Path $env:VDHS_FAKE_ADB_ROOT 'collision')) { $global:LASTEXITCODE=1 }
    return
}
if ($args[2] -eq 'shell') { return }
$global:LASTEXITCODE=1
'@
[IO.File]::WriteAllText($adb,$fixture,[Text.Encoding]::ASCII)
$fakeNetwork=@{Connects=0;Writes=0;Disposed=0;Reply="VDR1 1 0 1 unobserved ok`n";Position=0;Ready=$true;Request='';Shutdown=$false;Release='eof';Events=[Collections.Generic.List[string]]::new()}
$fakeWait=New-Object psobject
$fakeWait | Add-Member ScriptMethod Wait { param($Timeout) return $fakeNetwork.Ready }
$fakeStream=New-Object psobject -Property @{ReadTimeout=0;WriteTimeout=0}
$fakeStream | Add-Member ScriptMethod Write { param($Bytes,$Offset,$Count) $fakeNetwork.Writes++; $fakeNetwork.Request=[Text.Encoding]::ASCII.GetString($Bytes,$Offset,$Count) }
$fakeStream | Add-Member ScriptMethod ReadByte {
    if ($fakeNetwork.Position -ge $fakeNetwork.Reply.Length) {
        if ($fakeNetwork.Shutdown) {
            Assert ((Phase) -match ' restore$') 'Auto enabled before helper EOF.'
            $fakeNetwork.Events.Add('release-read')
            if ($fakeNetwork.Release -eq 'timeout') { throw 'Fixture release read timed out.' }
            if ($fakeNetwork.Release -eq 'data') { return 88 }
        }
        return -1
    }
    return [int][char]$fakeNetwork.Reply[$fakeNetwork.Position++]
}
$fakeSocket=New-Object psobject
$fakeSocket | Add-Member ScriptMethod Shutdown {
    param($How)
    Assert ($How -eq [Net.Sockets.SocketShutdown]::Send) 'Expected send-only shutdown.'
    Assert ($fakeNetwork.Position -eq $fakeNetwork.Reply.Length) 'Shutdown before complete status reply.'
    $fakeNetwork.Events.Add('shutdown-send'); $fakeNetwork.Shutdown=$true
    if ($fakeNetwork.Release -eq 'shutdown-error') { throw 'Fixture shutdown failed.' }
}
$fakeClient=New-Object psobject -Property @{Client=$fakeSocket}
$fakeClient | Add-Member ScriptMethod ConnectAsync { param($Address,$Port) $fakeNetwork.Connects++; $fakeNetwork.Position=0; $fakeNetwork.Shutdown=$false; $fakeNetwork.Events.Clear(); return $fakeWait }
$fakeClient | Add-Member ScriptMethod GetStream { return $fakeStream }
$fakeClient | Add-Member ScriptMethod Dispose {
    Assert ((Phase) -match ' restore$') 'Auto enabled before disposing status client.'
    $fakeNetwork.Events.Add('dispose'); $fakeNetwork.Disposed++
}
function New-Object { param([string]$TypeName) if ($TypeName -eq 'Net.Sockets.TcpClient') { return $fakeClient }; Microsoft.PowerShell.Utility\New-Object -TypeName $TypeName }
function Get-Process { param([string]$Name,[string]$ErrorAction) }
function Phase { return (Get-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance/phase.txt') -Raw).Trim() }
try {
    $status=& $ManagerPath -Action Status
    Assert (-not $status.paired) 'Default unexpectedly paired.'
    Assert ($status.requested -eq 'default auto') 'Fresh profile did not report the automatic default.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $work 'adb.log'))) 'Status accessed ADB.'
    & $ManagerPath -Action Configure -Adb $adb
    $phaseFile=Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance/phase.txt'
    $connectionFile=Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance/connection.txt'
    Assert (-not (Test-Path -LiteralPath $phaseFile)) 'Configure overrode the implicit automatic default.'
    Assert ($fakeNetwork.Connects -eq 0) 'Configure opened a runtime status connection.'
    Assert (-not (Test-Path -LiteralPath $connectionFile)) 'Normal Configure created a daily ADB dependency.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance/headset.token'))) 'Normal Configure exported a root token.'
    Assert ($status.transport -eq 'vd-frame') 'Normal transport is not the VD stream.'
    & $ManagerPath -Action Stop
    Assert ((Phase) -eq '1 restore') 'Offline stop did not save restore.'
    & $ManagerPath -Action Configure -Adb $adb
    Assert ((Phase) -eq '1 restore') 'Upgrade configuration overrode Stop.'
    & $ManagerPath -Action Pair -Adb $adb
    Assert ((Phase) -eq '3 auto') 'Pair did not enable the automatic default after confirmed handoff.'
    Assert ($fakeNetwork.Connects -eq 1 -and $fakeNetwork.Disposed -eq 1) 'Pair did not confirm and release its status connection.'
    $token=Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance/headset.token'
    Assert ((Get-Content -LiteralPath $token -Raw).Trim() -ceq ('a'*64)) 'Fixture token was not saved.'
    Assert ((Get-Acl -LiteralPath (Split-Path -Parent $token)).AreAccessRulesProtected) 'Pairing directory inherited broad permissions.'
    & $ManagerPath -Action Start -Adb $adb
    Assert ((Phase) -eq '5 auto') 'Start did not enable after successful status.'
    Assert ($fakeNetwork.Connects -eq 2 -and $fakeNetwork.Disposed -eq 2) 'Pair/Start did not each dispose their own client.'
    Assert ($fakeNetwork.Request -match '^VDM1 a{64} [a-f0-9]{32} 1 status 3000\n$') 'Manager sent a tracking-mode command instead of status.'
    Assert (($fakeNetwork.Events -join ',') -eq 'shutdown-send,release-read,dispose') 'Status socket handoff order was not acknowledged.'
    & $ManagerPath -Action Stop
    Assert ((Phase) -eq '6 restore') 'Stop was not monotonic.'
    Write-Host 'PASS: automatic fresh default, no-ADB status/stop, secure Pair enables after status, repeated Start, monotonic stop.'
    foreach ($release in @('timeout','data','shutdown-error')) {
        $fakeNetwork.Position=0; $fakeNetwork.Release=$release
        $disposed=$fakeNetwork.Disposed
        $pattern=if ($release -eq 'timeout') { 'Fixture release read timed out' } elseif ($release -eq 'data') { 'did not release the status connection' } else { 'Fixture shutdown failed' }
        Rejects { & $ManagerPath -Action Start -Adb $adb } $pattern
        Assert ((Phase) -match ' restore$') 'Failed handoff left auto enabled.'
        Assert ($fakeNetwork.Disposed -eq $disposed+1) 'Failed handoff leaked its client.'
    }
    $fakeNetwork.Release='eof'
    Write-Host 'PASS: send-half-close then peer EOF precedes auto; release timeout, extra data and shutdown failure stay off and dispose.'
    foreach ($response in @("VDR1 1 0 error unobserved error`n",'','invalid')) {
        $fakeNetwork.Reply=$response; $fakeNetwork.Position=0
        foreach ($action in @('Pair','Start')) {
            Rejects { & $ManagerPath -Action $action -Adb $adb }
            Assert ((Phase) -match ' restore$') 'Bad Pair/Start status left auto enabled.'
        }
    }
    $fakeNetwork.Ready=$false
    Rejects { & $ManagerPath -Action Start -Adb $adb }
    Assert ((Phase) -match ' restore$') 'Connection timeout left auto enabled.'
    $fakeNetwork.Ready=$true
    [IO.File]::WriteAllText((Join-Path $work 'collision'),'')
    Rejects { & $ManagerPath -Action Start -Adb $adb }
    Assert ((Phase) -match ' restore$') 'Port collision left auto enabled.'
    Remove-Item -LiteralPath (Join-Path $work 'collision')
    [IO.File]::WriteAllText((Join-Path $work 'forward.txt'),"FIXTURE-HEADSET tcp:29743 tcp:29743`n")
    & $ManagerPath -Action Disable -Adb $adb
    Assert ((Phase) -match ' restore$') 'Disable did not stop auto.'
    $log=Get-Content -LiteralPath (Join-Path $work 'adb.log') -Raw
    Assert ($log -match 'forward --remove tcp:29743') 'Own forward was not removed.'
    Assert ($log -match 'mixedvr.sh disable') 'Companion disable was not requested.'
    Assert (Test-Path -LiteralPath $token) 'Disable destroyed pairing.'
    $connects=$fakeNetwork.Connects
    $previousSequence=[UInt64]((Phase) -split ' ')[0]
    & $ManagerPath -Action Enable -Adb $adb
    Assert ((Phase) -eq "$($previousSequence+1) auto") 'Enable did not save the explicit choice.'
    Assert ($fakeNetwork.Connects -eq $connects) 'Normal Enable opened the legacy TCP channel.'
    & $ManagerPath -Action Stop
    [IO.File]::WriteAllText((Join-Path $work 'offline'),'')
    Rejects { & $ManagerPath -Action Disable -Adb $adb }
    Assert ((Phase) -match ' restore$') 'Offline disable did not at least disable local auto.'
    Write-Host 'PASS: malformed/failed/empty status, timeouts and forward collision stay off; disable restores local state, removes only own forward, keeps pairing.'
    Write-Host "All MixedVR manager fixture checks passed on PowerShell $($PSVersionTable.PSVersion)."
} finally {
    $env:LOCALAPPDATA=$savedLocal; $env:VDHS_FAKE_ADB_ROOT=$savedFake
    Remove-Item -LiteralPath $work -Recurse -Force
}
