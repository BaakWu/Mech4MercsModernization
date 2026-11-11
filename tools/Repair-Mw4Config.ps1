[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Install,

    [ValidateRange(640, 16384)]
    [int]$Width = 1920,

    [ValidateRange(480, 16384)]
    [int]$Height = 1080,

    [ValidateSet(16, 32)]
    [int]$BitDepth = 32,

    [ValidateRange(0, 32)]
    [int]$VideoDriverIndex = 0,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$resolved = Resolve-Path -LiteralPath $Install -ErrorAction Stop
$Install = $resolved.Path

$exe = Join-Path $Install 'MW4Mercs.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "MW4Mercs.exe was not found under '$Install'."
}

$templatePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'templates/options.ini'
if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
    throw "options.ini template not found at '$templatePath'."
}

$optionsPath = Join-Path $Install 'options.ini'

if ((Test-Path -LiteralPath $optionsPath -PathType Leaf) -and -not $Force) {
    $existing = Get-Content -LiteralPath $optionsPath -Raw
    $looksValid = (
        $existing -match '(?m)^\s*screenwidth\s*=\s*[1-9][0-9]*\s*$' -and
        $existing -match '(?m)^\s*screenheight\s*=\s*[1-9][0-9]*\s*$' -and
        $existing -match '(?m)^\s*bitdepth\s*=\s*(16|32)\s*$' -and
        $existing -match '(?m)^\s*videodriverindex\s*=\s*[0-9]+\s*$'
    )

    if ($looksValid) {
        Write-Output "options.ini already looks valid. Use -Force to rewrite it."
        return
    }
}

if ($PSCmdlet.ShouldProcess($optionsPath, 'Write known-good options.ini')) {
    if (Test-Path -LiteralPath $optionsPath -PathType Leaf) {
        $backupName = 'options.ini.bak-{0}' -f (Get-Date -Format 'yyyyMMdd-HHmmss')
        $backupPath = Join-Path $Install $backupName
        Copy-Item -LiteralPath $optionsPath -Destination $backupPath -Force
        Write-Output "Backed up existing options.ini to $backupPath"
    }

    $template = Get-Content -LiteralPath $templatePath -Raw
    $content = $template.
        Replace('{{WIDTH}}', [string]$Width).
        Replace('{{HEIGHT}}', [string]$Height).
        Replace('{{BITDEPTH}}', [string]$BitDepth).
        Replace('{{VIDEODRIVERINDEX}}', [string]$VideoDriverIndex)

    Set-Content -LiteralPath $optionsPath -Value $content -Encoding ascii
    Write-Output "Wrote $optionsPath ($Width x $Height, $BitDepth-bit, driver $VideoDriverIndex)."
}
