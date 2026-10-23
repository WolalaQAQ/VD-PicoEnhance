#Requires -Version 5.1
<#
.SYNOPSIS
    Install or manage VD-PicoEnhance on the headset and Windows PC.

.DESCRIPTION
    Uses the module ZIP and hand mesh beside this script, pushes them to a
    connected headset over adb, installs the Magisk module, places the mesh
    in Virtual Desktop's private directory, and reboots the headset.

    Download and extract VD-PicoEnhance-v<version>.zip from the release.
    Keep this script, vdhs_zygisk.zip and hand_mesh_fb.bin together. This script
    never downloads files; missing files cause an error before any device changes.

    Installs the local PC patch, headset module and mesh before reboot.
    The base PC patch supplies battery properties only. -WithKnuckles also
    installs the independent optional provider and LAN daemon. Saved choices remain.

.PARAMETER Action
    Install (default), Configure connection, Enable or Disable switching, Status,
    or Uninstall. ADB is used for installation and persistent headset management.

.PARAMETER DriverPath
    Complete path to the VD Streamer driver when not in the standard location.

.PARAMETER HeadsetOnly
    Advanced headset-only installation, for isolated module verification.

.PARAMETER ZipPath
    Local Magisk module ZIP. Defaults to vdhs_zygisk.zip beside this script.

.PARAMETER MeshPath
    Local hand mesh blob. Defaults to hand_mesh_fb.bin beside this script.
    An existing mesh is preserved unless this parameter is explicitly supplied.

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
    [ValidateSet('Install','Configure','Enable','Disable','Status','Uninstall','UninstallKnuckles')][string]$Action = 'Install',
    [string]$ZipPath,
    [string]$MeshPath,
    [string]$Adb,
    [string]$Serial,
    [string]$DriverPath = (Join-Path $env:ProgramFiles 'Virtual Desktop Streamer/OpenVRDriver/bin/win64/driver_VirtualDesktop.dll'),
    [ValidateSet(1, 2)][int]$Mode = 2,
    [switch]$NoReboot,
    [switch]$HeadsetOnly,
    [switch]$WithKnuckles,
    [string]$HeadsetAddress,
    [string]$PcAddress
)

$ErrorActionPreference = 'Stop'

$AppId = 'VirtualDesktop.Android'
$ModuleId = 'vdhs_zygisk'
$AppDir = "/data/data/$AppId"
$ReplaceMesh = if ($PSBoundParameters.ContainsKey('MeshPath')) { 1 } else { 0 }
$pcPackage = Join-Path $PSScriptRoot 'pc'
$pcInstaller = Join-Path $pcPackage 'install.ps1'
$pcManager = Join-Path $PSScriptRoot 'knuckles/manage.ps1'
$installPC = -not $HeadsetOnly -and $Mode -eq 2
$optionalReceipt = Join-Path $env:LOCALAPPDATA 'PicoMixedVR/installation.json'
$hasKnuckles = (Test-Path -LiteralPath $optionalReceipt) -and (Get-Content -LiteralPath $optionalReceipt -Raw | ConvertFrom-Json).installed
$installKnuckles = $installPC -and ($WithKnuckles -or $hasKnuckles)
if ($WithKnuckles -and -not $installPC) { throw '-WithKnuckles requires a full PC and headset installation.' }

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

function Invoke-PC([string]$Operation) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        & $pcInstaller -Action $Operation -DriverPath $DriverPath -PackagePath $pcPackage
    } else {
        $hostPath = (Get-Command powershell.exe).Source
        $arguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$pcInstaller+'"'),'-Action',$Operation,'-DriverPath',('"'+$DriverPath+'"'),'-PackagePath',('"'+$pcPackage+'"'))
        $process = Start-Process -FilePath $hostPath -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "PC $Operation failed (exit code $($process.ExitCode))." }
    }
}

