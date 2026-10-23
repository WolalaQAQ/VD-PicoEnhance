# Assemble only explicit distributable inputs, never a patched/original VD DLL.
param(
    [string]$OutputPath = 'artifacts/release',
    [string]$ModuleZip,
    [string]$PcPackagePath,
    [string]$KnucklesPackagePath
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $ModuleZip) { $ModuleZip = Join-Path $root 'zygisk/dist/vdhs_zygisk.zip' }
if (-not $PcPackagePath) { $PcPackagePath = Join-Path $root 'pc/driver/build' }
if (-not $KnucklesPackagePath) { $KnucklesPackagePath = Join-Path $root 'knuckles/dist' }
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) { throw 'Release output directory must be empty.' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
$props = Get-Content -LiteralPath (Join-Path $root 'zygisk/module.prop') -Raw
$version = [regex]::Match($props,'(?m)^version=(.+)$').Groups[1].Value.Trim()
if (-not $version) { throw 'Missing module version.' }
$files = @{
    'vdhs_zygisk.zip'='zygisk/dist/vdhs_zygisk.zip'
    'hand_mesh_fb.bin'='assets/mesh/hand_mesh_fb.bin'
    'install-vd-picoenhance.ps1'='tools/install-vd-picoenhance.ps1'
    'LICENSE'='LICENSE'
    'THIRD_PARTY_NOTICES.md'='THIRD_PARTY_NOTICES.md'
    'README.md'='README.md'
    'README_zh.md'='README_zh.md'
    'CHANGELOG.md'='CHANGELOG.md'
    'CHANGELOG_zh.md'='CHANGELOG_zh.md'
    'config/hand_gesture.example.txt'='config/hand_gesture.example.txt'
    'assets/mesh/README.md'='assets/mesh/README.md'
    'docs/ARCHITECTURE.md'='docs/ARCHITECTURE.md'
    'docs/PC-SETUP.md'='docs/PC-SETUP.md'
    'docs/PC-SETUP_zh.md'='docs/PC-SETUP_zh.md'
}
foreach ($name in @('vd-picoenhance.pe','capsule.json','NativePatch.cs','install.ps1','OPENVR-LICENSE.txt')) { $files['pc/'+$name] = 'pc/driver/build/'+$name }
foreach ($name in @('manage.ps1','manager-core.ps1','PicoHand-Knuckles.zip','pc/control.ps1','pc/driver_picomixedvr/driver.vrdrivermanifest','pc/driver_picomixedvr/OPENVR-LICENSE.txt','pc/driver_picomixedvr/ZIG-RUNTIME-NOTICES.txt','pc/driver_picomixedvr/bin/win64/driver_picomixedvr.dll')) {
    $files['knuckles/'+$name] = 'knuckles/dist/'+$name
}
foreach ($entry in $files.GetEnumerator()) {
    $source = if ($entry.Key -eq 'vdhs_zygisk.zip') { $ModuleZip }
              elseif ($entry.Key.StartsWith('pc/')) { Join-Path $PcPackagePath $entry.Key.Substring(3) }
              elseif ($entry.Key.StartsWith('knuckles/')) { Join-Path $KnucklesPackagePath $entry.Key.Substring(9) }
              else { Join-Path $root $entry.Value }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing release input: $($entry.Value)" }
    $target = Join-Path $output $entry.Key
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}
$sections = foreach ($language in @('', '_zh')) {
    $label = if ($language) { 'Chinese (Simplified)' } else { 'English' }
    $changelog = (Get-Content -LiteralPath (Join-Path $root "CHANGELOG$language.md") -Raw -Encoding UTF8).Replace("`r`n","`n")
    $match = [regex]::Match($changelog,'(?ms)^## '+[regex]::Escape($version)+' [^\n]*\n(.*?)(?=^## |\z)')
    if (-not $match.Success) { throw "Missing changelog section for $version in CHANGELOG$language.md" }
    "## $label`n`n$($match.Groups[1].Value.Trim())"
}
$utf8=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $output 'RELEASE_NOTES.md'),(($sections -join "`n`n")+"`n"),$utf8)
$paths = @(Get-ChildItem -LiteralPath $output -Recurse -File | ForEach-Object { $_.FullName.Substring($output.Length+1).Replace('\','/') } | Sort-Object)
$checksums = (($paths | ForEach-Object { "$((Get-FileHash -LiteralPath (Join-Path $output $_) -Algorithm SHA256).Hash.ToLowerInvariant())  $_" }) -join "`n")+"`n"
[IO.File]::WriteAllText((Join-Path $output 'SHA256SUMS.txt'),$checksums,$utf8)
$bundle='VD-PicoEnhance-v'+$version+'.zip'
& (Join-Path $PSScriptRoot 'pack-module.ps1') -StagePath $output -ZipPath (Join-Path $output $bundle) `
    -RequiredEntries @('vdhs_zygisk.zip','install-vd-picoenhance.ps1','pc/capsule.json','knuckles/PicoHand-Knuckles.zip','SHA256SUMS.txt')
$hash=(Get-FileHash -LiteralPath (Join-Path $output $bundle) -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::AppendAllText((Join-Path $output 'SHA256SUMS.txt'),"$hash  $bundle`n",$utf8)
Write-Host "Release bundle: $(Join-Path $output $bundle)"
