param([string]$SdkRoot, [string]$JdkRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_SDK_ROOT }
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_HOME }
if (-not $SdkRoot) { $SdkRoot = (Get-Content (Join-Path $repo 'tools/sdk-path.txt') -Raw).Trim() }
if (-not $JdkRoot) {
    $file = Join-Path $repo 'tools/jdk-path.txt'
    if (Test-Path $file) { $JdkRoot = (Get-Content $file -Raw).Trim() }
    else { $JdkRoot = $env:JAVA_HOME }
}
if (-not $JdkRoot) { throw 'JDK 17 is required (tools/jdk-path.txt or JAVA_HOME).' }
$javac = Join-Path $JdkRoot 'bin/javac.exe'
$d8 = Join-Path $SdkRoot 'build-tools/35.0.0/d8.bat'
$android = Join-Path $SdkRoot 'platforms/android-35/android.jar'
foreach ($file in @($javac, $d8, $android)) { if (-not (Test-Path $file)) { throw "Missing build input: $file" } }
$build = Join-Path $PSScriptRoot 'build-arm64'
$classes = Join-Path $build 'classes'
$dex = Join-Path $build 'dex'
New-Item -ItemType Directory -Force -Path $classes, $dex | Out-Null
$sources = @(Get-ChildItem (Join-Path $PSScriptRoot 'java') -Recurse -Filter '*.java' | ForEach-Object FullName)
if (-not $sources.Count) { throw 'Capability helper source is missing.' }
& $javac -encoding UTF-8 -source 8 -target 8 -Xlint:all,-options,-deprecation -Werror -classpath $android -d $classes @sources
if ($LASTEXITCODE -ne 0) { throw 'Capability helper javac failed' }
$oldJava = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $JdkRoot
    $inputs = @(Get-ChildItem $classes -Recurse -Filter '*.class' | ForEach-Object FullName)
    & $d8 --lib $android --min-api 29 --output $dex @inputs
    if ($LASTEXITCODE -ne 0) { throw 'Capability helper D8 failed' }
} finally { $env:JAVA_HOME = $oldJava }
$jar = Join-Path $build 'control.jar'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temporary = $jar + '.tmp-' + [Guid]::NewGuid().ToString('N')
$archive = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
try { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $dex 'classes.dex'), 'classes.dex') | Out-Null }
finally { $archive.Dispose() }
Move-Item -LiteralPath $temporary -Destination $jar -Force
