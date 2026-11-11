[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Install,

    [Parameter(Mandatory = $true, Position = 1)]
    [string]$Manifest
)

$ErrorActionPreference = 'Stop'

$resolved = Resolve-Path -LiteralPath $Install -ErrorAction Stop
$Install = $resolved.Path

$manifestPath = Resolve-Path -LiteralPath $Manifest -ErrorAction Stop
$manifestPath = $manifestPath.Path

$ok = 0
$missing = [System.Collections.Generic.List[string]]::new()
$mismatched = [System.Collections.Generic.List[string]]::new()
$malformed = 0

foreach ($line in Get-Content -LiteralPath $manifestPath) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.TrimStart().StartsWith('#')) {
        continue
    }

    if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*(.+)$') {
        $malformed++
        continue
    }

    $expected = $matches[1].ToLowerInvariant()
    $relative = $matches[2].Trim().Replace('/', [IO.Path]::DirectorySeparatorChar)
    $path = Join-Path $Install $relative

    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $missing.Add($relative)
        continue
    }

    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        $mismatched.Add($relative)
    }
    else {
        $ok++
    }
}

Write-Output ''
Write-Output "Manifest : $manifestPath"
Write-Output "Install  : $Install"
Write-Output "Verified : $ok"
Write-Output "Missing  : $($missing.Count)"
Write-Output "Mismatch : $($mismatched.Count)"
Write-Output "Malformed: $malformed"

if ($missing.Count -gt 0) {
    Write-Output ''
    Write-Output 'Missing files:'
    $missing | ForEach-Object { Write-Output "  $_" }
}

if ($mismatched.Count -gt 0) {
    Write-Output ''
    Write-Output 'Mismatched files:'
    $mismatched | ForEach-Object { Write-Output "  $_" }
}

if ($missing.Count -gt 0 -or $mismatched.Count -gt 0 -or $malformed -gt 0) {
    exit 1
}
