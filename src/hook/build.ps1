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
    'automation.cpp',
    'controls.cpp',
    'aim.cpp',
    'camo.cpp',
    'sound.cpp',
    'campaign.cpp',
    'cursor.cpp',
    'detour.cpp',
    'dinput_proxy.cpp',
    'display.cpp',
    'dllmain.cpp',
    'iat.cpp',
    'log.cpp',
    'shellscript.cpp',
    'window.cpp'
) | ForEach-Object { Join-Path $src $_ }

$def = Join-Path $src 'dinput.def'
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
    "/Fe:`"$OutDir\dinput.dll`""
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

$dll = Join-Path $OutDir 'dinput.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "Build did not produce $dll"
}

# ddraw proxy (frontend stretch)
$ddrawSources = @('ddraw_proxy.cpp', 'menu_scaler.cpp') | ForEach-Object { Join-Path $src $_ }
$ddrawDef = Join-Path $src 'ddraw.def'
$ddrawObjDir = Join-Path $OutDir 'obj-ddraw'
New-Item -ItemType Directory -Path $ddrawObjDir -Force | Out-Null
$ddrawCompile = @(
    'cl /nologo /LD /W4 /WX- /EHsc /GR- /GS- /Gy /utf-8 /DUNICODE /D_UNICODE'
    $optFlags
    "/Fo:`"$ddrawObjDir\\`""
    "/Fd:`"$ddrawObjDir\ddraw.pdb`""
    ($ddrawSources -join ' ')
    "/Fe:`"$OutDir\ddraw.dll`""
    '/link /MACHINE:X86'
    "/DEF:`"$ddrawDef`""
    'user32.lib gdi32.lib /SUBSYSTEM:WINDOWS /RELEASE'
) -join ' '
$ddrawCommand = "call `"$vcvars`" x86 >nul && $ddrawCompile"
$ddrawOut = cmd /c $ddrawCommand 2>&1
$ddrawOut | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    throw "ddraw proxy build failed with exit code $LASTEXITCODE."
}
$ddrawDll = Join-Path $OutDir 'ddraw.dll'
if (-not (Test-Path -LiteralPath $ddrawDll -PathType Leaf)) {
    throw "Build did not produce $ddrawDll"
}

$iniSource = Join-Path $src 'mw4hook.ini'
$iniTarget = Join-Path $OutDir 'mw4hook.ini'
if (-not (Test-Path -LiteralPath $iniSource -PathType Leaf)) {
    throw "Hook configuration not found: $iniSource"
}
Copy-Item -LiteralPath $iniSource -Destination $iniTarget -Force

Write-Output ''
Write-Output "Built: $dll"
Write-Output ("SHA-256: " + (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant())
Write-Output "Built: $ddrawDll"
Write-Output ("SHA-256: " + (Get-FileHash -LiteralPath $ddrawDll -Algorithm SHA256).Hash.ToLowerInvariant())
Write-Output "Config: $iniTarget"
