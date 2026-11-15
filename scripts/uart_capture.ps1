# Capture the H6631 serial console to a log file (listen-only by default).
# Usage:  powershell -ExecutionPolicy Bypass -File uart_capture.ps1 -Port COM5 [-Baud 115200] [-Interactive]
#   -Interactive  also sends what you type to the device (only once the RX wire is connected).
#   Ctrl+C to stop. Log goes to logs\uart-<timestamp>.log in the repo root.
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [int]$Baud = 115200,
    [switch]$Interactive,
    [int]$Seconds = 0   # stop automatically after this many seconds (0 = run until Ctrl+C)
)
$deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [datetime]::MaxValue }

$logDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir ("uart-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
# Keep DTR/RTS low so the ESP32 board's auto-reset circuit isn't toggled.
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Encoding = [System.Text.Encoding]::GetEncoding(28591)  # latin1: keeps every byte
$sp.ReadTimeout = 50
$sp.Open()
Write-Host "Listening on $Port @ $Baud -> $log  (Ctrl+C to stop)" -ForegroundColor Cyan

$fs = [System.IO.File]::Open($log, 'Append', 'Write', 'Read')
try {
    while ((Get-Date) -lt $deadline) {
        $n = $sp.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n
            $r = $sp.Read($buf, 0, $n)
            $fs.Write($buf, 0, $r); $fs.Flush()
            [Console]::Write([System.Text.Encoding]::GetEncoding(28591).GetString($buf, 0, $r))
        }
        if ($Interactive -and [Console]::KeyAvailable) {
            $k = [Console]::ReadKey($true)
            if ($k.Key -eq 'Enter') { $sp.Write("`n") } else { $sp.Write([string]$k.KeyChar) }
        }
        if ($n -eq 0) { Start-Sleep -Milliseconds 10 }
    }
}
finally {
    $fs.Close(); $sp.Close()
    Write-Host "`nSaved $log" -ForegroundColor Cyan
}
