# Source/capsule checks run without VD. Optional exact-original fixtures execute
# installer transactions only inside this script's newly created temp directory.
param([Parameter(Mandatory=$true)][string]$PackagePath, [string]$OriginalDriver, [string]$PreviousCapsule)
$ErrorActionPreference = 'Stop'
$PackagePath = [IO.Path]::GetFullPath($PackagePath)
if (-not ('VdPicoEnhance.NativePatch' -as [type])) { Add-Type -Path (Join-Path $PackagePath 'NativePatch.cs') }
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Rejects([scriptblock]$Action, [string]$Pattern = '.') {
    $failure=$null; try { & $Action } catch { $failure=$_ }
    Assert ($null -ne $failure) 'Expected operation to be rejected.'
    Assert ($failure.Exception.Message -match $Pattern) "Unexpected failure: $failure"
}
$metadata = Get-Content -LiteralPath (Join-Path $PackagePath 'capsule.json') -Raw | ConvertFrom-Json
$capsule = [IO.File]::ReadAllBytes((Join-Path $PackagePath 'vd-picoenhance.pe'))
Assert ([VdPicoEnhance.NativePatch]::Hash($capsule) -eq $metadata.capsule_sha256) 'Capsule hash mismatch.'
$pe = New-Object VdPicoEnhance.NativePatch+Pe -ArgumentList (,$capsule)
$exports = $pe.Exports()
foreach ($name in @('vd_native_run_frame','vd_native_runtime_table')) { Assert ($exports.ContainsKey($name)) "Missing capsule export: $name" }
foreach ($name in @('mv_trial_init','mv_trial_cleanup','mv_trial_create','mv_trial_update')) { Assert (-not $exports.ContainsKey($name)) "Base battery package contains switching hook: $name" }
Assert ($metadata.kind -eq 'battery-only-v1') 'Expected the battery-only capsule.'
foreach ($encoding in @([Text.Encoding]::ASCII, [Text.Encoding]::Unicode)) {
    $text = $encoding.GetString($capsule)
    Assert ($text -notmatch '(?i)[A-Z]:\\Users\\|mixedvr-vd-driver-20261020|headset\.token.*Documents') 'Machine-local data leaked into capsule.'
}
foreach ($name in @('install.ps1')) {
    $tokens=$null; $errors=$null
    $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PackagePath $name),[ref]$tokens,[ref]$errors)
    Assert ($errors.Count -eq 0) "Invalid PowerShell syntax in $name"
}
Rejects { [VdPicoEnhance.NativePatch]::Apply([byte[]]@(1,2,3),$capsule) }
Write-Host 'PASS: own-code capsule metadata/exports, portable paths, script syntax, unsupported original refusal.'
if (-not $OriginalDriver) { Write-Host 'Vendor original not supplied: installer transaction fixtures were not run.'; return }
$original = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($OriginalDriver))
Assert ([VdPicoEnhance.NativePatch]::Hash($original) -eq [VdPicoEnhance.NativePatch]::OriginalSha256) 'Fixture is not the supported original.'
$parent = Join-Path ([IO.Path]::GetTempPath()) 'opencode'
New-Item -ItemType Directory -Force -Path $parent | Out-Null
$work = Join-Path $parent ('vd-pc-install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$target = Join-Path $work 'driver_VirtualDesktop.dll'
$installer = Join-Path $PackagePath 'install.ps1'
$savedLocal = $env:LOCALAPPDATA
$env:LOCALAPPDATA = Join-Path $work 'profile'
$fakeRuntime = @{ Running=$false }
# The installer sees this owned fake, never a live process or SteamVR API.
function Get-Process { param([string]$Name,[string]$ErrorAction) if ($fakeRuntime.Running) { [pscustomobject]@{ProcessName='vrserver'} } }
try {
    [IO.File]::WriteAllBytes($target,$original)
    $fakeRuntime.Running=$true
    Rejects { & $installer -Action Install -DriverPath $target -PackagePath $PackagePath } 'Exit SteamVR'
    Assert ([VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($target)) -eq [VdPicoEnhance.NativePatch]::OriginalSha256) 'Running-process guard changed original.'
    Assert (-not (Test-Path -LiteralPath ($target+'.vd-picoenhance-original'))) 'Guard created a backup before refusing.'
    $fakeRuntime.Running=$false
    & $installer -Action Install -DriverPath $target -PackagePath $PackagePath
    $expected = [VdPicoEnhance.NativePatch]::Hash([VdPicoEnhance.NativePatch]::Apply($original,$capsule))
    $patched = [VdPicoEnhance.NativePatch]::Apply($original,$capsule)
    $oldPe = New-Object VdPicoEnhance.NativePatch+Pe -ArgumentList (,$original)
    foreach ($site in @(0x7fd58,0x7fd60,0xd215,0xd25f)) {
        $offset=$oldPe.Offset([uint32]$site)
        Assert ([Convert]::ToBase64String($original[$offset..($offset+6)]) -eq [Convert]::ToBase64String($patched[$offset..($offset+6)])) 'Battery-only patch changed a switching hook site.'
    }
    Assert ([VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($target)) -eq $expected) 'Fresh install differs from graft.'
    $profile=Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance'
    Assert (-not (Test-Path -LiteralPath (Join-Path $profile 'phase.txt'))) 'Fresh install overrode the automatic default.'
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    [IO.File]::WriteAllText((Join-Path $profile 'phase.txt'),"1 restore`n")
    & $installer -Action Install -DriverPath $target -PackagePath $PackagePath
    Assert ((& $installer -Action Status -DriverPath $target -PackagePath $PackagePath).owned_patch) 'Idempotent install lost ownership.'
    Assert ((Get-Content -LiteralPath (Join-Path $profile 'phase.txt') -Raw).Trim() -eq '1 restore') 'Reinstall overrode a saved Stop choice.'
    Write-Host 'PASS: loaded-driver refusal, exact backup, fresh installation and idempotence.'
    [IO.File]::WriteAllBytes($target,[byte[]]@(9,8,7))
    Rejects { & $installer -Action Install -DriverPath $target -PackagePath $PackagePath }
    Rejects { & $installer -Action Uninstall -DriverPath $target -PackagePath $PackagePath }
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($target)) -eq 'CQgH') 'Foreign update was overwritten.'
    [IO.File]::WriteAllBytes($target,[VdPicoEnhance.NativePatch]::Apply($original,$capsule))
    $profile=Join-Path $env:LOCALAPPDATA 'VD-PicoEnhance'
    New-Item -ItemType Directory -Force -Path $profile | Out-Null
    [IO.File]::WriteAllText((Join-Path $profile 'phase.txt'),"7 auto`n")
    & $installer -Action Uninstall -DriverPath $target -PackagePath $PackagePath
    Assert ((Get-Content -LiteralPath (Join-Path $profile 'phase.txt') -Raw).Trim() -eq '7 auto') 'Battery-only uninstall changed unrelated saved input control.'
    Assert ([VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($target)) -eq [VdPicoEnhance.NativePatch]::OriginalSha256) 'Uninstall did not restore exact official bytes.'
    & $installer -Action Uninstall -DriverPath $target -PackagePath $PackagePath
    Write-Host 'PASS: refuse unknown external updates, exact uninstall and repeated uninstall.'
    if ($PreviousCapsule) {
        $savedPhase=Get-Content -LiteralPath (Join-Path $profile 'phase.txt') -Raw
        $previous = [VdPicoEnhance.NativePatch]::Apply($original,[IO.File]::ReadAllBytes([IO.Path]::GetFullPath($PreviousCapsule)))
        [IO.File]::WriteAllBytes($target,$previous)
        $record = @{original_sha256=[VdPicoEnhance.NativePatch]::OriginalSha256; patched_sha256=[VdPicoEnhance.NativePatch]::Hash($previous); previous_sha256=[VdPicoEnhance.NativePatch]::OriginalSha256}
        [IO.File]::WriteAllText(($target+'.vd-picoenhance.json'),($record|ConvertTo-Json))
        & $installer -Action Install -DriverPath $target -PackagePath $PackagePath
        Assert ([VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($target)) -eq $expected) 'Upgrade differs from fresh install.'
        Assert ((Get-Content -LiteralPath (Join-Path $profile 'phase.txt') -Raw) -ceq $savedPhase) 'Upgrade overrode a saved Stop choice.'
        $record = Get-Content -LiteralPath ($target+'.vd-picoenhance.json') -Raw | ConvertFrom-Json
        [IO.File]::WriteAllBytes($target,$previous) # interruption after receipt, before DLL replace
        & $installer -Action Uninstall -DriverPath $target -PackagePath $PackagePath
        Assert ([VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($target)) -eq [VdPicoEnhance.NativePatch]::OriginalSha256) 'Interrupted transaction did not restore.'
        Write-Host 'PASS: prior owned patch upgrade and interrupted-before-replace recovery.'
    }
    Write-Host "All supplied PC package checks passed on PowerShell $($PSVersionTable.PSVersion)."
} finally { $env:LOCALAPPDATA=$savedLocal; Remove-Item -LiteralPath $work -Recurse -Force }
