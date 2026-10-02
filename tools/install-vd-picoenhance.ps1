#Requires -Version 5.1
<#
.SYNOPSIS
    One-shot installer for the VD-PicoEnhance Magisk module and its hand mesh.

.DESCRIPTION
    Downloads the module ZIP and the hand mesh from a GitHub release, pushes
    them to a connected headset over adb, installs the Magisk module, places
    the mesh in Virtual Desktop's private directory, and reboots the headset.

    Requirements: a rooted PICO 4 Pro (Magisk with Zygisk), Virtual Desktop
    Android 1.34.22.0 installed, and adb available (on PATH, or via -Adb, or
    under $env:ANDROID_HOME / $env:ANDROID_SDK_ROOT).

.PARAMETER Tag
    Release tag to install, for example v1.0.2. Defaults to the latest release.

.PARAMETER ZipPath
    Use a local Magisk module ZIP instead of downloading one.

.PARAMETER MeshPath
    Use a local hand mesh blob instead of downloading one.

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
    ./install-vd-picoenhance.ps1 -Tag v1.0.2 -Mode 2
#>
[CmdletBinding()]
param(
    [string]$Tag,
    [string]$ZipPath,
    [string]$MeshPath,
    [string]$Adb,
    [string]$Serial,
    [ValidateSet(1, 2)][int]$Mode = 2,
    [switch]$NoReboot
)

$ErrorActionPreference = 'Stop'

$Repo = 'WolalaQAQ/VD-PicoEnhance'
$AppId = 'VirtualDesktop.Android'
$ModuleId = 'vdhs_zygisk'
$AppDir = "/data/data/$AppId"

function Write-Step { param([string]$Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Ok { param([string]$Message) Write-Host "    $Message" -ForegroundColor Green }

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
$devices = @((& $Adb devices) | Select-Object -Skip 1 |
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
$id = ((& $Adb -s $Serial shell 'su -c id') 2>&1 | Out-String)
if ($id -notmatch 'uid=0') {
    throw "Root is required and 'su' did not return uid=0. Enable Magisk and grant shell root."
}
Write-Ok 'root ok'

# --- fetch the module and the mesh -----------------------------------------
$base = if ($Tag) { "https://github.com/$Repo/releases/download/$Tag" } else { "https://github.com/$Repo/releases/latest/download" }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('vd-picoenhance-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

if (-not $ZipPath) {
    $ZipPath = Join-Path $temp 'vdhs_zygisk.zip'
    Write-Step "Downloading the module from $base"
    Invoke-WebRequest -UseBasicParsing -Uri "$base/vdhs_zygisk.zip" -OutFile $ZipPath
}
if (-not (Test-Path -LiteralPath $ZipPath)) { throw "Module ZIP not found: $ZipPath" }

if (-not $MeshPath) {
    $MeshPath = Join-Path $temp 'hand_mesh_fb.bin'
    Write-Step "Downloading the hand mesh from $base"
    Invoke-WebRequest -UseBasicParsing -Uri "$base/hand_mesh_fb.bin" -OutFile $MeshPath
}
if (-not (Test-Path -LiteralPath $MeshPath)) { throw "Hand mesh not found: $MeshPath" }

# --- push -------------------------------------------------------------------
Write-Step 'Pushing files to the headset'
& $Adb -s $Serial push $ZipPath /data/local/tmp/vdhs_zygisk.zip | Out-Null
& $Adb -s $Serial push $MeshPath /data/local/tmp/hand_mesh_fb.bin | Out-Null
Write-Ok 'pushed to /data/local/tmp'

# --- install the Magisk module ---------------------------------------------
Write-Step 'Installing the Magisk module'
$install = ((& $Adb -s $Serial shell "su -c 'magisk --install-module /data/local/tmp/vdhs_zygisk.zip'") 2>&1 | Out-String)
Write-Host ($install.Trim())
if ($install -notmatch 'Done') { throw 'Module installation did not report success.' }

Write-Step "Setting payload mode to $Mode"
& $Adb -s $Serial shell "su -c 'echo $Mode > /data/adb/modules_update/$ModuleId/payload/mode.txt 2>/dev/null; echo $Mode > /data/adb/modules/$ModuleId/payload/mode.txt 2>/dev/null; true'" | Out-Null
Write-Ok "mode = $Mode"

# --- place the hand mesh ----------------------------------------------------
Write-Step 'Placing the hand mesh'
$remote = 'd=' + $AppDir + '; mkdir -p $d/vdhs; cp /data/local/tmp/hand_mesh_fb.bin $d/vdhs/hand_mesh_fb.bin; ' +
          'chown $(stat -c %u:%g $d) $d/vdhs/hand_mesh_fb.bin; chmod 0644 $d/vdhs/hand_mesh_fb.bin; ls -l $d/vdhs/hand_mesh_fb.bin'
& $Adb -s $Serial shell "su -c '$remote'"

# --- reboot -----------------------------------------------------------------
if ($NoReboot) {
    Write-Step 'Skipping the reboot (-NoReboot). The module applies on the next boot.'
} else {
    Write-Step 'Rebooting the headset'
    & $Adb -s $Serial reboot | Out-Null
    Write-Ok 'reboot sent; the module applies during boot'
}

Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host 'After boot: if PICO is in gesture mode, pick up the controller once so' -ForegroundColor Gray
Write-Host 'Virtual Desktop can cold-start, then start it and check that hands work.' -ForegroundColor Gray
