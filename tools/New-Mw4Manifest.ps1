[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Install,

    [Parameter(Mandatory = $true, Position = 1)]
    [string]$Output,

    [switch]$IncludeMutable
)

$ErrorActionPreference = 'Stop'

$resolved = Resolve-Path -LiteralPath $Install -ErrorAction Stop
$Install = $resolved.Path

$exe = Join-Path $Install 'MW4Mercs.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "MW4Mercs.exe was not found under '$Install'."
}

$outputFull = [IO.Path]::GetFullPath($Output)
$outputDir = Split-Path -Parent $outputFull
if ($outputDir -and -not (Test-Path -LiteralPath $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$excludedFiles = @(
    'options.ini',
    'playerLog.txt'
)

$excludedDirs = @(
    'screenshots',
    'RESOURCE/Pilots',
    'RESOURCE/UserMissions',
    'Stats'
)

$exeItem = Get-Item -LiteralPath $exe
$exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$version = $exeItem.VersionInfo.FileVersion

$files = Get-ChildItem -LiteralPath $Install -Recurse -File -ErrorAction Stop
$entries = [System.Collections.Generic.List[string]]::new()
$skipped = 0

foreach ($file in $files) {
    $relative = [IO.Path]::GetRelativePath($Install, $file.FullName).Replace('\', '/')

    if (-not $IncludeMutable) {
        if ($excludedFiles -contains $relative) {
            $skipped++
            continue
        }

        $skip = $false
        foreach ($dir in $excludedDirs) {
            if ($relative -eq $dir -or $relative.StartsWith("$dir/", [StringComparison]::OrdinalIgnoreCase)) {
                $skip = $true
                break
            }
        }

        if ($skip) {
            $skipped++
            continue
        }

        if ($file.Name -match '\.(log|bak)$' -or $file.Name -like '*.bak-*') {
            $skipped++
            continue
        }
    }

    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $entries.Add(('{0} *{1}' -f $hash, $relative))
}

$header = @(
    '# MW4 Mercs manifest',
    "# Install: $Install",
    "# Exe version: $version",
    "# Exe SHA256: $exeHash",
    "# Generated: $(Get-Date -Format o)",
    "# Files: $($entries.Count)",
    "# Excluded mutable files: $skipped",
    ''
)

Set-Content -LiteralPath $outputFull -Value ($header + $entries) -Encoding ascii
Write-Output "Wrote $outputFull ($($entries.Count) files, $skipped excluded)."
