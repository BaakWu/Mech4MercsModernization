# Build mw4hook (version.dll proxy) for 32-bit Windows.
#
# Requires Visual Studio 2022 C++ build tools. The script locates vcvarsall.bat
# and compiles from a clean output directory.
param(
    [string]$Configuration = 'Release',
    [string]$OutDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts\hook')
)

$ErrorActionPreference = 'Stop'

$src = $PSScriptRoot
if (-not (Test-Path -LiteralPath $src -PathType Container)) {
    throw "Hook sources not found: $src"
}

$vcvars = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if (-not $vcvars) {
    throw 'Visual Studio 2022 x86 build tools were not found.'
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir = [IO.Path]::GetFullPath($OutDir)

$sources = @(
    'ddraw_proxy.cpp',
    'dllmain.cpp',
    'iat.cpp',
    'log.cpp',
    'resolution.cpp',
    'window.cpp'
) | ForEach-Object { Join-Path $src $_ }

$def = Join-Path $src 'ddraw.def'
$objDir = Join-Path $OutDir 'obj'
New-Item -ItemType Directory -Path $objDir -Force | Out-Null

$optFlags = if ($Configuration -eq 'Debug') {
    '/Od /Zi /RTC1'
} else {
    '/O2 /GL'
}

$compile = @(
    'cl /nologo /LD /W4 /WX- /EHsc /GR- /GS- /Gy /utf-8 /DUNICODE /D_UNICODE'
    $optFlags
    "/Fo:`"$objDir\\`""
    "/Fd:`"$objDir\mw4hook.pdb`""
    ($sources -join ' ')
    "/Fe:`"$OutDir\ddraw.dll`""
    '/link /MACHINE:X86'
    "/DEF:`"$def`""
    'user32.lib advapi32.lib /SUBSYSTEM:WINDOWS /RELEASE'
) -join ' '

$command = "call `"$vcvars`" x86 >nul && $compile"

$out = cmd /c $command 2>&1
$out | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    throw "mw4hook build failed with exit code $LASTEXITCODE."
}

$dll = Join-Path $OutDir 'ddraw.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "Build did not produce $dll"
}

Write-Output ''
Write-Output "Built: $dll"
Write-Output ("SHA-256: " + (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant())
