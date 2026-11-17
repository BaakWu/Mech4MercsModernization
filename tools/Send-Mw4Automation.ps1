[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Install,
    # Commands, one per element (see src/hook/automation.cpp).
    [Parameter(Mandatory = $true)][string[]]$Command,
    [int]$TimeoutSeconds = 60,
    # Directory that receives PNG copies of 'shot' frames.
    [string]$ShotDir = ''
)

# Test-only: drive the running game through the hook's in-process automation
# ([debug] automation=1 in mw4hook.ini). Nothing touches the real cursor,
# keyboard focus, or the desktop.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$installPath = [IO.Path]::GetFullPath($Install)
# The game process whose hook log exists (the relaunched one), newest first.
# The window handle is not used: from another Windows session (e.g. SSH) a
# process's MainWindowHandle reads 0.
$proc = Get-Process MW4Mercs -ErrorAction Stop |
    Where-Object { Test-Path -LiteralPath (Join-Path $installPath "mw4hook-$($_.Id).log") } |
    Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $proc) { throw 'No MW4Mercs process with a hook log is running.' }
$log = Join-Path $installPath "mw4hook-$($proc.Id).log"
if (-not (Test-Path -LiteralPath $log)) { throw "Hook log not found: $log" }

$before = (Get-Content -LiteralPath $log | Select-String -SimpleMatch 'auto: batch done').Count
$cmd = Join-Path $installPath 'mw4hook-cmd.txt'
$tmp = "$cmd.tmp"
Set-Content -LiteralPath $tmp -Value ($Command -join "`r`n") -Encoding ascii
Move-Item -LiteralPath $tmp -Destination $cmd -Force

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    $done = (Get-Content -LiteralPath $log | Select-String -SimpleMatch 'auto: batch done').Count
    if ($done -gt $before) { break }
}
if ((Get-Date) -ge $deadline) { throw 'Timed out waiting for the automation batch.' }

if ($ShotDir) {
    New-Item -ItemType Directory -Path $ShotDir -Force | Out-Null
    foreach ($line in $Command) {
        if ($line -match '^\s*shot\s+(\S+)') {
            $bmp = Join-Path $installPath $Matches[1]
            if (Test-Path -LiteralPath $bmp) {
                $png = Join-Path $ShotDir ([IO.Path]::ChangeExtension($Matches[1], '.png'))
                $image = [System.Drawing.Image]::FromFile($bmp)
                $image.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
                $image.Dispose()
                Remove-Item -LiteralPath $bmp
                Write-Output "shot: $png"
            } else {
                Write-Output "shot missing: $bmp"
            }
        }
    }
}
