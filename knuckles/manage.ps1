#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Install','Configure','Enable','Disable','Status','Uninstall')][string]$Action = 'Status',
    [string]$Adb,
    [string]$Serial,
    [string]$HeadsetAddress,
    [string]$PcAddress,
    [string]$VrPathReg
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'manager-core.ps1')
$options = @{
    Config=(Join-Path $env:LOCALAPPDATA 'PicoMixedVR'); Package=$PSScriptRoot
    Registry=(Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath')
    Adb=$Adb; Serial=$Serial; HeadsetAddress=$HeadsetAddress; PcAddress=$PcAddress; VrPathReg=$VrPathReg
}
if ($Action -ne 'Status') {
    if (-not $options.Adb) { $options.Adb = (Get-Command adb -ErrorAction Stop).Source }
    if (-not $options.Serial) {
        $devices = & $options.Adb devices
        if ($LASTEXITCODE) { throw 'ADB device inventory failed.' }
        $connected = @($devices | Where-Object { $_ -match '^\S+\s+device\s*$' } | ForEach-Object { ($_ -split '\s+')[0] })
        if ($connected.Count -ne 1) { throw 'Connect one authorized headset or pass -Serial.' }
        $options.Serial = $connected[0]
    }
}
Invoke-Knuckles $options $Action
