param(
    [Parameter(Mandatory = $true)]
    [string]$StagePath,
    [Parameter(Mandatory = $true)]
    [string]$ZipPath,
    [string[]]$RequiredEntries = @(
        'module.prop', 'customize.sh', 'post-fs-data.sh', 'service.sh',
        'coldstart.sh', 'uninstall.sh', 'capability/classes.dex', 'capability/control.jar',
        'zygisk/arm64-v8a.so', 'payload/libvdhs.so', 'payload/VdHsMod.dll',
        'payload/mode.txt', 'payload/backend.txt', 'payload/hand_mesh_fb.bin'
    )
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = (Get-Item -LiteralPath $StagePath).FullName.TrimEnd([char[]]'\/')
$prefix = $root + [IO.Path]::DirectorySeparatorChar
$zip = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ZipPath)
[string[]]$relativePaths = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object {
    $_.FullName.Substring($prefix.Length).Replace('\', '/')
})
[Array]::Sort($relativePaths, [StringComparer]::Ordinal)

$zipStream = [IO.File]::Open($zip, [IO.FileMode]::Create)
try {
    $archive = New-Object IO.Compression.ZipArchive($zipStream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($relativePath in $relativePaths) {
            $entry = $archive.CreateEntry($relativePath, [IO.Compression.CompressionLevel]::Optimal)
            $source = [IO.File]::OpenRead((Join-Path $root $relativePath))
            try {
                $target = $entry.Open()
                try { $source.CopyTo($target) } finally { $target.Dispose() }
            } finally { $source.Dispose() }
        }
    } finally { $archive.Dispose() }
} finally { $zipStream.Dispose() }

$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($archive.Entries | ForEach-Object { $_.FullName })
    if (@($entries | Where-Object { $_.Contains('\') }).Count -gt 0) {
        throw 'module ZIP contains backslash entry names'
    }
    foreach ($required in $RequiredEntries) {
        if ($entries -cnotcontains $required) { throw "missing required module entry: $required" }
    }
} finally { $archive.Dispose() }
