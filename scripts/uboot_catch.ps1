# Try to stop U-Boot autoboot: spam spaces on the console while the device powers up.
# Usage: powershell -File uboot_catch.ps1 -Port COM6 [-Seconds 40]
# Stops sending as soon as a U-Boot prompt ("=>" or "sunxi#") appears; everything is logged to logs\uboot-*.log.
param([Parameter(Mandatory = $true)][string]$Port, [int]$Baud = 1500000, [int]$Seconds = 40)
$enc = [System.Text.Encoding]::GetEncoding(28591)
$logDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs'; New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir ("uboot-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))
$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.Open()
$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds($Seconds); $caught = $false
try {
    while ((Get-Date) -lt $deadline) {
        if (-not $caught) { $sp.Write(' ') }
        $n = $sp.BytesToRead
        if ($n -gt 0) { $b = New-Object byte[] $n; $r = $sp.Read($b, 0, $n); [void]$sb.Append($enc.GetString($b, 0, $r)) }
        if (-not $caught -and $sb.ToString() -match '(=>|sunxi#)\s*$') {
            $caught = $true; Write-Host "U-Boot prompt caught"
            $deadline = (Get-Date).AddSeconds(2)
        }
        Start-Sleep -Milliseconds 15
    }
}
finally { $sp.Close(); [System.IO.File]::WriteAllText($log, $sb.ToString(), $enc) }
if (-not $caught) { Write-Host "no U-Boot prompt" }
Write-Host "log: $log"
