param([Parameter(Mandatory=$true)][string]$BundlePath)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Assert([bool]$Condition,[string]$Text) { if (-not $Condition) { throw $Text } }
function Read-Entry($Entry) {
    $memory=New-Object IO.MemoryStream
    $stream=$Entry.Open()
    try { $stream.CopyTo($memory); return ,$memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
}
$archive=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($BundlePath))
try {
    $entries=@{}
    foreach ($entry in $archive.Entries) {
        $name=$entry.FullName
        Assert ($name -notmatch '\\|^/|\.\./|^[A-Za-z]:') "Invalid ZIP path: $name"
        Assert ($name -notmatch '(?i)driver_VirtualDesktop\.dll|(^|/)(work|case|evidence|\.local|reference|\.git|build)/|(^|/)(headset\.token|pairing\.json|phase\.txt|connection\.txt)$') "Non-distributable file: $name"
        if ($name.EndsWith('/')) { continue }
        Assert (-not $entries.ContainsKey($name)) "Duplicate ZIP entry: $name"
        $entries[$name]=$entry
    }
    foreach ($name in @('vdhs_zygisk.zip','install-vd-picoenhance.ps1','hand_mesh_fb.bin','LICENSE','THIRD_PARTY_NOTICES.md','RELEASE_NOTES.md','SHA256SUMS.txt','docs/PC-SETUP.md','docs/PC-SETUP_zh.md','pc/vd-picoenhance.pe','pc/capsule.json','pc/NativePatch.cs','pc/install.ps1','pc/OPENVR-LICENSE.txt','knuckles/manage.ps1','knuckles/manager-core.ps1','knuckles/PicoHand-Knuckles.zip','knuckles/pc/control.ps1','knuckles/pc/driver_picomixedvr/driver.vrdrivermanifest','knuckles/pc/driver_picomixedvr/OPENVR-LICENSE.txt','knuckles/pc/driver_picomixedvr/bin/win64/driver_picomixedvr.dll')) { Assert ($entries.ContainsKey($name)) "Missing release entry: $name" }
    Assert (-not $entries.ContainsKey('pc/mixedvr.ps1')) 'Archived VD-stream manager was included as a release entrypoint.'
    Assert ($entries.ContainsKey('knuckles/pc/driver_picomixedvr/ZIG-RUNTIME-NOTICES.txt')) 'Linked-runtime license texts missing.'
    $checksums=[Text.Encoding]::UTF8.GetString((Read-Entry $entries['SHA256SUMS.txt']))
    Assert (-not $checksums.Contains("`r")) 'Checksums must use LF.'
    $checked=0
    foreach ($line in $checksums.Trim().Split("`n")) {
        Assert ($line -match '^([a-f0-9]{64})  (.+)$') 'Invalid checksum line.'
        $digest=$Matches[1]; $name=$Matches[2]
        Assert ($entries.ContainsKey($name)) "Checksum has no ZIP entry: $name"
        $sha=[Security.Cryptography.SHA256]::Create()
        try { $actual=[BitConverter]::ToString($sha.ComputeHash((Read-Entry $entries[$name]))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
        Assert ($actual -eq $digest) "Content hash mismatch: $name"
        $checked++
    }
    Assert ($checked -eq $entries.Count-1) 'Checksum coverage is incomplete.'
    $metadata=[Text.Encoding]::UTF8.GetString((Read-Entry $entries['pc/capsule.json'])) | ConvertFrom-Json
    Assert ($metadata.format -eq 'vd-picoenhance-native-v1') 'Wrong capsule format.'
    Assert ($metadata.kind -eq 'battery-only-v1') 'Base capsule is not battery-only.'
    foreach ($encoding in @([Text.Encoding]::ASCII,[Text.Encoding]::Unicode)) {
        $text=$encoding.GetString((Read-Entry $entries['pc/vd-picoenhance.pe']))
        Assert ($text -notmatch '(?i)[A-Z]:\\Users\\|mixedvr-vd-driver-20261020') 'Local path leaked into PC capsule.'
    }
    $moduleBytes=Read-Entry $entries['vdhs_zygisk.zip']
    $memory=New-Object IO.MemoryStream -ArgumentList (,$moduleBytes)
    $module=New-Object IO.Compression.ZipArchive -ArgumentList $memory,([IO.Compression.ZipArchiveMode]::Read)
    try {
        foreach ($name in @('mixedvr.sh','bin/vdhs-mixedvrd','bin/tracking-control.jar','payload/libvdhs.so','payload/VdHsMod.dll','module.prop')) { Assert ($null -ne $module.GetEntry($name)) "Missing headset entry: $name" }
        foreach ($entry in $module.Entries) { Assert ($entry.FullName -notmatch '(^|/)(token|enabled|requested|baseline|pid|ready|inflight)$') 'Runtime companion state was packaged.' }
        $helperText=[Text.Encoding]::ASCII.GetString((Read-Entry $module.GetEntry('bin/vdhs-mixedvrd')))
        Assert ($helperText -notmatch '(?i)[A-Z]:[/\\]+Users[/\\]+|mixedvr-vd-driver-20261020|headset-reuse/source') 'Build-machine path leaked into headset helper.'
    } finally { $module.Dispose(); $memory.Dispose() }
    foreach ($name in @('knuckles/pc/driver_picomixedvr/bin/win64/driver_picomixedvr.dll')) {
        foreach ($encoding in @([Text.Encoding]::ASCII,[Text.Encoding]::Unicode)) {
            $text=$encoding.GetString((Read-Entry $entries[$name]))
            Assert ($text -notmatch '(?i)[A-Z]:[/\\]+Users[/\\]+|standalone-mixedvr-20261022') 'Local build path leaked into optional provider.'
        }
    }
    $memory=New-Object IO.MemoryStream -ArgumentList (,(Read-Entry $entries['knuckles/PicoHand-Knuckles.zip']))
    $optional=New-Object IO.Compression.ZipArchive -ArgumentList $memory,([IO.Compression.ZipArchiveMode]::Read)
    try {
        foreach ($name in @('module.prop','customize.sh','service.sh','standalone.sh','uninstall.sh','bin/pico-mixedvrd','bin/tracking-control.jar')) { Assert ($null -ne $optional.GetEntry($name)) "Missing optional module entry: $name" }
        $props=[Text.Encoding]::UTF8.GetString((Read-Entry $optional.GetEntry('module.prop')))
        Assert ($props -cmatch '(?m)^name=PicoHand<->Knuckles\r?$') 'Optional module display name differs.'
        foreach ($entry in $optional.Entries) {
            Assert ($entry.FullName -notmatch '\\|^/|\.\./|(^|/)(token|network|enabled|requested|baseline|pid|ready|inflight|lease)$') 'Runtime state or unsafe path in optional module.'
        }
        $text=[Text.Encoding]::ASCII.GetString((Read-Entry $optional.GetEntry('bin/pico-mixedvrd')))
        Assert ($text -notmatch '(?i)[A-Z]:[/\\]+Users[/\\]+|/data/data|mixedvr_request.txt') 'Optional daemon contains personal paths or old VD frame/file control.'
    } finally { $optional.Dispose(); $memory.Dispose() }
    Write-Host "PASS: release layout, $checked content hashes, nested helper/module entries, no original VD image, runtime state or machine paths."
} finally { $archive.Dispose() }
