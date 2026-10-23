# Owned bundle, ADB and elevation fixtures only; never touches a real installation.
param([Parameter(Mandatory=$true)][string]$InstallerPath)
$ErrorActionPreference='Stop'
function Assert([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Rejects([scriptblock]$Operation,[string]$Pattern) {
    $errorRecord=$null
    try { & $Operation } catch { $errorRecord=$_ }
    Assert ($null -ne $errorRecord -and $errorRecord.Exception.Message -match $Pattern) "Unexpected failure: $errorRecord"
}
$work=Join-Path ([IO.Path]::GetTempPath()) ('opencode/vd-unified-'+[Guid]::NewGuid().ToString('N'))
$bundle=Join-Path $work 'bundle with spaces'
New-Item -ItemType Directory -Path (Join-Path $bundle 'pc') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $bundle 'knuckles/pc/driver_picomixedvr/bin/win64') -Force | Out-Null
$installer=Join-Path $bundle 'install-vd-picoenhance.ps1'
Copy-Item -LiteralPath $InstallerPath -Destination $installer
$saved=$env:VD_UNIFIED_FIXTURE
$savedLocal=$env:LOCALAPPDATA
$env:LOCALAPPDATA=Join-Path $work 'profile'
$env:VD_UNIFIED_FIXTURE=$work
$utf8=[Text.UTF8Encoding]::new($false)
$log=Join-Path $work 'calls.txt'
function Calls { if (Test-Path -LiteralPath $log) { return [IO.File]::ReadAllText($log) }; return '' }
function Clear-Calls { [IO.File]::WriteAllText($log,'') }
$fixture=@{Running=$false}
function Get-Process { param([string]$Name,[string]$ErrorAction) if ($fixture.Running) { [pscustomobject]@{ProcessName='vrserver'} } }
function Start-Process {
    param([string]$FilePath,[string[]]$ArgumentList,[string]$Verb,[switch]$Wait,[switch]$PassThru)
    Assert ($Verb -eq 'RunAs' -and $Wait -and $PassThru) 'PC elevation did not wait for completion.'
    Assert ($FilePath -match 'powershell\.exe$') 'Unexpected elevated executable.'
    $index=[Array]::IndexOf($ArgumentList,'-Action')
    $operation=$ArgumentList[$index+1]
    [IO.File]::AppendAllText($log,"pc $operation`n")
    [pscustomobject]@{ExitCode= $(if (Test-Path -LiteralPath (Join-Path $work 'fail-pc')) { 1 } else { 0 })}
}
$adb=Join-Path $work 'adb.ps1'
$adbSource=@'
[IO.File]::AppendAllText((Join-Path $env:VD_UNIFIED_FIXTURE 'calls.txt'),('adb '+($args -join ' ')+"`n"))
$global:LASTEXITCODE=0
if ($args[0] -eq 'devices') { 'List of devices attached'; 'FIXTURE-HEADSET device'; return }
if ($args[-1] -eq 'su -c id') { 'uid=0(root)'; return }
if ($args[-1] -match 'magisk --install-module') { 'Done'; return }
'fixture ok'
'@
[IO.File]::WriteAllText($adb,$adbSource,$utf8)
$pcSource=@'
param([string]$Action,[string]$DriverPath,[string]$PackagePath)
[IO.File]::AppendAllText((Join-Path $env:VD_UNIFIED_FIXTURE 'calls.txt'),"pc $Action`n")
if ($Action -eq 'Status') { [pscustomobject]@{official= -not (Test-Path (Join-Path $env:VD_UNIFIED_FIXTURE 'unsupported'));owned_patch=$false}; return }
if (Test-Path (Join-Path $env:VD_UNIFIED_FIXTURE 'fail-pc')) { throw 'PC fixture install failed' }
'@
[IO.File]::WriteAllText((Join-Path $bundle 'pc/install.ps1'),$pcSource,$utf8)
$managerSource=@'
param([string]$Action,[string]$Adb,[string]$Serial,[string]$HeadsetAddress,[string]$PcAddress)
[IO.File]::AppendAllText((Join-Path $env:VD_UNIFIED_FIXTURE 'calls.txt'),"manager $Action`n")
if ($Action -eq 'Install' -and (Test-Path (Join-Path $env:VD_UNIFIED_FIXTURE 'fail-configure'))) { throw 'Configure fixture failed' }
if ($Action -eq 'Install') {
    $path=Join-Path $env:LOCALAPPDATA 'PicoMixedVR'
    [IO.Directory]::CreateDirectory($path) | Out-Null
    [IO.File]::WriteAllText((Join-Path $path 'installation.json'),'{"installed":true}')
}
'@
[IO.File]::WriteAllText((Join-Path $bundle 'knuckles/manage.ps1'),$managerSource,$utf8)
foreach ($name in @('vdhs_zygisk.zip','hand_mesh_fb.bin','pc/vd-picoenhance.pe','pc/capsule.json','driver.dll','knuckles/PicoHand-Knuckles.zip','knuckles/pc/driver_picomixedvr/bin/win64/driver_picomixedvr.dll')) {
    [IO.File]::WriteAllText((Join-Path $bundle $name),'owned fixture')
}
$driver=Join-Path $bundle 'driver.dll'
try {
    & $installer -Adb $adb -DriverPath $driver
    $calls=Calls
    Assert ($calls.IndexOf('pc Install') -lt $calls.IndexOf('magisk --install-module')) 'PC install did not precede module changes.'
    Assert ($calls -notmatch 'manager ') 'Base installation enabled the optional module.'
    Assert ($calls -notmatch 'manager (Pair|Start)') 'Installer required the old runtime start flow.'
    Assert ([regex]::Matches($calls,'adb -s FIXTURE-HEADSET reboot').Count -eq 1) 'Expected one final reboot.'
    Write-Host 'PASS: base installs battery/module/mesh without optional input control.'
    Clear-Calls
    & $installer -Adb $adb -DriverPath $driver -WithKnuckles
    $calls=Calls
    Assert ($calls.IndexOf('manager Install') -gt $calls.IndexOf('magisk --install-module')) 'Optional pairing did not follow pending module install.'
    Assert ($calls.IndexOf('manager Install') -lt $calls.IndexOf('adb -s FIXTURE-HEADSET reboot')) 'Reboot preceded optional pairing.'
    Clear-Calls
    & $installer -Adb $adb -DriverPath $driver -NoReboot
    Assert ((Calls) -match 'manager Install') 'Upgrade dropped the installed optional module.'
    Assert ((Calls) -notmatch 'reboot') '-NoReboot was ignored.'

    Clear-Calls
    $fixture.Running=$true
    Rejects { & $installer -Adb $adb -DriverPath $driver } 'Exit SteamVR'
    Assert ((Calls) -eq '') 'Running-runtime refusal touched ADB or installed files.'
    $fixture.Running=$false
    [IO.File]::WriteAllText((Join-Path $work 'unsupported'),'')
    Rejects { & $installer -Adb $adb -DriverPath $driver } 'Unsupported VD'
    Assert ((Calls) -notmatch 'adb |pc Install') 'Unsupported driver led to device changes.'
    Remove-Item -LiteralPath (Join-Path $work 'unsupported')

    Clear-Calls
    [IO.File]::WriteAllText((Join-Path $work 'fail-pc'),'')
    Rejects { & $installer -Adb $adb -DriverPath $driver } 'PC .*failed'
    Assert ((Calls) -notmatch 'magisk --install-module|adb -s FIXTURE-HEADSET push|reboot') 'Failed PC install changed the headset.'
    Remove-Item -LiteralPath (Join-Path $work 'fail-pc')
    Clear-Calls
    [IO.File]::WriteAllText((Join-Path $work 'fail-configure'),'')
    Rejects { & $installer -Adb $adb -DriverPath $driver -WithKnuckles } 'Configure fixture failed'
    Assert ((Calls) -notmatch 'adb -s FIXTURE-HEADSET reboot') 'Failed configuration still rebooted.'
    Remove-Item -LiteralPath (Join-Path $work 'fail-configure')
    Write-Host 'PASS: running/unsupported PC rejected before changes; install/configuration failures stop the flow.'

    Clear-Calls
    & $installer -Adb $adb -Mode 1 -NoReboot
    Assert ((Calls) -notmatch 'pc |manager |reboot') 'Injection-only mode configured PC switching.'
    Rejects { & $installer -Adb $adb -WithKnuckles -HeadsetOnly } 'requires a full'
    Rejects { & $installer -Adb $adb -WithKnuckles -Mode 1 } 'requires a full'
    Clear-Calls
    & $installer -Action Disable -Adb $adb
    & $installer -Action Enable -Adb $adb
    & $installer -Action Configure -Adb $adb -Serial FIXTURE-NEW
    & $installer -Action Status -DriverPath $driver
    Assert ((Calls) -match 'manager Disable' -and (Calls) -match 'manager Enable' -and (Calls) -match 'manager Status') 'Unified management actions were not routed.'
    Assert ((Calls) -notmatch 'manager Start') 'Normal Enable entered legacy ADB forwarding.'
    Assert ((Calls) -match 'manager Configure' -and (Calls) -notmatch 'pc Install|magisk|reboot') 'Connection-only configuration reran installation.'
    Clear-Calls
    & $installer -Action Uninstall -Adb $adb -DriverPath $driver
    $calls=Calls
    Assert ($calls.IndexOf('manager Uninstall') -lt $calls.IndexOf('pc Uninstall')) 'Uninstall did not restore switching first.'
    Assert ($calls -match '/vdhs_zygisk/remove' -and $calls -match 'adb -s FIXTURE-HEADSET reboot') 'Headset uninstall/reboot was not requested.'
    Clear-Calls
    & $installer -Action UninstallKnuckles -Adb $adb
    Assert ((Calls) -match 'manager Uninstall' -and (Calls) -notmatch 'pc |magisk|reboot|vdhs_zygisk/remove') 'Optional-only removal changed the base installation.'
    Write-Host "All unified installer fixture checks passed on PowerShell $($PSVersionTable.PSVersion)."
} finally {
    $env:VD_UNIFIED_FIXTURE=$saved
    $env:LOCALAPPDATA=$savedLocal
    Remove-Item -LiteralPath $work -Recurse -Force
}
