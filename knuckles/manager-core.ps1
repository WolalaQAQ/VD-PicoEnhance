# Fixed-scope installation for the independently selected Knuckles module.
$script:KnucklesUtf8 = [Text.UTF8Encoding]::new($false)
$script:KnucklesFiles = @('driver.vrdrivermanifest','OPENVR-LICENSE.txt','ZIG-RUNTIME-NOTICES.txt','bin/win64/driver_picomixedvr.dll')

function Write-KnucklesAtomic([string]$Path, [string]$Text) {
    $temporary = $Path + '.tmp-' + [Guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllText($temporary, $Text, $script:KnucklesUtf8)
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $Path) }
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}
function Protect-KnucklesDirectory([string]$Path) {
    [IO.Directory]::CreateDirectory($Path) | Out-Null
    $acl = Get-Acl -LiteralPath $Path
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $rules = @($acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]))
    $identities = @($rules | ForEach-Object { $_.IdentityReference.Value } | Sort-Object)
    $expected = @($sid,'S-1-5-18') | Sort-Object
    $valid = $acl.AreAccessRulesProtected -and $rules.Count -eq 2 -and ($identities -join ',') -eq ($expected -join ',')
    foreach ($rule in $rules) {
        $valid = $valid -and $rule.IdentityReference.Value -in @($sid,'S-1-5-18') -and
            $rule.AccessControlType -eq 'Allow' -and $rule.FileSystemRights -eq 'FullControl' -and
            $rule.InheritanceFlags -eq 'ContainerInherit, ObjectInherit' -and $rule.PropagationFlags -eq 'None' -and -not $rule.IsInherited
    }
    if ($valid) { return }
    $acl.SetSecurityDescriptorSddlForm("D:P(A;OICI;FA;;;$sid)(A;OICI;FA;;;SY)", [Security.AccessControl.AccessControlSections]::Access)
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Assert-KnucklesPrivateFile([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { return }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $rules = @((Get-Acl -LiteralPath $Path).GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]))
    if ($rules.Count -ne 2) { throw 'Pairing file has unexpected permissions; it was not changed.' }
    foreach ($rule in $rules) {
        if (-not $rule.IsInherited -or $rule.IdentityReference.Value -notin @($sid,'S-1-5-18') -or
            $rule.AccessControlType -ne 'Allow' -or $rule.FileSystemRights -ne 'FullControl') {
            throw 'Pairing file is not private; it was not changed.'
        }
    }
}
function Invoke-KnucklesAdb($O, [string[]]$Arguments) {
    $output = & $O.Adb -s $O.Serial @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Headset command failed; no automatic retry was attempted.' }
    return $output
}
function Invoke-KnucklesRoot($O, [string]$Command) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Command))
    Invoke-KnucklesAdb $O @('shell',("su -c 'echo " + $encoded + " | base64 -d | sh'"))
}
function Assert-KnucklesIdle {
    if (Get-Process -Name vrserver -ErrorAction SilentlyContinue) { throw 'Exit SteamVR before installing, configuring or uninstalling PicoHand<->Knuckles.' }
}
function Knuckles-PrivateAddress([string]$Text) {
    $ip = [Net.IPAddress]::Parse($Text)
    $b = $ip.GetAddressBytes()
    if ($b.Length -ne 4 -or -not ($b[0] -eq 10 -or ($b[0] -eq 172 -and $b[1] -ge 16 -and $b[1] -le 31) -or ($b[0] -eq 192 -and $b[1] -eq 168))) {
        throw 'PicoHand<->Knuckles requires a private IPv4 LAN address.'
    }
    $ip.ToString()
}
function Get-KnucklesPcAddress([string]$HeadsetAddress) {
    $socket = New-Object Net.Sockets.Socket([Net.Sockets.AddressFamily]::InterNetwork,[Net.Sockets.SocketType]::Dgram,[Net.Sockets.ProtocolType]::Udp)
    try { $socket.Connect($HeadsetAddress,29744); return $socket.LocalEndPoint.Address.ToString() }
    finally { $socket.Dispose() }
}
function Set-KnucklesRegistration($O, [bool]$Present) {
    $before = Get-Content -LiteralPath $O.Registry -Raw | ConvertFrom-Json
    $target = [IO.Path]::GetFullPath((Join-Path $O.Config 'driver')).TrimEnd('\','/')
    $others = @($before.external_drivers | Where-Object { $_ -and [IO.Path]::GetFullPath($_).TrimEnd('\','/') -ine $target } | Sort-Object)
    $found = @($before.external_drivers | Where-Object { $_ -and [IO.Path]::GetFullPath($_).TrimEnd('\','/') -ieq $target }).Count -gt 0
    if ($found -eq $Present) { return }
    $tool = $O.VrPathReg
    if (-not $tool) { $tool = Join-Path $before.runtime[0] 'bin/win64/vrpathreg.exe' }
    $verb = if ($Present) { 'adddriver' } else { 'rmdriver' }
    $null = & $tool $verb $target
    if ($LASTEXITCODE -ne 0) { throw 'SteamVR driver registration failed.' }
    $after = Get-Content -LiteralPath $O.Registry -Raw | ConvertFrom-Json
    $beforeFields = [ordered]@{}
    $afterFields = [ordered]@{}
    foreach ($p in @($before.PSObject.Properties | Where-Object { $_.Name -ne 'external_drivers' } | Sort-Object Name)) { $beforeFields[$p.Name] = $p.Value }
    foreach ($p in @($after.PSObject.Properties | Where-Object { $_.Name -ne 'external_drivers' } | Sort-Object Name)) { $afterFields[$p.Name] = $p.Value }
    if (($beforeFields | ConvertTo-Json -Depth 12 -Compress) -cne ($afterFields | ConvertTo-Json -Depth 12 -Compress)) { throw 'Non-driver OpenVR registration fields changed; inspect before continuing.' }
    $afterOthers = @($after.external_drivers | Where-Object { $_ -and [IO.Path]::GetFullPath($_).TrimEnd('\','/') -ine $target } | Sort-Object)
    $afterFound = @($after.external_drivers | Where-Object { $_ -and [IO.Path]::GetFullPath($_).TrimEnd('\','/') -ieq $target }).Count -gt 0
    if (($others -join "`n") -cne ($afterOthers -join "`n") -or $afterFound -ne $Present) { throw 'Unexpected driver registration change; inspect openvrpaths before continuing.' }
}
function Set-KnucklesPhase($O, [string]$Action) {
    & (Join-Path $O.Package 'pc/control.ps1') -Action $Action -ConfigDirectory $O.Config
}
function Configure-Knuckles($O) {
    $address = $O.HeadsetAddress
    if (-not $address) {
        $network = Invoke-KnucklesAdb $O @('shell','ip -4 addr show wlan0') | Out-String
        if ($network -notmatch '\binet (\d+\.\d+\.\d+\.\d+)/') { throw 'Headset Wi-Fi IPv4 address not found; pass -HeadsetAddress.' }
        $address = $Matches[1]
    }
    $address = Knuckles-PrivateAddress $address
    $pc = $O.PcAddress
    if (-not $pc) { $pc = Get-KnucklesPcAddress $address }
    $pc = Knuckles-PrivateAddress $pc
    $command = @'
set -e
r=/data/adb/pico_mixedvr_standalone
d=/data/adb/modules_update/pico_mixedvr_standalone
[ -d "$d" ] || d=/data/adb/modules/pico_mixedvr_standalone
test -x "$d/bin/pico-mixedvrd"
choice=1
[ "$(cat "$r/requested" 2>/dev/null)" != 0 ] || choice=0
sh "$d/standalone.sh" stop
umask 077
mkdir -p "$r"
chmod 0700 "$r"
'@
    $command += "`nprintf `"%s %s\n`" $address $pc > `"`$r/network`"`nchmod 0600 `"`$r/network`"`n"
    $command += @'
sh "$d/standalone.sh" enable
if [ "$choice" = 0 ]; then
    sh "$d/standalone.sh" disable
elif [ "$d" = /data/adb/modules/pico_mixedvr_standalone ]; then
    sh "$d/standalone.sh" start
fi
'@
    $null = Invoke-KnucklesRoot $O $command
    $network = @(Invoke-KnucklesRoot $O 'cat /data/adb/pico_mixedvr_standalone/network')
    $networkBytes = (Invoke-KnucklesRoot $O 'wc -c < /data/adb/pico_mixedvr_standalone/network' | Out-String).Trim()
    $expectedNetwork = $address + ' ' + $pc
    if ($network.Count -ne 1 -or $network[0] -cne $expectedNetwork -or $networkBytes -ne [string]($expectedNetwork.Length + 1)) {
        throw 'Headset network pairing readback differs; pairing was not completed.'
    }
    $token = (Invoke-KnucklesRoot $O 'cat /data/adb/pico_mixedvr_standalone/token' | Out-String).Trim()
    if ($token -cnotmatch '\A[a-f0-9]{64}\z') { throw 'Headset pairing token format is invalid.' }
    Protect-KnucklesDirectory $O.Config
    foreach ($name in @('endpoint.txt','headset.token','phase.txt')) { Assert-KnucklesPrivateFile (Join-Path $O.Config $name) }
    Write-KnucklesAtomic (Join-Path $O.Config 'endpoint.txt') ($address + ":29744`n")
    Write-KnucklesAtomic (Join-Path $O.Config 'headset.token') ($token + "`n")
    if (-not (Test-Path -LiteralPath (Join-Path $O.Config 'phase.txt'))) {
        Write-KnucklesAtomic (Join-Path $O.Config 'phase.txt') "1 auto`n"
    }
    Write-Output 'Configured private LAN pairing. Existing Stop/Disable choices were retained.'
}
function Invoke-Knuckles($O, [string]$Action) {
    $receiptPath = Join-Path $O.Config 'installation.json'
    $receipt = if (Test-Path -LiteralPath $receiptPath) { Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json } else { $null }
    if ($Action -eq 'Status') {
        $phase = if (Test-Path -LiteralPath (Join-Path $O.Config 'phase.txt')) { [IO.File]::ReadAllText((Join-Path $O.Config 'phase.txt')).Trim() } else { 'not configured' }
        return [pscustomobject]@{name='PicoHand<->Knuckles'; installed=[bool]($receipt -and $receipt.installed); saved_control=$phase; execution_status='check SteamVR log and headset readback'}
    }
    if ($Action -in @('Install','Configure','Uninstall')) { Assert-KnucklesIdle }
    if ($Action -eq 'Install') {
        foreach ($name in $script:KnucklesFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $O.Package "pc/driver_picomixedvr/$name"))) { throw "Missing optional provider file: $name" }
        }
        if (-not (Test-Path -LiteralPath (Join-Path $O.Package 'PicoHand-Knuckles.zip'))) { throw 'Optional headset module ZIP is missing.' }
        Protect-KnucklesDirectory $O.Config
        $provider = Join-Path $O.Config 'driver'
        $manifest = Join-Path $provider 'driver.vrdrivermanifest'
        if ((Test-Path -LiteralPath $provider) -and (-not (Test-Path -LiteralPath $manifest) -or (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).name -cne 'picomixedvr')) { throw 'The provider destination is not owned by this component.' }
        # Stop the old owner before installing its independent successor.
        $null = Invoke-KnucklesRoot $O @'
set -e
if [ -d /data/adb/vdhs_mixedvr ]; then
    d=/data/adb/modules_update/vdhs_zygisk
    [ -f "$d/mixedvr.sh" ] || d=/data/adb/modules/vdhs_zygisk
    sh "$d/mixedvr.sh" disable
fi
'@
        $null = Invoke-KnucklesAdb $O @('push',(Join-Path $O.Package 'PicoHand-Knuckles.zip'),'/data/local/tmp/picohand-knuckles.zip')
        $null = Invoke-KnucklesRoot $O 'magisk --install-module /data/local/tmp/picohand-knuckles.zip'
        $inventory = [ordered]@{}
        foreach ($name in $script:KnucklesFiles) {
            $target = Join-Path $provider $name
            [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
            if (Test-Path -LiteralPath $target) {
                $backup = $target + '.previous'
                if (-not (Test-Path -LiteralPath $backup)) { [IO.File]::Copy($target,$backup,$false) }
            }
            [IO.File]::Copy((Join-Path $O.Package "pc/driver_picomixedvr/$name"),$target,$true)
            $inventory[$name] = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        Configure-Knuckles $O
        Set-KnucklesRegistration $O $true
        Write-KnucklesAtomic $receiptPath (([ordered]@{format='picohand-knuckles-v1'; installed=$true; files=$inventory} | ConvertTo-Json -Depth 5) + "`n")
        $null = Invoke-KnucklesRoot $O 'rm -f /data/local/tmp/picohand-knuckles.zip'
        return
    }
    if ($Action -eq 'Configure') { Configure-Knuckles $O; return }
    if (-not $receipt -or -not $receipt.installed) { throw 'PicoHand<->Knuckles is not installed; use -WithKnuckles during installation.' }
    if ($Action -eq 'Enable') {
        $null = Invoke-KnucklesRoot $O 'set -e; d=/data/adb/modules/pico_mixedvr_standalone; test ! -f "$d/remove"; rm -f "$d/disable"; sh "$d/standalone.sh" enable; sh "$d/standalone.sh" start'
        Set-KnucklesPhase $O 'Start'
        return
    }
    Set-KnucklesPhase $O 'Stop'
    $null = Invoke-KnucklesRoot $O 'sh /data/adb/modules/pico_mixedvr_standalone/standalone.sh disable'
    if ($Action -eq 'Uninstall') {
        Set-KnucklesRegistration $O $false
        $null = Invoke-KnucklesRoot $O 'touch /data/adb/modules/pico_mixedvr_standalone/remove'
        $receipt.installed = $false
        Write-KnucklesAtomic $receiptPath (($receipt | ConvertTo-Json -Depth 5) + "`n")
        Write-Output 'Optional module restored and unregistered. Pairing and recovery copies retained; base enhancements and battery patch unchanged.'
    }
}
