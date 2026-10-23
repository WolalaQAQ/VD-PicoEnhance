# Build the optional provider, owned fixtures and Magisk ZIP; no runtime access.
param([string]$Zig, [string]$OpenVRRoot, [string]$Python, [string]$SdkRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Python) { $Python = (Get-Command python -ErrorAction Stop).Source }
if (-not $Zig) { $Zig = (Get-Command zig -ErrorAction Stop).Source }
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_SDK_ROOT }
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_HOME }
if (-not $SdkRoot) { $SdkRoot = (Get-Content -LiteralPath (Join-Path $repo 'tools/sdk-path.txt') -Raw).Trim() }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = (Get-Content -LiteralPath (Join-Path $repo 'tools/jdk-path.txt') -Raw).Trim() }
if (-not $OpenVRRoot) {
    $OpenVRRoot = Join-Path $PSScriptRoot 'pc/build/openvr'
    $headers = Join-Path $OpenVRRoot 'headers'
    New-Item -ItemType Directory -Force -Path $headers | Out-Null
    $commit = '91825305130f446f82054c1ec3d416321ace0072'
    foreach ($file in @('headers/openvr_driver.h','LICENSE')) {
        $target = Join-Path $OpenVRRoot $file
        if (-not (Test-Path -LiteralPath $target)) {
            Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/ValveSoftware/openvr/$commit/$file" -OutFile $target
        }
    }
}
$header = Join-Path $OpenVRRoot 'headers/openvr_driver.h'
# Pin SDK content across GitHub's LF download and Windows CRLF checkouts.
$headerBytes = [Text.Encoding]::UTF8.GetBytes(([Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($header))).Replace("`r`n","`n").Replace("`n","`r`n"))
$sha = [Security.Cryptography.SHA256]::Create()
try { $actualHeaderSha = [BitConverter]::ToString($sha.ComputeHash($headerBytes)).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
if ($actualHeaderSha -ne 'e9dc0565e31305023d43a4110ee0bf6e4916e05cb592b3f58e01c80dd41e52aa') { throw 'OpenVR SDK header does not match the pinned version.' }
$env:ZIG = (Resolve-Path -LiteralPath $Zig).Path
$env:OPENVR_ROOT = (Resolve-Path -LiteralPath $OpenVRRoot).Path
$env:ANDROID_SDK_ROOT = $SdkRoot
& $Python -B -X utf8 (Join-Path $PSScriptRoot 'pc/build.py')
if ($LASTEXITCODE) { throw 'Optional PC provider build or owned tests failed.' }
& $Python -B -X utf8 (Join-Path $PSScriptRoot 'headset/build.py')
if ($LASTEXITCODE) { throw 'Optional headset build failed.' }
& $Python -B -X utf8 (Join-Path $PSScriptRoot 'headset/check.py')
if ($LASTEXITCODE) { throw 'Optional headset owned checks failed.' }
& $Python -B -X utf8 (Join-Path $PSScriptRoot 'capture/check.py')
if ($LASTEXITCODE) { throw 'Optional input capture owned checks failed.' }
$latest = Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'pc/build') -Directory |
    Where-Object { $_.Name -match '^\d{8}-\d{6}$' } | Sort-Object Name -Descending | Select-Object -First 1
$pc = Join-Path $PSScriptRoot 'dist/pc'
New-Item -ItemType Directory -Force -Path $pc | Out-Null
Copy-Item -LiteralPath (Join-Path $latest.FullName 'driver_picomixedvr') -Destination $pc -Recurse -Force
foreach ($file in @('control.ps1')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "pc/$file") -Destination $pc -Force }
foreach ($file in @('manage.ps1','manager-core.ps1')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $PSScriptRoot 'dist') -Force }
Write-Host 'Built optional PicoHand<->Knuckles package and owned provider tests.'
