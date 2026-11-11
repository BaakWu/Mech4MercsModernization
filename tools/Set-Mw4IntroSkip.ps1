[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$Install,
    [switch]$Restore
)

# Test-only helper: replace the startup FMV sequence with the game's own tiny
# CEmpty.mpg placeholder. This skips the long intro without deleting files, so
# the movie player never sees a missing asset. Originals are kept beside the
# files as *.fmv-orig and can be restored.
$ErrorActionPreference = 'Stop'

$installPath = [IO.Path]::GetFullPath($Install)
$movies = Join-Path $installPath 'Content\Movies'
$marker = Join-Path $movies '.fmv-intro-skip.json'

$introFiles = @(
    'mektek_mekpak31_logo.mpeg',
    'cybrlogo.mpg',
    'fasalogo.mpg',
    'mgslogo.mpg',
    'wklogo.mpg',
    'mektek.mpg',
    'Gameopen.mpg'
)

if (-not (Test-Path -LiteralPath $movies -PathType Container)) {
    throw "Movies directory not found: $movies"
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        Write-Output 'No intro-skip record found; nothing to restore.'
        return
    }
    $record = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
    foreach ($name in $record.files) {
        $backup = Join-Path $movies ($name + '.fmv-orig')
        $target = Join-Path $movies $name
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            if ($PSCmdlet.ShouldProcess($target, 'Restore intro movie')) {
                Move-Item -LiteralPath $backup -Destination $target -Force
            }
        }
    }
    Remove-Item -LiteralPath $marker -Force
    Write-Output "Restored $($record.files.Count) intro movie(s)."
    return
}

$placeholder = Join-Path $movies 'CEmpty.mpg'
if (-not (Test-Path -LiteralPath $placeholder -PathType Leaf)) {
    throw "Placeholder movie not found: $placeholder"
}

$replaced = @()
if (Test-Path -LiteralPath $marker -PathType Leaf) {
    $existing = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
    $replaced += @($existing.files)
}
foreach ($name in $introFiles) {
    if ($replaced -contains $name) {
        continue
    }
    $target = Join-Path $movies $name
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        continue
    }
    $backup = Join-Path $movies ($name + '.fmv-orig')
    if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
        if ($PSCmdlet.ShouldProcess($target, 'Back up intro movie')) {
            Copy-Item -LiteralPath $target -Destination $backup -Force
        }
    }
    if ($PSCmdlet.ShouldProcess($target, 'Replace intro movie with CEmpty.mpg')) {
        Copy-Item -LiteralPath $placeholder -Destination $target -Force
    }
    $replaced += $name
}

if ($replaced.Count -eq 0) {
    Write-Output 'No intro movies were replaced.'
    return
}

[pscustomobject]@{
    install = $installPath
    created = (Get-Date).ToString('o')
    files   = $replaced
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $marker -Encoding utf8

Write-Output "Replaced $($replaced.Count) intro movie(s) with CEmpty.mpg."
Write-Output 'Run with -Restore to undo.'
