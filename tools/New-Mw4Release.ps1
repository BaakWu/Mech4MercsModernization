[CmdletBinding()]
param(
    # Skip rebuilding (use what is in artifacts\).
    [switch]$NoBuild
)

# Builds the release: the GUI installer with the hook embedded.
#   dist\MW4Mercs-ModernizationPatch-Setup-<version>.exe
# The version is kToolVersion in src\patcher\main.cpp (shown in the installer
# window and written to the install record).
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$main = Get-Content (Join-Path $repo 'src\patcher\main.cpp') -Raw
if ($main -notmatch 'kToolVersion\s*=\s*L"([^"]+)"') { throw 'kToolVersion not found in src\patcher\main.cpp' }
$version = $Matches[1]

if (-not $NoBuild) {
    pwsh (Join-Path $repo 'src\hook\build.ps1')
    if ($LASTEXITCODE) { throw 'Hook build failed.' }
    pwsh (Join-Path $repo 'src\patcher\build.ps1')
    if ($LASTEXITCODE) { throw 'Installer build failed.' }
}

# The shipped settings must not enable development features or opt-in
# behavior by default.
$ini = Get-Content (Join-Path $repo 'artifacts\hook\mw4hook.ini') -Raw
if ($ini -match '(?im)^\s*\[debug\]') { throw 'mw4hook.ini contains a [debug] section.' }
if ($ini -match '(?im)^\s*(mw5|decay|track_pointer|gyro)\s*=\s*1') { throw 'mw4hook.ini enables an opt-in feature by default.' }

$setup = Join-Path $repo 'artifacts\MW4Mercs-ModernizationPatch-Setup.exe'
if (-not (Test-Path $setup)) { throw "Missing build output: $setup" }
$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$out = Join-Path $dist "MW4Mercs-ModernizationPatch-Setup-$version.exe"
Copy-Item $setup $out -Force

Write-Host "Release: $out"
Write-Host ("SHA-256: {0}" -f (Get-FileHash $out -Algorithm SHA256).Hash.ToLowerInvariant())
foreach ($f in 'dinput.dll', 'ddraw.dll', 'mw4hook.ini') {
    Write-Host ('  {0}  {1} (embedded)' -f (Get-FileHash (Join-Path $repo "artifacts\hook\$f") -Algorithm SHA256).Hash.ToLowerInvariant(), $f)
}
