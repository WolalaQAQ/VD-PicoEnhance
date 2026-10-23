#Requires -Version 5.1
param(
    [ValidateSet('Install','Uninstall','Status')][string]$Action = 'Status',
    [string]$DriverPath = (Join-Path $env:ProgramFiles 'Virtual Desktop Streamer/OpenVRDriver/bin/win64/driver_VirtualDesktop.dll'),
    [string]$PackagePath = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'
if (-not ('VdPicoEnhance.NativePatch' -as [type])) { Add-Type -Path (Join-Path $PackagePath 'NativePatch.cs') }
$DriverPath = [IO.Path]::GetFullPath($DriverPath)
$receiptPath = $DriverPath + '.vd-picoenhance.json'
$backupPath = $DriverPath + '.vd-picoenhance-original'
$current = [IO.File]::ReadAllBytes($DriverPath)
$currentHash = [VdPicoEnhance.NativePatch]::Hash($current)
$receipt = if (Test-Path -LiteralPath $receiptPath) { Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json } else { $null }
$owned = $receipt -and ($currentHash -eq $receipt.patched_sha256 -or $currentHash -eq $receipt.previous_sha256)
if ($Action -eq 'Status') {
    [pscustomobject]@{ driver=$DriverPath; official=($currentHash -eq [VdPicoEnhance.NativePatch]::OriginalSha256); owned_patch=[bool]$owned; sha256=$currentHash; backup_present=(Test-Path -LiteralPath $backupPath) }
    return
}
if (Get-Process -Name vrserver -ErrorAction SilentlyContinue) { throw 'Exit SteamVR before changing its loaded VD driver. No files were changed.' }
if ($currentHash -ne [VdPicoEnhance.NativePatch]::OriginalSha256 -and -not $owned) { throw 'Unsupported or unrecognized modified VD driver. No files were changed.' }
$original = if ($currentHash -eq [VdPicoEnhance.NativePatch]::OriginalSha256) { $current } else { [IO.File]::ReadAllBytes($backupPath) }
if ([VdPicoEnhance.NativePatch]::Hash($original) -ne [VdPicoEnhance.NativePatch]::OriginalSha256) { throw 'Official backup does not match supported VD 1.34.22.0. No files were changed.' }
$utf8 = [Text.UTF8Encoding]::new($false)
function Write-Atomic([string]$Path, [byte[]]$Bytes) {
    $temp = $Path + '.tmp-' + [Guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllBytes($temp, $Bytes)
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temp, $Path, [NullString]::Value) }
        else { [IO.File]::Move($temp, $Path) }
    } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force } }
}
if ($Action -eq 'Uninstall') {
    if ($currentHash -ne [VdPicoEnhance.NativePatch]::OriginalSha256) { Write-Atomic $DriverPath $original }
    if (Test-Path -LiteralPath $receiptPath) { Remove-Item -LiteralPath $receiptPath -Force }
    Write-Host 'Restored official VD driver. Original backup retained.'
    return
}
$metadata = Get-Content -LiteralPath (Join-Path $PackagePath 'capsule.json') -Raw | ConvertFrom-Json
$capsule = [IO.File]::ReadAllBytes((Join-Path $PackagePath 'vd-picoenhance.pe'))
if ([VdPicoEnhance.NativePatch]::Hash($capsule) -ne $metadata.capsule_sha256 -or $metadata.original_sha256 -ne [VdPicoEnhance.NativePatch]::OriginalSha256) { throw 'PC capsule does not match its package metadata. No files were changed.' }
if ($metadata.kind -ne 'battery-only-v1') { throw 'Expected the battery-only PC package.' }
$patched = [VdPicoEnhance.NativePatch]::Apply($original, $capsule)
$patchedHash = [VdPicoEnhance.NativePatch]::Hash($patched)
if ($patchedHash -eq $currentHash) { Write-Host 'This PC patch is already installed.'; return }
if (Test-Path -LiteralPath $backupPath) {
    if ([VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($backupPath)) -ne [VdPicoEnhance.NativePatch]::OriginalSha256) { throw 'Existing backup is different; it was not overwritten.' }
} else { Write-Atomic $backupPath $original }
# Save both transaction endpoints before replacing the DLL, so interruption
# before or after the replace can be recovered by the same installer.
$record = [ordered]@{ format='vd-picoenhance-driver-v1'; original_sha256=[VdPicoEnhance.NativePatch]::OriginalSha256; previous_sha256=$currentHash; patched_sha256=$patchedHash; capsule_sha256=$metadata.capsule_sha256 }
Write-Atomic $receiptPath ($utf8.GetBytes(($record | ConvertTo-Json)))
Write-Atomic $DriverPath $patched
if ([VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($DriverPath)) -ne $patchedHash) { throw 'Installed driver verification failed. Restore with -Action Uninstall.' }
Write-Host 'Installed PICO controller battery patch. Knuckles switching is a separate optional module.'
