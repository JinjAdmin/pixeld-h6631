# Send shell command(s) to the H6631 root console and print what comes back.
# Usage: powershell -File uart_cmd.ps1 -Port COM6 -Cmd 'uname -a' [-Wait 3]
# Output between unique start/end markers is shown; the app's debug spam is interleaved, so markers help.
# Every session is appended to logs\shell-<date>.log.
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [Parameter(Mandatory = $true)][string]$Cmd,
    [int]$Baud = 1500000,
    [double]$Wait = 3
)
$enc = [System.Text.Encoding]::GetEncoding(28591)
$logDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir ("shell-{0:yyyyMMdd}.log" -f (Get-Date))

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.DtrEnable = $false; $sp.RtsEnable = $false
$sp.Encoding = $enc
$sp.Open()
try {
    $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $sp.DiscardInBuffer()
    # Markers are built with printf so the echoed command line doesn't match them.
    $line = "printf '\n@@S%s@@\n' $tag; $Cmd; printf '\n@@E%s@@\n' $tag`n"
    $sp.Write($line)
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($Wait)
    while ((Get-Date) -lt $deadline) {
        $n = $sp.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n; $r = $sp.Read($buf, 0, $n)
            [void]$sb.Append($enc.GetString($buf, 0, $r))
            if ($sb.ToString().Contains("@@E$tag@@")) { Start-Sleep -Milliseconds 100; $deadline = Get-Date }
        } else { Start-Sleep -Milliseconds 20 }
    }
    $all = $sb.ToString()
    Add-Content -Path $log -Value ("===== {0:HH:mm:ss} $ {1}`r`n{2}" -f (Get-Date), $Cmd, $all) -Encoding UTF8
    $e = $all.IndexOf("@@E$tag@@")
    $s = if ($e -gt 0) { $all.LastIndexOf("@@S$tag@@", $e) } else { -1 }
    if ($s -ge 0 -and $e -gt $s) {
        $m = "@@S$tag@@".Length
        $body = $all.Substring($s + $m, $e - $s - $m)
        # drop the app's interleaved debug lines
        ($body -split "`r?`n" | Where-Object { $_ -notmatch '^\[(debug|info|err)\]\[|recvfrom timeout|wait for exit timeout|^\s*$' }) -join "`n"
    } else {
        "[no end marker within $Wait s; raw output follows]"; $all
    }
}
finally { $sp.Close() }
