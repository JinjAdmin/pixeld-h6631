# Ask for the MQTT broker details and write private\mqtt.conf for the light's display program.
# Run it yourself; the password is typed hidden and is only written to that file (key=value, LF, no BOM).
$ErrorActionPreference = 'Stop'
$dir = Join-Path (Split-Path $PSScriptRoot -Parent) 'private'
New-Item -ItemType Directory -Force $dir | Out-Null
$hostName = Read-Host 'MQTT broker IP address (e.g. 192.168.1.10)'
$port = Read-Host 'MQTT port [1883]'
if (-not $port) { $port = '1883' }
$user = Read-Host 'MQTT username for the light'
$sec = Read-Host 'MQTT password' -AsSecureString
$b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
try {
    $pw = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)
    if ($pw -match "[`r`n]") { throw 'Password cannot contain line breaks.' }
    $conf = "host=$hostName`nport=$port`nuser=$user`npass=$pw`n"
    $path = Join-Path $dir 'mqtt.conf'
    [IO.File]::WriteAllText($path, $conf, (New-Object Text.UTF8Encoding $false))
    Write-Host "Saved $path"
}
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b); $pw = $null }
