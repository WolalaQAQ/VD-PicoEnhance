# Build VD-PicoEnhance and stage the Magisk/Zygisk module tree.
#
#   pwsh -File zygisk/build.ps1                 # offline-safe default build
#   pwsh -File zygisk/build.ps1 -Harmony        # also build the Harmony backend, stage 0Harmony.dll
#   pwsh -File zygisk/build.ps1 -Redirect <dll> # also stage Xenko.OpenXR.patched.dll
#   pwsh -File zygisk/build.ps1 -Pack           # build, then pack dist/vdhs_zygisk.zip
#
# Outputs (the module directory is zygisk/ itself; the zygisk/ subdirectory is the
# Magisk ABI directory):
#   zygisk/zygisk/arm64-v8a.so          Zygisk injector
#   zygisk/payload/libvdhs.so           shared native payload
#   zygisk/payload/VdHsMod.dll          managed mod
#   zygisk/payload/backend.txt          backend selection
#   zygisk/payload/0Harmony.dll         (optional)
#   zygisk/payload/Xenko.OpenXR.patched.dll (optional, redirect backend)
#
# This script only writes files inside the repository and never touches a device.
param(
    [switch]$Harmony,
    [string]$RedirectArtifact = "",
    [switch]$Pack,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$ModuleRoot = Split-Path -Parent $MyInvocation.MyCommand.Path   # .../VD-PicoEnhance/zygisk
$RepoRoot   = Split-Path -Parent $ModuleRoot                    # .../VD-PicoEnhance

# Toolchain paths are resolved at build time so nothing machine-specific is
# committed. Order: ANDROID_SDK_ROOT / ANDROID_HOME, then the untracked local
# file tools/sdk-path.txt. The NDK/CMake versions are the ones this project was
# built and tested with.
$SdkRoot = $env:ANDROID_SDK_ROOT
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_HOME }
if (-not $SdkRoot) {
    $sdkFile = Join-Path $RepoRoot 'tools\sdk-path.txt'
    if (Test-Path $sdkFile) { $SdkRoot = (Get-Content $sdkFile -Raw).Trim() }
}
if (-not $SdkRoot) { throw 'set ANDROID_SDK_ROOT (or ANDROID_HOME), or write the SDK path to tools/sdk-path.txt' }

$Ndk     = Join-Path $SdkRoot 'ndk\26.1.10909125'
$Cmake   = Join-Path $SdkRoot 'cmake\3.22.1\bin\cmake.exe'
$Ninja   = Join-Path $SdkRoot 'cmake\3.22.1\bin\ninja.exe'
$Dotnet  = if ($env:DOTNET_ROOT) { Join-Path $env:DOTNET_ROOT 'dotnet.exe' } elseif ($cmd = Get-Command dotnet -ErrorAction SilentlyContinue) { $cmd.Source } else { 'dotnet' }
$Toolchain = Join-Path $Ndk 'build\cmake\android.toolchain.cmake'

foreach ($p in @($Ndk, $Cmake, $Ninja, $Dotnet, $Toolchain)) {
    if (-not (Test-Path $p)) { throw "missing tool: $p" }
}

$ReleaseDir  = Join-Path $RepoRoot 'mod\src\VdHsMod\bin\Release'
$VdhsProj    = Join-Path $RepoRoot 'mod\src\VdHsMod\VdHsMod.csproj'
$NativeSrc   = Join-Path $RepoRoot 'mod\native'
$NativeBuild = Join-Path $NativeSrc 'build-arm64'
$ModSrc      = Join-Path $ModuleRoot 'jni'
$ModBuild    = Join-Path $ModSrc 'build-arm64'
$OutZygisk   = Join-Path $ModuleRoot 'zygisk'
$OutPayload  = Join-Path $ModuleRoot 'payload'

function Build-CMake([string]$src, [string]$build) {
    if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
    & $Cmake -S $src -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" -DANDROID_ABI=arm64-v8a `
        -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $src" }
    & $Cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $src" }
}

Write-Host '== 1/4 VdHsMod.dll =='
$harmonyArg = if ($Harmony) { '-p:VdHsHarmony=true' } else { '-p:VdHsHarmony=false' }
& $Dotnet build $VdhsProj -c Release $harmonyArg -v q
if ($LASTEXITCODE -ne 0) { throw 'VdHsMod build failed' }
if ($Harmony) { & $Dotnet build $VdhsProj -c Release $harmonyArg -v q --no-incremental | Out-Null }

Write-Host '== 2/4 libvdhs.so (native payload) =='
Build-CMake $NativeSrc $NativeBuild

Write-Host '== 3/4 arm64-v8a.so (Zygisk module) =='
Build-CMake $ModSrc $ModBuild

Write-Host '== 4/4 stage module tree =='
New-Item -ItemType Directory -Force -Path $OutZygisk, $OutPayload | Out-Null

Copy-Item (Join-Path $ModBuild 'arm64-v8a.so')    (Join-Path $OutZygisk 'arm64-v8a.so') -Force
Copy-Item (Join-Path $NativeBuild 'libvdhs.so')   (Join-Path $OutPayload 'libvdhs.so') -Force
Copy-Item (Join-Path $ReleaseDir 'VdHsMod.dll')   (Join-Path $OutPayload 'VdHsMod.dll') -Force

if ($Harmony) {
    $net35 = Join-Path $env:USERPROFILE '.nuget\packages\lib.harmony\2.3.5\lib\net35\0Harmony.dll'
    if (-not (Test-Path $net35)) { throw "Harmony package not restored: $net35 (run the -Harmony build once online)" }
    Copy-Item $net35 (Join-Path $OutPayload '0Harmony.dll') -Force
    Write-Host "  staged 0Harmony.dll (net35)"
} else {
    Remove-Item (Join-Path $OutPayload '0Harmony.dll') -ErrorAction SilentlyContinue
}

if ($RedirectArtifact) {
    if (-not (Test-Path $RedirectArtifact)) { throw "redirect artifact not found: $RedirectArtifact" }
    Copy-Item $RedirectArtifact (Join-Path $OutPayload 'Xenko.OpenXR.patched.dll') -Force
    Write-Host "  staged Xenko.OpenXR.patched.dll"
} else {
    $redirect = Join-Path $OutPayload 'Xenko.OpenXR.patched.dll'
    if (Test-Path -LiteralPath $redirect) { Remove-Item -LiteralPath $redirect -Force }
}

Write-Host 'staged:'
Get-ChildItem -Recurse $OutZygisk, $OutPayload -File |
    ForEach-Object { '{0,10}  {1}' -f $_.Length, ($_.FullName.Substring($ModuleRoot.Length + 1)) }

if ($Pack) {
    $dist = Join-Path $ModuleRoot 'dist'
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $zip = Join-Path $dist 'vdhs_zygisk.zip'
    Remove-Item $zip -ErrorAction SilentlyContinue
    # The Magisk zip root holds module.prop, the scripts, zygisk/ and payload/.
    $stage = Join-Path $dist 'stage'
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    foreach ($f in 'module.prop', 'customize.sh', 'post-fs-data.sh', 'service.sh') {
        Copy-Item (Join-Path $ModuleRoot $f) $stage -Force
    }
    Copy-Item $OutZygisk (Join-Path $stage 'zygisk') -Recurse -Force
    Copy-Item $OutPayload (Join-Path $stage 'payload') -Recurse -Force
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    Remove-Item -Recurse -Force $stage
    Write-Host "packed: $zip"
}
