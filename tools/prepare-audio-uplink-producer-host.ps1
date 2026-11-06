param([string]$OutputDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Get-Content -LiteralPath "$root/main/conversation/audio_uplink.c" -Raw
# Compile the exact production producer in capture tests. Only the asynchronous
# queue/network boundary is faked; sequence validation and PCM encoding are real.
function Read-Function([string]$Name) {
    $match = [regex]::Match($source, "(?m)^esp_err_t $Name\(")
    if (-not $match.Success) { throw "Missing production function: $Name" }
    $start = $match.Index
    $open = $source.IndexOf('{', $start)
    $depth = 1
    $end = $open + 1
    while ($depth -gt 0 -and $end -lt $source.Length) {
        if ($source[$end] -eq '{') { $depth++ }
        if ($source[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Unbalanced production function: $Name" }
    return $source.Substring($start, $end - $start)
}
$pcm = Read-Function 'nuwax_audio_uplink_build_pcm16_frame'
$enqueue = (Read-Function 'nuwax_audio_uplink_enqueue_frame').Replace(
    'nuwax_audio_uplink_enqueue_frame(', 'host_production_enqueue_frame(')
[IO.File]::WriteAllText((Join-Path $OutputDir 'audio_uplink_producer_host.inc'),
    "$pcm`n$enqueue`n")
