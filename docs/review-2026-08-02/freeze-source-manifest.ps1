param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'source-manifest.sha256'),
    [string]$PreviousManifestPath = ''
)

$ErrorActionPreference = 'Stop'

function Get-LongPath([string]$Path) {
    if ($Path.StartsWith('\\?\')) {
        return $Path
    }
    return '\\?\' + [System.IO.Path]::GetFullPath($Path)
}

function Get-FileSha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead((Get-LongPath $Path))
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha256.ComputeHash($stream)
        return -join ($hash | ForEach-Object { $_.ToString('x2') })
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Test-Excluded([string]$RelativePath) {
    $path = $RelativePath.Replace('\', '/')
    $segments = $path.Split('/')

    if ($segments[0] -match '^build(?:-|$)') {
        return $true
    }
    if ($segments -contains 'build') {
        return $true
    }
    foreach ($excludedSegment in @('.git', '.cache', '__pycache__', 'node_modules')) {
        if ($segments -contains $excludedSegment) {
            return $true
        }
    }
    if ([System.IO.Path]::GetExtension($path) -ieq '.pyc') {
        return $true
    }
    if ($path.StartsWith('docs/review-2026-08-02/', [System.StringComparison]::Ordinal)) {
        return $true
    }
    $executionRecordName = 'docs/2026-08-02-NuwaxAIPartner-v2' +
        [char]0x5168 + [char]0x5de5 + [char]0x7a0b + [char]0x4ee3 +
        [char]0x7801 + [char]0x8bc4 + [char]0x5ba1 + [char]0x6267 +
        [char]0x884c + [char]0x5de5 + [char]0x4f5c + [char]0x8bb0 +
        [char]0x5f55 + '.md'
    if ($path -eq $executionRecordName) {
        return $true
    }
    return $false
}

$root = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\')
$rootPrefix = $root + '\'
$longRoot = Get-LongPath $root
$entries = [System.Collections.Generic.List[object]]::new()

foreach ($longFile in [System.IO.Directory]::EnumerateFiles(
             $longRoot, '*', [System.IO.SearchOption]::AllDirectories)) {
    $absolute = $longFile.Substring(4)
    if (-not $absolute.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped project root: $absolute"
    }
    $relative = $absolute.Substring($rootPrefix.Length).Replace('\', '/')
    if (Test-Excluded $relative) {
        continue
    }
    $entries.Add([pscustomobject]@{
        Path = $relative
        Hash = Get-FileSha256 $absolute
    })
}

$entriesByPath = @{}
foreach ($entry in $entries) {
    if ($entriesByPath.ContainsKey($entry.Path)) {
        throw "Duplicate relative path: $($entry.Path)"
    }
    $entriesByPath[$entry.Path] = $entry
}
$sortedPaths = [string[]]@($entriesByPath.Keys)
[System.Array]::Sort($sortedPaths, [System.StringComparer]::Ordinal)
$sorted = @($sortedPaths | ForEach-Object { $entriesByPath[$_] })
$lines = @($sorted | ForEach-Object { '{0} *{1}' -f $_.Hash, $_.Path })
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllLines((Get-LongPath $ManifestPath), $lines, $utf8NoBom)

$manifestHash = Get-FileSha256 $ManifestPath
$result = [ordered]@{
    ProjectRoot = $root
    EntryCount = $lines.Count
    ManifestSha256 = $manifestHash
    DuplicatePaths = @($sorted | Group-Object Path | Where-Object Count -gt 1).Count
    PreviousMissing = 0
    PreviousMismatch = 0
    CurrentExtra = 0
}

if ($PreviousManifestPath) {
    $previous = @{}
    foreach ($line in [System.IO.File]::ReadAllLines((Get-LongPath $PreviousManifestPath))) {
        if ($line -notmatch '^([0-9a-f]{64}) \*(.+)$') {
            throw "Malformed previous manifest line: $line"
        }
        $previous[$Matches[2]] = $Matches[1]
    }
    $current = @{}
    foreach ($entry in $sorted) {
        $current[$entry.Path] = $entry.Hash
    }
    $result.PreviousMissing = @($previous.Keys | Where-Object { -not $current.ContainsKey($_) }).Count
    $result.PreviousMismatch = @($previous.Keys | Where-Object {
            $current.ContainsKey($_) -and $current[$_] -ne $previous[$_]
        }).Count
    $result.CurrentExtra = @($current.Keys | Where-Object { -not $previous.ContainsKey($_) }).Count
}

[pscustomobject]$result
