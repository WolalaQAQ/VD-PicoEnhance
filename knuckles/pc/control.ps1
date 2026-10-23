param([ValidateSet('Start','Stop','Status')][string]$Action = 'Status', [string]$ConfigDirectory)
$ErrorActionPreference = 'Stop'
if (-not $ConfigDirectory) { $ConfigDirectory = Join-Path $env:LOCALAPPDATA 'PicoMixedVR' }
$path = Join-Path $ConfigDirectory 'phase.txt'
if ($Action -eq 'Status') {
    if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path }
    else { Write-Output 'restore (prototype default)' }
    return
}
if (-not (Test-Path -LiteralPath $ConfigDirectory -PathType Container)) { throw 'Configure the private pairing directory first.' }
[UInt64]$sequence = 0
if (Test-Path -LiteralPath $path) {
    $old = [IO.File]::ReadAllText($path).Trim()
    if ($old -notmatch '^(\d+) (auto|restore|stop)$') { throw 'Invalid saved phase.' }
    $sequence = [UInt64]$Matches[1]
}
$sequence++
$mode = if ($Action -eq 'Start') { 'auto' } else { 'restore' }
$temporary = Join-Path $ConfigDirectory ([Guid]::NewGuid().ToString('N') + '.tmp')
[IO.File]::WriteAllText($temporary, "$sequence $mode`n", [Text.UTF8Encoding]::new($false))
try {
    if (Test-Path -LiteralPath $path) { [IO.File]::Replace($temporary, $path, [NullString]::Value) }
    else { [IO.File]::Move($temporary, $path) }
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}
Write-Output "$sequence $mode requested; execution must be checked in the driver/daemon receipt."
