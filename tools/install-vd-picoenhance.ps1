#Requires -Version 5.1
<#
.SYNOPSIS
    One-shot installer for the VD-PicoEnhance Magisk module and its hand mesh.

.DESCRIPTION
    Uses the module ZIP and hand mesh beside this script, pushes them to a
    connected headset over adb, installs the Magisk module, places the mesh
    in Virtual Desktop's private directory, and reboots the headset.

    Download install-vd-picoenhance.ps1, vdhs_zygisk.zip and hand_mesh_fb.bin
    from the same release and keep them in one directory. This script never
    downloads files; missing files cause an error before any device changes.

    Requirements: a rooted PICO 4 Pro (Magisk with Zygisk), Virtual Desktop
    Android 1.34.22.0 installed, and adb available (on PATH, or via -Adb, or
    under $env:ANDROID_HOME / $env:ANDROID_SDK_ROOT).

.PARAMETER ZipPath
    Local Magisk module ZIP. Defaults to vdhs_zygisk.zip beside this script.

.PARAMETER MeshPath
    Local hand mesh blob. Defaults to hand_mesh_fb.bin beside this script.

.PARAMETER Adb
    Path to adb.exe. Defaults to adb on PATH, then the SDK platform-tools.

.PARAMETER Serial
    Target device serial, needed only when several devices are connected.

.PARAMETER Mode
    Payload mode written to the installed module: 2 = full layer (default),
    1 = injection smoke test.

.PARAMETER NoReboot
    Do not reboot the headset when finished.

.EXAMPLE
    ./install-vd-picoenhance.ps1

.EXAMPLE
    ./install-vd-picoenhance.ps1 -ZipPath ./vdhs_zygisk.zip -MeshPath ./hand_mesh_fb.bin -NoReboot
#>
[CmdletBinding()]
param(
    [string]$ZipPath,
    [string]$MeshPath,
    [string]$Adb,
    [string]$Serial,
    [ValidateSet(1, 2)][int]$Mode = 2,
    [switch]$NoReboot
)

$ErrorActionPreference = 'Stop'

$AppId = 'VirtualDesktop.Android'
$ModuleId = 'vdhs_zygisk'
$AppDir = "/data/data/$AppId"

