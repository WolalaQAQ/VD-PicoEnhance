param([string]$SdkRoot, [string]$JdkRoot, [string]$OutputPath)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_SDK_ROOT }
if (-not $SdkRoot) { $SdkRoot = $env:ANDROID_HOME }
if (-not $SdkRoot) { $SdkRoot = (Get-Content (Join-Path $RepoRoot 'tools/sdk-path.txt') -Raw).Trim() }
if (-not $JdkRoot) {
    $jdkFile = Join-Path $RepoRoot 'tools/jdk-path.txt'
    if (Test-Path $jdkFile) { $JdkRoot = (Get-Content $jdkFile -Raw).Trim() }
    else { $JdkRoot = $env:JAVA_HOME }
}
if (-not $JdkRoot) { throw 'Set JAVA_HOME to JDK 17, or record it in tools/jdk-path.txt.' }
$javac = Join-Path $JdkRoot 'bin/javac.exe'
$d8 = Join-Path $SdkRoot 'build-tools/35.0.0/d8.bat'
$android = Join-Path $SdkRoot 'platforms/android-35/android.jar'
foreach ($file in @($javac, $d8, $android)) { if (-not (Test-Path $file)) { throw "missing Java build input: $file" } }
$Build = if ($OutputPath) { [IO.Path]::GetFullPath($OutputPath) } else { Join-Path $PSScriptRoot 'build-arm64' }
$Classes = Join-Path $Build 'java-classes'
$Dex = Join-Path $Build 'tracking-control.jar'
New-Item -ItemType Directory -Force -Path $Classes | Out-Null
$sources = @(Get-ChildItem (Join-Path $PSScriptRoot 'java') -Recurse -Filter '*.java' | ForEach-Object FullName)
& $javac '-J-Dfile.encoding=UTF-8' -encoding UTF-8 -source 8 -target 8 -Xlint:all,-options -Werror -classpath $android -d $Classes @sources
if ($LASTEXITCODE -ne 0) { throw 'TrackingControl javac failed' }
$env:JAVA_HOME = $JdkRoot
$files = @(Get-ChildItem $Classes -Recurse -Filter '*.class' | ForEach-Object FullName)
& $d8 --lib $android --min-api 29 --output $Dex @files
if ($LASTEXITCODE -ne 0) { throw 'TrackingControl D8 failed' }
