# Ask for Wi-Fi name and password and write private\wpa_supplicant.conf for the H6631 custom firmware.
# Run it yourself; the password is typed hidden and is only written to that file (LF endings, no BOM).
$ErrorActionPreference = 'Stop'
$dir = Join-Path (Split-Path $PSScriptRoot -Parent) 'private'
New-Item -ItemType Directory -Force $dir | Out-Null
$ssid = Read-Host 'Wi-Fi name (SSID, case-sensitive)'
$sec = Read-Host 'Wi-Fi password' -AsSecureString
$sec2 = Read-Host 'Wi-Fi password again' -AsSecureString
$b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
$b2 = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec2)
try {
    $pw = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)
    $pw2 = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b2)
    if ($pw -ne $pw2) { throw 'Passwords did not match - run the script again.' }
    if ($pw.Length -lt 8 -or $pw.Length -gt 63) { throw 'WPA passwords must be 8-63 characters.' }
    if ($ssid -match '"' -or $pw -match '"') { throw 'Name or password contains a double quote; tell Claude so the file format can be adjusted.' }
    $conf = "ctrl_interface=/var/run/wpa_supplicant`nupdate_config=0`n`nnetwork={`n`tssid=`"$ssid`"`n`tpsk=`"$pw`"`n`tkey_mgmt=WPA-PSK`n`tscan_ssid=1`n}`n"
    $path = Join-Path $dir 'wpa_supplicant.conf'
    [IO.File]::WriteAllText($path, $conf, (New-Object Text.UTF8Encoding $false))
    Write-Host "Saved $path"
}
finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b); [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b2)
    $pw = $null; $pw2 = $null
}
