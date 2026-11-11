[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Install,
    [switch]$NoSound
)

# Development helper: start the game in windowed mode with the hook installed.
$ErrorActionPreference = 'Stop'

$installPath = [IO.Path]::GetFullPath($Install)
$exe = Join-Path $installPath 'MW4Mercs.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Game executable not found: $exe"
}
if (-not (Test-Path -LiteralPath (Join-Path $installPath 'ddraw.dll') -PathType Leaf)) {
    throw 'ddraw.dll hook is not installed. Run tools\Set-Mw4Hook.ps1 first.'
}

$arguments = @('-window')
if ($NoSound) {
    # Test-only: avoids the game's DirectSound CreateSoundBuffer failure while
    # exercising the window/display path.
    $arguments += '-nosound'
}

$process = Start-Process -FilePath $exe -ArgumentList $arguments `
    -WorkingDirectory $installPath -PassThru

Write-Output "Started MW4Mercs.exe $($arguments -join ' ') (pid $($process.Id))"
Write-Output 'Use tools\Get-Mw4WindowInfo.ps1 to verify the client area.'
