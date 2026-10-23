#Requires -Version 5.1
param(
    [ValidateSet('Configure','Enable','Pair','Start','Stop','Disable','Status')][string]$Action = 'Status',
    [string]$Adb,
    [string]$Serial
)
$ErrorActionPreference = 'Stop'
$state = Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance'
$phasePath = Join-Path $state 'phase.txt'
$pairingPath = Join-Path $state 'pairing.json'
$tokenPath = Join-Path $state 'headset.token'
$utf8 = [Text.UTF8Encoding]::new($false)
function Protect-State {
    New-Item -ItemType Directory -Force -Path $state | Out-Null
    $acl = Get-Acl -LiteralPath $state
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $rules = @($acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]))
    $identities = @($rules | ForEach-Object { $_.IdentityReference.Value } | Sort-Object)
    $expected = @($sid,'S-1-5-18') | Sort-Object
    $secured = $acl.AreAccessRulesProtected -and $rules.Count -eq 2 -and ($identities -join ',') -eq ($expected -join ',')
    foreach ($rule in $rules) {
        $secured = $secured -and $rule.AccessControlType -eq 'Allow' -and $rule.FileSystemRights -eq 'FullControl' -and
                   $rule.InheritanceFlags -eq 'ContainerInherit, ObjectInherit' -and $rule.PropagationFlags -eq 'None' -and -not $rule.IsInherited
    }
    # A repeated no-op Set-Acl on a protected directory can request audit
    # privileges. Verify the complete DACL before skipping the redundant write.
    if (-not $secured) {
        $acl.SetSecurityDescriptorSddlForm("D:P(A;OICI;FA;;;$sid)(A;OICI;FA;;;SY)", [Security.AccessControl.AccessControlSections]::Access)
        Set-Acl -LiteralPath $state -AclObject $acl
    }
}
function Save-Pairing {
    Protect-State
    $token = Invoke-Adb -Arguments @('exec-out', "su -c 'cat /data/adb/vdhs_mixedvr/token'")
    if ($token -cnotmatch '^[a-f0-9]{64}$') { throw 'Headset pairing token is invalid.' }
    [IO.File]::WriteAllText($tokenPath, "$token`n", $utf8)
    [IO.File]::WriteAllText($pairingPath, (@{serial=$Serial;adb=[IO.Path]::GetFullPath($Adb)} | ConvertTo-Json), $utf8)
    [IO.File]::WriteAllText((Join-Path $state 'connection.txt'), ([IO.Path]::GetFullPath($Adb)+"`n$Serial`n"), [Text.Encoding]::Unicode)
    $token = $null
}
function Save-Phase([string]$Label) {
    New-Item -ItemType Directory -Force -Path $state | Out-Null
    [UInt64]$sequence = 0
    if (Test-Path -LiteralPath $phasePath) {
        $line = Get-Content -LiteralPath $phasePath -Raw
        if ($line -match '^([0-9]+) ') { $sequence = [UInt64]$Matches[1] }
    }
    $next = $sequence + 1
    $temporary = $phasePath + '.tmp-' + [Guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText($temporary, "$next $Label`n", $utf8)
    if (Test-Path -LiteralPath $phasePath) { [IO.File]::Replace($temporary, $phasePath, [NullString]::Value) }
    else { [IO.File]::Move($temporary, $phasePath) }
}
if ($Action -eq 'Stop') {
    Save-Phase 'restore'
    Write-Host 'MixedVR stop requested. The VD worker restores HAND and the headset baseline; the helper lease also restores on disconnect.'
    return
}
if ($Action -eq 'Status') {
    $phase = if (Test-Path -LiteralPath $phasePath) { (Get-Content -LiteralPath $phasePath -Raw).Trim() } else { 'default auto' }
    [pscustomobject]@{ transport='vd-frame'; paired=(Test-Path -LiteralPath $tokenPath); requested=$phase; steamvr_running=[bool](Get-Process -Name vrserver -ErrorAction SilentlyContinue) }
    return
}
if ($Action -eq 'Disable') { Save-Phase 'restore' }
if (Test-Path -LiteralPath $pairingPath) {
    $savedPairing = Get-Content -LiteralPath $pairingPath -Raw | ConvertFrom-Json
    if (-not $Adb -and $savedPairing.adb) { $Adb = $savedPairing.adb }
    if (-not $Serial) { $Serial = $savedPairing.serial }
}
if (-not $Adb) { $command = Get-Command adb -ErrorAction SilentlyContinue; if ($command) { $Adb = $command.Source } }
if (-not $Adb) { throw 'ADB is needed for installation or persistent headset management, not daily switching. Supply -Adb or put adb on PATH.' }
function Invoke-Adb([string[]]$Arguments) {
    $result = @(& $Adb -s $Serial @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "ADB $($Arguments[0]) failed. Check device connection and root authorization." }
    return ($result -join "`n").Trim()
}
if (-not $Serial) {
    $devices = @(& $Adb devices)
    if ($LASTEXITCODE -ne 0) { throw 'ADB device discovery failed.' }
    $online = @($devices | Where-Object { $_ -match '^([^\s]+)\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($online.Count -ne 1) { throw 'Specify -Serial for exactly one connected, authorized headset.' }
    $Serial = $online[0]
}
if ((Invoke-Adb -Arguments @('get-state')) -ne 'device') { throw 'Selected headset is not online.' }
if ($Action -eq 'Configure') {
    # Installation can use Magisk's pending module before the single reboot.
    # Never start a pending executable or overwrite an existing disabled choice.
    $provision = 'su -c ''set -e; d=/data/adb/modules_update/vdhs_zygisk; [ -d $d ] || d=/data/adb/modules/vdhs_zygisk; r=/data/adb/vdhs_mixedvr; if [ x$(cat $r/requested 2>/dev/null) = x0 ] || { [ -f $r/token ] && [ ! -f $r/requested ] && [ ! -f $r/enabled ]; }; then :; else sh $d/mixedvr.sh enable; fi'''
    Write-Output (Invoke-Adb -Arguments @('shell', $provision))
    Protect-State
    # The stream is already authenticated by VD. Do not create runtime ADB
    # configuration or export a root token for the normal installation path.
    # Existing legacy pairing files remain available for explicit diagnostics.
    Write-Host 'VD stream switching configured. Saved Stop/Disable choices are unchanged; daily operation needs no ADB.'
    return
}
if ($Action -eq 'Enable') {
    Write-Output (Invoke-Adb -Arguments @('shell', "su -c 'sh /data/adb/modules/vdhs_zygisk/mixedvr.sh enable'"))
    Save-Phase 'auto'
    Write-Host 'MixedVR enabled. The VD driver and headset mod exchange intent and execution receipts over the VD stream.'
    return
}
if ($Action -eq 'Disable') {
    Write-Output (Invoke-Adb -Arguments @('shell', "su -c 'sh /data/adb/modules/vdhs_zygisk/mixedvr.sh disable'"))
    # Remove only our selected-device forward, never another device's mapping.
    $forwards = Invoke-Adb -Arguments @('forward','--list')
    if ($forwards -match (('(?m)^' + [regex]::Escape($Serial)) + '\s+tcp:29743\s+tcp:29743\s*$')) {
        $null = Invoke-Adb -Arguments @('forward','--remove','tcp:29743')
    }
    Write-Host 'MixedVR disabled and headset baseline restored. Pairing files retained for explicit re-enabling.'
    return
}
if ($Action -eq 'Pair') {
    Save-Phase 'restore'
    if (Get-Process -Name vrserver -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 2500 }
    Write-Output (Invoke-Adb -Arguments @('shell', "su -c 'sh /data/adb/modules/vdhs_zygisk/mixedvr.sh enable'"))
    Save-Pairing
    Write-Host 'Paired. Starting the default-enabled automatic switching connection.'
}
if (-not (Test-Path -LiteralPath $tokenPath)) { throw 'Pair this headset first with -Action Pair.' }
if ($Action -ne 'Pair') {
    Save-Phase 'restore'
    if (Get-Process -Name vrserver -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 2500 }
}
Write-Output (Invoke-Adb -Arguments @('shell', "su -c 'sh /data/adb/modules/vdhs_zygisk/mixedvr.sh start'"))
$forwards = Invoke-Adb -Arguments @('forward','--list')
$matching = ('(?m)^' + [regex]::Escape($Serial)) + '\s+tcp:29743\s+tcp:29743\s*$'
if ($forwards -notmatch $matching) { $null = Invoke-Adb -Arguments @('forward','--no-rebind','tcp:29743','tcp:29743') }
$token = (Get-Content -LiteralPath $tokenPath -Raw).Trim()
if ($token -cnotmatch '^[a-f0-9]{64}$') { throw 'Pairing token is invalid; pair again.' }
$session = [Guid]::NewGuid().ToString('N')
$client = New-Object Net.Sockets.TcpClient
try {
    if (-not $client.ConnectAsync('127.0.0.1',29743).Wait(2000)) { throw 'Headset helper connection timed out.' }
    $stream = $client.GetStream(); $stream.ReadTimeout = 2500; $stream.WriteTimeout = 1500
    $request = [Text.Encoding]::ASCII.GetBytes("VDM1 $token $session 1 status 3000`n")
    $stream.Write($request,0,$request.Length)
    $response = ''
    for ($i = 0; $i -lt 128; ++$i) {
        $b = $stream.ReadByte()
        if ($b -lt 0) { throw 'Headset helper closed without status.' }
        if ($b -eq 10) { break }
        $response += [char]$b
    }
    if ($response -notmatch '^VDR1 1 [012] (0|1|null) (unobserved|blocked|warming|ready) ok$') { throw 'Headset helper status was not ready; automatic switching remains off.' }
    # The helper accepts one client. Wait for its close acknowledgement before
    # allowing the driver to connect; Dispose alone can race the helper's accept.
    $client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
    if ($stream.ReadByte() -ne -1) { throw 'Headset helper did not release the status connection; automatic switching remains off.' }
} finally { $client.Dispose(); $token = $null; $request = $null }
Save-Phase 'auto'
Write-Host 'MixedVR enabled. Start VD/SteamVR normally. Without a real Index input decision the original input remains unchanged.'
