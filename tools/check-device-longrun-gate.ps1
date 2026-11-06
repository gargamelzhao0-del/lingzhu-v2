param(
    [Parameter(Mandatory=$true)] [string]$LogPath,
    [ValidateRange(1,31536000)] [int]$MinimumDurationSeconds = 259200,
    [ValidateRange(1,3600)] [int]$MaximumGapSeconds = 120,
    [ValidateRange(1,2147483647)] [int]$MinimumInternalBytes = 16384,
    [ValidateRange(1,2147483647)] [int]$MinimumDmaLargestBytes = 8192,
    [string]$ReportPath,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$arguments = @((Join-Path $PSScriptRoot 'check-device-longrun-gate.py'), $LogPath,
    '--minimum-duration', $MinimumDurationSeconds, '--maximum-gap', $MaximumGapSeconds,
    '--minimum-internal', $MinimumInternalBytes, '--minimum-dma', $MinimumDmaLargestBytes)
if ($ReportPath) { $arguments += @('--report', $ReportPath) }
& $Python @arguments
# 0: criteria satisfied; 2: observed violation; 3: insufficient/ambiguous evidence.
exit $LASTEXITCODE
