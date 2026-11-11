[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$Install,
    [string]$HookDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts\hook'),
    [switch]$Uninstall
)

# Development helper: copy the built dinput/ddraw proxies and their ini into a
# test install, or remove them again. The final release will do this from the
# patcher executable.
$ErrorActionPreference = 'Stop'

$installPath = [IO.Path]::GetFullPath($Install)
$backupDir = Join-Path $installPath '.mw4hook-backup'

function Backup-File([string]$name) {
    $target = Join-Path $installPath $name
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        return
    }
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $backup = Join-Path $backupDir $name
    if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
        if ($PSCmdlet.ShouldProcess($target, 'Back up existing file')) {
            Copy-Item -LiteralPath $target -Destination $backup -Force
        }
    }
}

if ($Uninstall) {
    foreach ($name in 'dinput.dll', 'ddraw.dll', 'mw4hook.ini') {
        $target = Join-Path $installPath $name
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            if ($PSCmdlet.ShouldProcess($target, 'Remove hook file')) {
                Remove-Item -LiteralPath $target -Force
            }
        }
        $backup = Join-Path $backupDir $name
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            if ($PSCmdlet.ShouldProcess($target, 'Restore backup')) {
                Move-Item -LiteralPath $backup -Destination $target -Force
            }
        }
    }
    if (Test-Path -LiteralPath $backupDir -PathType Container) {
        Remove-Item -LiteralPath $backupDir -Force -Recurse
    }
    Write-Output 'mw4hook removed from the install.'
    return
}

$dinput = Join-Path $HookDir 'dinput.dll'
$ddraw = Join-Path $HookDir 'ddraw.dll'
$ini = Join-Path $HookDir 'mw4hook.ini'
if (-not (Test-Path -LiteralPath $dinput -PathType Leaf)) {
    throw "Hook DLL not found: $dinput. Run src/hook/build.ps1 first."
}
if (-not (Test-Path -LiteralPath $ddraw -PathType Leaf)) {
    throw "Frontend scaler not found: $ddraw. Run src/hook/build.ps1 first."
}
if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) {
    throw "Hook config not found: $ini"
}

Backup-File 'dinput.dll'
Backup-File 'ddraw.dll'
Backup-File 'mw4hook.ini'

if ($PSCmdlet.ShouldProcess($installPath, 'Install mw4hook')) {
    Copy-Item -LiteralPath $dinput -Destination (Join-Path $installPath 'dinput.dll') -Force
    Copy-Item -LiteralPath $ddraw -Destination (Join-Path $installPath 'ddraw.dll') -Force
    Copy-Item -LiteralPath $ini -Destination (Join-Path $installPath 'mw4hook.ini') -Force
}

Write-Output "mw4hook installed in: $installPath"
Write-Output 'Launch with:  Start-Process .\MW4Mercs.exe -ArgumentList "-window" -WorkingDirectory <install>'
