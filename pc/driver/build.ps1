# Build our portable native capsule; no installed VD file or device is accessed.
param([string]$SdkRoot, [string]$OpenVRHeader, [string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_SDK_ROOT }
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_HOME }
if (-not $SdkRoot) { $SdkRoot = (Get-Content -LiteralPath (Join-Path $root 'tools/sdk-path.txt') -Raw).Trim() }
$bin = Join-Path $SdkRoot 'ndk/26.1.10909125/toolchains/llvm/prebuilt/windows-x86_64/bin'
if (-not $OutputPath) { $OutputPath = Join-Path $PSScriptRoot 'build' }
$out = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path $out | Out-Null
$headerSha = 'e9dc0565e31305023d43a4110ee0bf6e4916e05cb592b3f58e01c80dd41e52aa'
if (-not $OpenVRHeader) {
    $OpenVRHeader = Join-Path $out 'openvr_driver.h'
    if (-not (Test-Path -LiteralPath $OpenVRHeader)) {
        Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/ValveSoftware/openvr/91825305130f446f82054c1ec3d416321ace0072/headers/openvr_driver.h' -OutFile $OpenVRHeader
    }
}
if ((Get-FileHash -LiteralPath $OpenVRHeader -Algorithm SHA256).Hash.ToLowerInvariant() -ne $headerSha) { throw 'OpenVR header must match SDK v2.12.14.' }
if ([IO.Path]::GetFullPath($OpenVRHeader) -ne (Join-Path $out 'openvr_driver.h')) { Copy-Item -LiteralPath $OpenVRHeader -Destination (Join-Path $out 'openvr_driver.h') -Force }
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $out 'string'), '#pragma once' + "`nnamespace std { class string { public: string(); string(const string &); ~string(); string &operator=(const char *); }; }`n", $utf8)
[IO.File]::WriteAllText((Join-Path $out 'vector'), '#pragma once' + "`nnamespace std { template<class T> class vector; }`n", $utf8)
[IO.File]::WriteAllText((Join-Path $out 'runtime_table.c'), '__declspec(dllexport) const unsigned char vd_native_runtime_table[32768] = {0};' + "`n", $utf8)
if (-not ('VdPicoEnhance.NativePatch' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'NativePatch.cs') }
$sources = @('native_battery.c','native_frame.c','native_entry.c')
$sourceRoot = Join-Path $PSScriptRoot 'src'
$capsule = Join-Path $out 'vd-picoenhance.pe'
function Run-Compiler([string]$Tool, [string[]]$Arguments) {
    & (Join-Path $bin $Tool) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed." }
}
$entry = [VdPicoEnhance.NativePatch]::CodeRva
for ($pass = 0; $pass -lt 2; ++$pass) {
    $objects = @()
    foreach ($name in @($sources) + 'runtime_table.c') {
        $source = if ($name -eq 'runtime_table.c') { Join-Path $out $name } else { Join-Path $sourceRoot $name }
        $object = Join-Path $out ($name + '.obj')
        $flags = @('--target=x86_64-pc-windows-msvc','-O2','-Wall','-Wextra','-Werror','-Wframe-larger-than=4096','-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-stack-arg-probe','-funwind-tables',"-DVD_ENTRY_RVA=$entry",'-I',$sourceRoot,'-isystem',$out)
        if ($name.EndsWith('.cpp')) { $flags += @('-std=c++17','-fno-exceptions','-fno-rtti','-DOPENVR_NO_STL','-DOPENVR_INTERFACE_INTERNAL') }
        else { $flags += '-std=c11' }
        Run-Compiler 'clang.exe' ($flags + @('-c',$source,'-o',$object))
        $objects += $object
    }
    $linkBase = '0x{0:x}' -f ([VdPicoEnhance.NativePatch]::ImageBase + [VdPicoEnhance.NativePatch]::Delta)
    Run-Compiler 'ld.lld.exe' (@('-flavor','link','/dll','/noentry','/nodefaultlib','/wx','/machine:x64','/opt:noref','/merge:.rdata=.text','/merge:.pdata=.text','/timestamp:0',"/base:$linkBase","/out:$capsule") + $objects)
    $exports = [VdPicoEnhance.NativePatch]::Exports([IO.File]::ReadAllBytes($capsule))
    $actual = $exports['vd_native_run_frame'] + [VdPicoEnhance.NativePatch]::Delta
    if ($actual -eq $entry) { break }
    if ($pass -eq 1) { throw 'Capsule entry layout did not stabilize.' }
    $entry = $actual
}
$metadata = [ordered]@{
    format = 'vd-picoenhance-native-v1'
    kind = 'battery-only-v1'
    vd_version = '1.34.22.0'
    original_sha256 = [VdPicoEnhance.NativePatch]::OriginalSha256
    capsule_sha256 = [VdPicoEnhance.NativePatch]::Hash([IO.File]::ReadAllBytes($capsule))
    openvr_commit = '91825305130f446f82054c1ec3d416321ace0072'
    openvr_header_sha256 = $headerSha
    source_sha256 = [ordered]@{}
}
foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -File | Sort-Object Name) { $metadata.source_sha256[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
[IO.File]::WriteAllText((Join-Path $out 'capsule.json'), ($metadata | ConvertTo-Json -Depth 5), $utf8)
foreach ($name in @('NativePatch.cs','install.ps1','OPENVR-LICENSE.txt')) {
    $file = Join-Path $PSScriptRoot $name
    if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination $out -Force }
}
Write-Host "Built source-only native capsule: $capsule"
