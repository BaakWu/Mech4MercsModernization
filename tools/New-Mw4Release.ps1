[CmdletBinding()]
param(
    # Release version shown in the README and the zip name.
    # Default: the date plus the short commit id, e.g. 2026.11.16-95092d8.
    [string]$Version = '',
    # Skip rebuilding the hook DLLs (use what is in artifacts\hook).
    [switch]$NoBuild
)

# Builds the release zip: dinput.dll, ddraw.dll, mw4hook.ini and README.txt,
# to be copied into the game folder next to MW4Mercs.exe.
#   dist\MW4Mercs-WindowedPatch-<version>.zip
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not $Version) {
    $commit = (git rev-parse --short HEAD).Trim()
    $Version = '{0}-{1}' -f (Get-Date -Format 'yyyy.MM.dd'), $commit
}
if (-not $NoBuild) {
    pwsh (Join-Path $repo 'src\hook\build.ps1')
    if ($LASTEXITCODE) { throw 'Hook build failed.' }
}

$hook = Join-Path $repo 'artifacts\hook'
$files = 'dinput.dll', 'ddraw.dll', 'mw4hook.ini'
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $hook $f))) { throw "Missing build output: $f" }
}
# The shipped settings must not enable development features.
$ini = Get-Content (Join-Path $hook 'mw4hook.ini') -Raw
if ($ini -match '(?im)^\s*\[debug\]') { throw 'mw4hook.ini contains a [debug] section.' }

$name = "MW4Mercs-WindowedPatch-$Version"
$stage = Join-Path $repo "artifacts\release\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($f in $files) { Copy-Item (Join-Path $hook $f) $stage }

$hashes = foreach ($f in $files) {
    '  {0}  {1}' -f (Get-FileHash (Join-Path $stage $f) -Algorithm SHA256).Hash.ToLowerInvariant(), $f
}
$readme = (Get-Content (Join-Path $repo 'templates\release-README.txt') -Raw).
    Replace('{{VERSION}}', $Version).
    Replace('{{HASHES}}', ($hashes -join "`r`n"))
# CRLF so it reads correctly in Notepad.
$readme = ($readme -replace "`r?`n", "`r`n")
# Not "README.txt": the game folder already has one, and extracting the zip
# there would overwrite it.
$readmeName = 'WindowedPatch-README.txt'
[IO.File]::WriteAllText((Join-Path $stage $readmeName), $readme, [Text.UTF8Encoding]::new($false))

# Nothing in the zip may share a name with a file of the 3.1c distribution.
$manifest = Get-Content (Join-Path $repo 'manifests\mw4-mercs-3.1c-full.sha256') |
    Where-Object { $_ -and -not $_.StartsWith('#') } |
    ForEach-Object { ($_ -split '\s+', 2)[1].Trim().TrimStart('*').ToLowerInvariant() }
foreach ($f in $files + $readmeName) {
    if ($manifest -contains $f.ToLowerInvariant()) { throw "$f would overwrite a game file." }
}

$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# Files at the zip root, so extracting into the game folder puts them in place.
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

Write-Host "Release: $zip"
Write-Host ("SHA-256: {0}" -f (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant())
$hashes | ForEach-Object { Write-Host $_ }