function Write-Step { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Ok { param([string]$Message) Write-Host "    $Message" -ForegroundColor Green }

function Invoke-Adb {
    param([string[]]$Arguments)
    # Windows PowerShell 5.1 does not turn native exit codes into exceptions.
    $output = & $Adb @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed (exit code $LASTEXITCODE): $($Arguments -join ' ')`n$($output -join [Environment]::NewLine)"
    }
    $output
}

# --- locate the local module and mesh ---------------------------------------
Write-Step 'Looking for the local module and hand mesh'
if (-not $ZipPath) { $ZipPath = Join-Path $PSScriptRoot 'vdhs_zygisk.zip' }
if (-not $MeshPath) { $MeshPath = Join-Path $PSScriptRoot 'hand_mesh_fb.bin' }
if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) {
    throw "Module ZIP not found: $ZipPath. Keep vdhs_zygisk.zip from the same release beside this script, or pass -ZipPath. No files will be downloaded."
}
if (-not (Test-Path -LiteralPath $MeshPath -PathType Leaf)) {
    throw "Hand mesh not found: $MeshPath. Keep hand_mesh_fb.bin from the same release beside this script, or pass -MeshPath. No files will be downloaded."
}
$ZipPath = (Resolve-Path -LiteralPath $ZipPath).ProviderPath
$MeshPath = (Resolve-Path -LiteralPath $MeshPath).ProviderPath
Write-Ok "module: $ZipPath"
Write-Ok "mesh: $MeshPath"

# --- locate adb -------------------------------------------------------------
if (-not $Adb) {
    $cmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($cmd) { $Adb = $cmd.Source }
    else {
        foreach ($sdk in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT)) {
            if ($sdk) {
                $candidate = Join-Path $sdk 'platform-tools\adb.exe'
                if (Test-Path -LiteralPath $candidate) { $Adb = $candidate; break }
            }
        }
    }
}
if (-not $Adb -or -not (Test-Path -LiteralPath $Adb)) {
    throw 'adb not found. Install Android platform-tools, put adb on PATH, or pass -Adb <path>.'
}
Write-Ok "adb: $Adb"

# --- find the headset -------------------------------------------------------
Write-Step 'Looking for a headset'
$deviceList = Invoke-Adb -Arguments @('devices')
$devices = @($deviceList | Select-Object -Skip 1 |
    Where-Object { ($_ -split '\s+')[1] -eq 'device' } |
    ForEach-Object { ($_ -split '\s+')[0] })
if ($Serial) {
    if ($devices -notcontains $Serial) { throw "Device $Serial is not connected in 'device' state." }
} else {
    if ($devices.Count -eq 0) { throw 'No headset in "device" state. Connect it over USB, enable USB debugging and authorize this PC.' }
    if ($devices.Count -gt 1) { throw "Several devices are connected ($($devices -join ', ')). Re-run with -Serial <serial>." }
    $Serial = $devices[0]
}
Write-Ok "device: $Serial"

# --- root -------------------------------------------------------------------
Write-Step 'Checking root'
$id = Invoke-Adb -Arguments @('-s', $Serial, 'shell', 'su -c id') | Out-String
if ($id -notmatch 'uid=0') {
    throw "Root is required and 'su' did not return uid=0. Enable Magisk and grant shell root."
}
Write-Ok 'root ok'

# --- push -------------------------------------------------------------------
Write-Step 'Pushing files to the headset'
Invoke-Adb -Arguments @('-s', $Serial, 'push', $ZipPath, '/data/local/tmp/vdhs_zygisk.zip') | Out-Null
Invoke-Adb -Arguments @('-s', $Serial, 'push', $MeshPath, '/data/local/tmp/hand_mesh_fb.bin') | Out-Null
Write-Ok 'pushed to /data/local/tmp'

# --- install the Magisk module ---------------------------------------------
Write-Step 'Installing the Magisk module'
$install = Invoke-Adb -Arguments @('-s', $Serial, 'shell', "su -c 'magisk --install-module /data/local/tmp/vdhs_zygisk.zip'") | Out-String
Write-Host ($install.Trim())
if ($install -notmatch 'Done') { throw 'Module installation did not report success.' }

Write-Step "Setting payload mode to $Mode"
$remote = 'set -e; found=0; for d in /data/adb/modules_update/' + $ModuleId + ' /data/adb/modules/' + $ModuleId + '; do ' +
          'if [ -d $d ]; then echo ' + $Mode + ' > $d/payload/mode.txt; found=1; fi; done; ' +
          'if [ $found -ne 1 ]; then echo Module-directory-not-found >&2; exit 1; fi'
Invoke-Adb -Arguments @('-s', $Serial, 'shell', "su -c '$remote'") | Out-Null
Write-Ok "mode = $Mode"

# --- place the hand mesh ----------------------------------------------------
Write-Step 'Placing the hand mesh'
# -D includes app data when restoring Android's SELinux labels.
$remote = 'set -e; d=' + $AppDir + '; owner=$(stat -c %u:%g $d); mkdir -p $d/vdhs; ' +
          'chown $owner $d/vdhs; chmod 0700 $d/vdhs; ' +
          'cp /data/local/tmp/hand_mesh_fb.bin $d/vdhs/hand_mesh_fb.bin; ' +
          'chown $owner $d/vdhs/hand_mesh_fb.bin; chmod 0644 $d/vdhs/hand_mesh_fb.bin; ' +
          'restorecon -RFD $d/vdhs; ls -l $d/vdhs/hand_mesh_fb.bin'
Invoke-Adb -Arguments @('-s', $Serial, 'shell', "su -c '$remote'")

# --- reboot -----------------------------------------------------------------
if ($NoReboot) {
    Write-Step 'Skipping the reboot (-NoReboot). The module applies on the next boot.'
} else {
    Write-Step 'Rebooting the headset'
    Invoke-Adb -Arguments @('-s', $Serial, 'reboot') | Out-Null
    Write-Ok 'reboot sent; the module applies during boot'
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host 'After boot: if PICO is in gesture mode, pick up the controller once so' -ForegroundColor Gray
Write-Host 'Virtual Desktop can cold-start, then start it and check that hands work.' -ForegroundColor Gray
