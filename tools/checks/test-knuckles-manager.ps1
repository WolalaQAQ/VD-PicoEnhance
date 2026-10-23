# Owned filesystem, process and headset fakes only. No installed driver or ADB.
param([string]$ManagerRoot = (Join-Path $PSScriptRoot '../../knuckles'))
$ErrorActionPreference='Stop'
. (Join-Path $ManagerRoot 'manager-core.ps1')
$work=Join-Path ([IO.Path]::GetTempPath()) ('opencode/knuckles-manager-'+[Guid]::NewGuid().ToString('N'))
$package=Join-Path $work 'package'
$config=Join-Path $work 'private config'
$ctx=@{running=$false; calls=[Collections.Generic.List[string]]::new(); failRestore=$false; badNetwork=$false; assertions=0}
function Assert([bool]$Condition,[string]$Message) { $ctx.assertions++; if (-not $Condition) { throw $Message } }
function Rejects([scriptblock]$Operation,[string]$Pattern) {
    $failure=$null; try { & $Operation } catch { $failure=$_ }
    Assert ($null -ne $failure -and $failure.Exception.Message -match $Pattern) "Unexpected failure: $failure"
}
function Get-Process { param([string]$Name,[string]$ErrorAction) if ($ctx.running) { [pscustomobject]@{ProcessName='vrserver'} } }
function Invoke-KnucklesAdb($O,[string[]]$Arguments) { $ctx.calls.Add('adb '+($Arguments -join ' ')) }
function Invoke-KnucklesRoot($O,[string]$Command) {
    $ctx.calls.Add('root '+$Command)
    if ($ctx.failRestore -and $Command -eq 'sh /data/adb/modules/pico_mixedvr_standalone/standalone.sh disable') { throw 'fixture restore failed' }
    if ($Command -eq 'cat /data/adb/pico_mixedvr_standalone/token') { return ('a'*64) }
    if ($Command -eq 'cat /data/adb/pico_mixedvr_standalone/network') {
        if ($ctx.badNetwork) { return '%snmalformed' }
        return ($O.HeadsetAddress + ' ' + $O.PcAddress)
    }
    if ($Command -eq 'wc -c < /data/adb/pico_mixedvr_standalone/network') { return [string]($O.HeadsetAddress.Length + $O.PcAddress.Length + 2) }
}
$savedRegistry=$env:KNUCKLES_FIXTURE_REGISTRY
try {
    [IO.Directory]::CreateDirectory((Join-Path $package 'pc/driver_picomixedvr/bin/win64')) | Out-Null
    Copy-Item -LiteralPath (Join-Path $ManagerRoot 'pc/control.ps1') -Destination (Join-Path $package 'pc')
    [IO.File]::WriteAllText((Join-Path $package 'pc/driver_picomixedvr/driver.vrdrivermanifest'),'{"name":"picomixedvr"}')
    foreach ($name in @('PicoHand-Knuckles.zip','pc/driver_picomixedvr/OPENVR-LICENSE.txt','pc/driver_picomixedvr/ZIG-RUNTIME-NOTICES.txt','pc/driver_picomixedvr/bin/win64/driver_picomixedvr.dll')) {
        [IO.File]::WriteAllText((Join-Path $package $name),'owned fixture')
    }
    $registry=Join-Path $work 'openvrpaths.vrpath'
    [IO.File]::WriteAllText($registry,(@{external_drivers=@((Join-Path $work 'unrelated-driver'));runtime=@('unchanged-runtime');version=1}|ConvertTo-Json))
    $env:KNUCKLES_FIXTURE_REGISTRY=$registry
    $tool=Join-Path $work 'vrpathreg.ps1'
    [IO.File]::WriteAllText($tool,@'
param([string]$Verb,[string]$Path)
$r=Get-Content -LiteralPath $env:KNUCKLES_FIXTURE_REGISTRY -Raw | ConvertFrom-Json
if ($Verb -eq 'adddriver') { $r.external_drivers=@($r.external_drivers)+$Path }
else { $r.external_drivers=@($r.external_drivers | Where-Object { $_ -ne $Path }) }
[IO.File]::WriteAllText($env:KNUCKLES_FIXTURE_REGISTRY,($r | ConvertTo-Json))
$global:LASTEXITCODE=0
'@)
    $o=@{Config=$config;Package=$package;Registry=$registry;VrPathReg=$tool;Adb='never-run-adb';Serial='owned-device';HeadsetAddress=([Net.IPAddress]::new([byte[]](10,3,2,1))).ToString();PcAddress=([Net.IPAddress]::new([byte[]](10,3,2,2))).ToString()}
    Assert (-not (Invoke-Knuckles $o 'Status').installed) 'Fresh status claimed installed.'
    Assert ((Get-KnucklesPcAddress '127.0.0.1') -eq '127.0.0.1') 'PC route selection failed on the owned loopback route.'
    Assert ($ctx.calls.Count -eq 0 -and -not (Test-Path -LiteralPath $config)) 'Fresh status wrote state or contacted headset.'
    $ctx.running=$true
    Rejects { Invoke-Knuckles $o 'Install' } 'Exit SteamVR'
    Assert ($ctx.calls.Count -eq 0 -and -not (Test-Path -LiteralPath $config)) 'Running-runtime rejection wrote state.'
    $ctx.running=$false
    Invoke-Knuckles $o 'Install'
    Assert ((Invoke-Knuckles $o 'Status').installed) 'Install receipt missing.'
    Assert (([IO.File]::ReadAllText((Join-Path $config 'phase.txt'))).Trim() -eq '1 auto') 'Selected fresh module did not default to auto.'
    Assert (([IO.File]::ReadAllText((Join-Path $config 'headset.token'))).Trim() -ceq ('a'*64)) 'Token not preserved.'
    $r=Get-Content -LiteralPath $registry -Raw | ConvertFrom-Json
    Assert ($r.external_drivers.Count -eq 2 -and $r.runtime[0] -eq 'unchanged-runtime') 'Install changed unrelated registrations.'
    Assert ((Get-Acl -LiteralPath $config).AreAccessRulesProtected) 'Private pairing DACL not protected.'
    $all=$ctx.calls -join "`n"
    Assert ($all -match 'modules_update/vdhs_zygisk' -and $all -match 'mixedvr.sh"? disable' -and $all -notmatch 'forward|vdhs_mixedvr.* start') 'Install omitted staged recovery or reused the old communication owner.'
    Assert ($all -match 'choice=1' -and $all -match 'standalone.sh.*disable') 'Configure omitted explicit disable preservation.'
    $ctx.badNetwork=$true
    Rejects { Invoke-Knuckles $o 'Configure' } 'network pairing readback differs'
    Assert (([IO.File]::ReadAllText((Join-Path $config 'phase.txt'))).Trim() -eq '1 auto') 'Failed pairing changed saved control.'
    $ctx.badNetwork=$false
    Invoke-Knuckles $o 'Disable'
    $stopped=[IO.File]::ReadAllText((Join-Path $config 'phase.txt'))
    Assert ($stopped.Trim() -eq '2 restore') 'Disable did not save Restore.'
    Invoke-Knuckles $o 'Install'
    Assert ([IO.File]::ReadAllText((Join-Path $config 'phase.txt')) -ceq $stopped) 'Upgrade overrode saved Stop.'
    Assert (@((Get-Content -LiteralPath $registry -Raw | ConvertFrom-Json).external_drivers).Count -eq 2) 'Upgrade duplicated registration.'
    Assert (Test-Path -LiteralPath (Join-Path $config 'driver/bin/win64/driver_picomixedvr.dll.previous')) 'Owned prior provider backup missing.'
    Invoke-Knuckles $o 'Enable'
    Assert ((Invoke-Knuckles $o 'Status').saved_control -eq '3 auto') 'Enable did not rearm explicitly.'
    $ctx.failRestore=$true
    $ctx.calls.Clear()
    Rejects { Invoke-Knuckles $o 'Uninstall' } 'fixture restore failed'
    Assert ((Invoke-Knuckles $o 'Status').installed) 'Failed recovery discarded install receipt.'
    Assert (($ctx.calls -join "`n") -notmatch 'touch .*remove') 'Failed recovery marked module for removal.'
    Assert (@((Get-Content -LiteralPath $registry -Raw | ConvertFrom-Json).external_drivers).Count -eq 2) 'Failed recovery unregistered provider.'
    $ctx.failRestore=$false
    Invoke-Knuckles $o 'Uninstall'
    Assert (-not (Invoke-Knuckles $o 'Status').installed) 'Uninstall left installed selection.'
    Assert ((Test-Path -LiteralPath (Join-Path $config 'headset.token')) -and (Test-Path -LiteralPath (Join-Path $config 'driver/bin/win64/driver_picomixedvr.dll'))) 'Uninstall discarded pairing or owned recovery files.'
    $r=Get-Content -LiteralPath $registry -Raw | ConvertFrom-Json
    Assert ($r.external_drivers.Count -eq 1 -and $r.external_drivers[0] -like '*unrelated-driver') 'Uninstall changed other registrations.'
    Rejects { Invoke-Knuckles $o 'Enable' } 'not installed'
    Rejects { Knuckles-PrivateAddress '127.0.0.1' } 'private IPv4'
    Write-Host "PASS: $($ctx.assertions) Knuckles assertions on PowerShell $($PSVersionTable.PSVersion); all headset/process/registration targets owned."
} finally {
    $env:KNUCKLES_FIXTURE_REGISTRY=$savedRegistry
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
