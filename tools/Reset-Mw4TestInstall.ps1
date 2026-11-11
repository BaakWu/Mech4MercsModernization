[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$RarPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'MekTek.rar'),

    [string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) 'MekTek'),

    [string]$SevenZipPath = '',

    [switch]$Force,

    [switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')
$repoPrefix = $repoFull + '\'

$RarPath = [IO.Path]::GetFullPath($RarPath)
$Destination = [IO.Path]::GetFullPath($Destination)

# This archive is the immutable source for every test install.
$expectedArchiveSha256 = '8c8a2c192834a597862cce19e4215d563f93de46872eff8fd1980d47bb417a34'
$expectedExeSha256 = '72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41'

if (-not (Test-Path -LiteralPath $RarPath -PathType Leaf)) {
    throw "Pristine archive not found: $RarPath"
}

if ($Destination.TrimEnd('\') -eq $repoFull) {
    throw 'Destination must not be the repository root.'
}

if (-not $Destination.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Destination must be inside the repository: $repoFull"
}

if (-not $SevenZipPath) {
    $candidates = @(
        'C:\Program Files\7-Zip\7z.exe',
        'C:\Program Files (x86)\7-Zip\7z.exe'
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $SevenZipPath = $candidate
            break
        }
    }
}

if (-not $SevenZipPath -or -not (Test-Path -LiteralPath $SevenZipPath -PathType Leaf)) {
    throw '7-Zip was not found. Pass -SevenZipPath "C:\path\to\7z.exe".'
}

$beforeHash = (Get-FileHash -LiteralPath $RarPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($beforeHash -ne $expectedArchiveSha256) {
    throw @"
MekTek.rar hash mismatch.
Expected: $expectedArchiveSha256
Actual:   $beforeHash
Refusing to use a modified or unexpected archive.
"@
}

if ((Test-Path -LiteralPath $Destination) -and -not $Force) {
    throw "Destination already exists: $Destination. Re-run with -Force to delete and re-extract."
}

if (-not $PSCmdlet.ShouldProcess($Destination, 'Delete and re-extract pristine test install')) {
    return
}

$artifacts = Join-Path $repo 'artifacts'
if (-not (Test-Path -LiteralPath $artifacts)) {
    New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
}

$staging = Join-Path $artifacts ('reset-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging -Force | Out-Null

try {
    if (Test-Path -LiteralPath $Destination) {
        Write-Output "Removing existing extracted install: $Destination"
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }

    Write-Output "Extracting $RarPath to staging..."
    $output = & $SevenZipPath x -y ("-o$staging") $RarPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | Select-Object -Last 40 | Out-Host
        throw "7-Zip extraction failed with exit code $LASTEXITCODE."
    }

    $extractedRoot = Join-Path $staging 'MekTek'
    if (-not (Test-Path -LiteralPath $extractedRoot -PathType Container)) {
        throw "The archive did not contain the expected top-level 'MekTek' folder."
    }

    Move-Item -LiteralPath $extractedRoot -Destination $Destination

    $install = Join-Path $Destination 'Mechwarrior Mercenaries - Mektek Mekpak'
    $exe = Join-Path $install 'MW4Mercs.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "Extracted install does not contain MW4Mercs.exe: $install"
    }

    $exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($exeHash -ne $expectedExeSha256) {
        throw @"
The extracted MW4Mercs.exe does not match the supported 3.1c baseline.
Expected: $expectedExeSha256
Actual:   $exeHash
"@
    }

    $testScript = Join-Path $PSScriptRoot 'Test-Mw4Manifest.ps1'
    $manifest = Join-Path $repo 'manifests\mw4-mercs-3.1c-core.sha256'
    $pwshPath = Join-Path $PSHOME 'pwsh.exe'
    if (-not (Test-Path -LiteralPath $pwshPath -PathType Leaf)) {
        throw "pwsh.exe was not found at $pwshPath. PowerShell 7 is required."
    }

    Write-Output 'Verifying extracted install against the core manifest...'
    $argLine = "-NoProfile -File `"$testScript`" -Install `"$install`" -Manifest `"$manifest`""
    $verify = Start-Process -FilePath $pwshPath -ArgumentList $argLine -Wait -PassThru -NoNewWindow
    if ($verify.ExitCode -ne 0) {
        throw "Extracted install failed core manifest verification (exit code $($verify.ExitCode))."
    }

    $afterHash = (Get-FileHash -LiteralPath $RarPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($afterHash -ne $beforeHash) {
        throw "MekTek.rar changed during extraction. Expected $beforeHash, got $afterHash."
    }

    Write-Output ''
    Write-Output 'Pristine test install reset complete.'
    Write-Output "Archive  : $RarPath"
    Write-Output "Install  : $install"
    Write-Output "Archive  : unchanged ($afterHash)"
}
finally {
    if (-not $KeepStaging -and (Test-Path -LiteralPath $staging)) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
