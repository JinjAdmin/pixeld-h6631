# Receive files pushed from the H6631 with `nc <pc-ip> <port> < file`, one TCP connection per file, in order.
# Usage: powershell -File recv_files.ps1 -OutDir backup\2026-11-14 -Names a.bin,b.bin [-Port 9123]
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string[]]$Names,
    [int]$Port = 9123
)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$l = New-Object System.Net.Sockets.TcpListener ([System.Net.IPAddress]::Any), $Port
$l.Start()
Write-Host "listening on $Port"
try {
    foreach ($name in $Names) {
        $c = $l.AcceptTcpClient()
        $path = Join-Path $OutDir $name
        $fs = [System.IO.File]::Create($path)
        $c.GetStream().CopyTo($fs)
        $fs.Close(); $c.Close()
        Write-Host ("{0} {1} bytes" -f $name, (Get-Item $path).Length)
    }
}
finally { $l.Stop() }
