param([ValidateRange(30, 1800)][int]$DurationSeconds = 1200)
$ErrorActionPreference = 'Stop'
$traceDirectory = Join-Path $PSScriptRoot 'diagnostics'
New-Item -ItemType Directory -Path $traceDirectory -Force | Out-Null
$captureName = 'wake-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$sessionName = 'DiskSwitcher-' + $captureName
$traceFile = Join-Path $traceDirectory ($captureName + '.etl')
$statusFile = Join-Path $traceDirectory 'capture-status.txt'
$stopMarker = Join-Path $traceDirectory ($captureName + '.stop')
$started = $false
try {
    $startOutput = & wpr.exe -start DiskIO.light -start FileIO.light -instancename $sessionName 2>&1
    if ($LASTEXITCODE -ne 0) { throw "WPR start failed: $startOutput" }
    $started = $true
    $deadline = (Get-Date).AddSeconds($DurationSeconds)
    @("Recording since: $(Get-Date -Format o)", "Deadline: $($deadline.ToString('o'))",
      "Session: $sessionName", "Output: $traceFile", "Stop marker: $stopMarker",
      'Memory mode. Records OS I/O events; does not poll the HDD.') | Set-Content -LiteralPath $statusFile -Encoding UTF8
    while ((Get-Date) -lt $deadline -and !(Test-Path -LiteralPath $stopMarker)) {
        Start-Sleep -Seconds 2
    }
    $stopOutput = & wpr.exe -stop $traceFile 'Investigate intermittent wake of Seagate WFL4Q67Z (Disk 1)' -skipPdbGen -instancename $sessionName 2>&1
    if ($LASTEXITCODE -ne 0) { throw "WPR stop failed: $stopOutput" }
    $started = $false
    @("Saved: $(Get-Date -Format o)", "Trace: $traceFile", "Session: $sessionName") |
        Set-Content -LiteralPath $statusFile -Encoding UTF8
} catch {
    @("Error: $(Get-Date -Format o)", $_.Exception.Message, "Session: $sessionName") |
        Set-Content -LiteralPath $statusFile -Encoding UTF8
} finally {
    if ($started) { & wpr.exe -cancel -instancename $sessionName | Out-Null }
}
