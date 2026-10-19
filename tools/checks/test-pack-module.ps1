param([string]$TempRoot = [IO.Path]::GetTempPath())

$ErrorActionPreference = 'Stop'
$packer = Join-Path (Split-Path -Parent $PSScriptRoot) 'pack-module.ps1'
$work = Join-Path $TempRoot ('vdhs-pack-module-' + [Guid]::NewGuid().ToString('N'))
$stage = Join-Path $work 'module stage [fixture]'
$zip = Join-Path $work 'module [fixture].zip'

function Write-Fixture([string]$Name, [byte[]]$Bytes) {
    $path = Join-Path $stage $Name
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllBytes($path, $Bytes)
}

function Assert-Zip([string[]]$ExpectedEntries) {
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $actual = @($archive.Entries | ForEach-Object { $_.FullName })
        [string[]]$expected = @($ExpectedEntries)
        [Array]::Sort($expected, [StringComparer]::Ordinal)
        if (($actual -join "`n") -cne ($expected -join "`n")) {
            throw "unexpected ZIP entry list or order: $($actual -join ', ')"
        }
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName -match '\\|^/|^[A-Za-z]:|(^|/)\.\.(/|$)') {
                throw "entry is not a forward-slash relative path: $($entry.FullName)"
            }
            $source = [IO.File]::ReadAllBytes((Join-Path $stage $entry.FullName))
            $target = New-Object IO.MemoryStream
            try {
                $stream = $entry.Open()
                try { $stream.CopyTo($target) } finally { $stream.Dispose() }
                if ([Convert]::ToBase64String($source) -cne [Convert]::ToBase64String($target.ToArray())) {
                    throw "entry content differs: $($entry.FullName)"
                }
            } finally { $target.Dispose() }
        }
    } finally { $archive.Dispose() }
}

function Assert-Unlocked([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $stream.Dispose()
}

function Assert-Fails([scriptblock]$Action, [string]$MessagePattern) {
    $failure = $null
    try { & $Action } catch { $failure = $_ }
    if ($null -eq $failure -or $failure.Exception.Message -notlike $MessagePattern) {
        throw "expected failure matching '$MessagePattern', got: $failure"
    }
}

try {
    $baseEntries = @(
        'module.prop', 'customize.sh', 'post-fs-data.sh', 'service.sh',
        'zygisk/arm64-v8a.so', 'payload/libvdhs.so', 'payload/VdHsMod.dll',
        'payload/mode.txt', 'payload/backend.txt', 'payload/hand_mesh_fb.bin'
    )
    foreach ($name in $baseEntries) { Write-Fixture $name ([Text.Encoding]::UTF8.GetBytes($name)) }
    # A module ZIP must not acquire the stage directory or a neighboring file.
    [IO.File]::WriteAllText((Join-Path $work 'outside.txt'), 'not part of the module')
    Push-Location $work
    try {
        & $packer -StagePath '.\module stage [fixture]\' -ZipPath '.\module [fixture].zip'
    } finally { Pop-Location }
    Assert-Zip $baseEntries
    Assert-Unlocked $zip
    foreach ($name in $baseEntries) { Assert-Unlocked (Join-Path $stage $name) }
    Write-Host 'PASS: base layout, relative paths, ordinal order, exact contents, released streams'

    # Additional caller-owned payloads are included without changing the packer.
    $extraEntries = @(
        'bin/helper', 'optional/helper-data.bin',
        'helper.sh', 'cleanup.sh',
        'payload/libvdhs_shell.so', 'payload/optional-setting.txt',
        'payload/sub directory/[literal].bin', 'payload/.hidden', 'payload/empty.bin'
    )
    foreach ($name in $extraEntries) { Write-Fixture $name ([byte[]]@(0, 1, 127, 128, 255)) }
    Write-Fixture 'payload/empty.bin' ([byte[]]@())
    $allEntries = $baseEntries + $extraEntries
    & $packer -StagePath $stage -ZipPath $zip -RequiredEntries $allEntries
    Assert-Zip $allEntries
    Assert-Unlocked $zip
    Write-Host 'PASS: extra scripts/directories, optional payloads, hidden/empty/binary files'

    Assert-Fails { & $packer -StagePath $stage -ZipPath $zip -RequiredEntries @('payload/missing.so') } `
        'missing required module entry: payload/missing.so'
    Assert-Unlocked $zip
    Write-Host 'PASS: missing required ZIP entry fails and releases the archive'

    $lockedPath = Join-Path $stage 'payload/libvdhs.so'
    $lock = [IO.File]::Open($lockedPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        Assert-Fails { & $packer -StagePath $stage -ZipPath $zip } '*OpenRead*'
        Assert-Unlocked $zip
    } finally { $lock.Dispose() }
    foreach ($name in $allEntries) { Assert-Unlocked (Join-Path $stage $name) }
    # Replacing even a larger/partial ZIP leaves no stale entries.
    foreach ($name in $extraEntries) { [IO.File]::Delete((Join-Path $stage $name)) }
    & $packer -StagePath $stage -ZipPath $zip
    Assert-Zip $baseEntries
    Assert-Unlocked $zip
    Write-Host 'PASS: input-open failure releases streams; repack replaces existing ZIP'
    Write-Host "All module ZIP tests passed on PowerShell $($PSVersionTable.PSVersion)."
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