if ($Action -eq 'Status') {
    & $pcManager -Action Status
    if (Test-Path -LiteralPath $DriverPath) { & $pcInstaller -Action Status -DriverPath $DriverPath }
    return
}
if ($Action -eq 'Disable' -or $Action -eq 'Enable') {
    & $pcManager -Action $Action -Adb $Adb -Serial $Serial
    return
}
if ($Action -eq 'Configure') {
    & $pcManager -Action Configure -Adb $Adb -Serial $Serial -HeadsetAddress $HeadsetAddress -PcAddress $PcAddress
    return
}
if ($Action -eq 'UninstallKnuckles') {
    & $pcManager -Action Uninstall -Adb $Adb -Serial $Serial
    return
}
if ($installPC) {
    foreach ($file in @($pcInstaller,(Join-Path $pcPackage 'vd-picoenhance.pe'),(Join-Path $pcPackage 'capsule.json'))) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'PC payload missing. Extract the complete release bundle before installing.' }
    }
    if (Get-Process -Name vrserver -ErrorAction SilentlyContinue) { throw 'Exit SteamVR before installing or uninstalling. No files were changed.' }
    $status = & $pcInstaller -Action Status -DriverPath $DriverPath
    if (-not $status.official -and -not $status.owned_patch) { throw 'Unsupported VD Streamer driver. No files were changed.' }
}
if ($installKnuckles -and $Action -eq 'Install') {
    foreach ($file in @($pcManager,(Join-Path $PSScriptRoot 'knuckles/PicoHand-Knuckles.zip'),(Join-Path $PSScriptRoot 'knuckles/pc/driver_picomixedvr/bin/win64/driver_picomixedvr.dll'))) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'Optional Knuckles package missing. No files were changed.' }
    }
}

# --- locate the local module and mesh ---------------------------------------
if ($Action -eq 'Install') {
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
}

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
$Adb = (Resolve-Path -LiteralPath $Adb).ProviderPath
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

if ($Action -eq 'Uninstall') {
    if ($hasKnuckles -and -not $HeadsetOnly) { & $pcManager -Action Uninstall -Adb $Adb -Serial $Serial }
    if ($installPC) { Invoke-PC 'Uninstall' }
    Invoke-Adb -Arguments @('-s', $Serial, 'shell', "su -c 'touch /data/adb/modules/vdhs_zygisk/remove'") | Out-Null
    if (-not $NoReboot) { Invoke-Adb -Arguments @('-s', $Serial, 'reboot') | Out-Null }
    Write-Ok 'PC original restored and headset module marked for removal on reboot. Pairing and original backup retained.'
    return
}
if ($installPC) {
    Write-Step 'Installing the VD PC patch (approve the Windows permission prompt if shown)'
    Invoke-PC 'Install'
}

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
          'if [ ! -e $d/vdhs/hand_mesh_fb.bin ] || [ ' + $ReplaceMesh + ' -eq 1 ]; then ' +
          'cp /data/local/tmp/hand_mesh_fb.bin $d/vdhs/hand_mesh_fb.bin; fi; ' +
          'chown $owner $d/vdhs/hand_mesh_fb.bin; chmod 0644 $d/vdhs/hand_mesh_fb.bin; ' +
          'restorecon -RFD $d/vdhs; ls -l $d/vdhs/hand_mesh_fb.bin'
Invoke-Adb -Arguments @('-s', $Serial, 'shell', "su -c '$remote'")

if ($installKnuckles) {
    Write-Step 'Installing optional PicoHand<->Knuckles and its private LAN pairing'
    & $pcManager -Action Install -Adb $Adb -Serial $Serial -HeadsetAddress $HeadsetAddress -PcAddress $PcAddress
}

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
Write-Host 'After boot: on supported PICO firmware, the module declares VD hand-mode startup support.' -ForegroundColor Gray
if ($installPC) {
    Write-Host 'PICO controller battery support is installed.' -ForegroundColor Gray
    if ($installKnuckles) {
        Write-Host 'PicoHand<->Knuckles is selected; SteamVR loads its independent provider automatically.' -ForegroundColor Gray
        Write-Host 'Daily switching uses the paired LAN connection, not ADB or VD frames. Existing Stop/Disable choices are retained.' -ForegroundColor Gray
    } else {
        Write-Host 'PicoHand<->Knuckles was not selected; no optional provider or LAN daemon was installed.' -ForegroundColor Gray
    }
} else {
    Write-Host 'Headset-only installation complete; PC files and pairing were not changed.' -ForegroundColor Gray
}
