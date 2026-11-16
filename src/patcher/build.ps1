# Build mw4patch.exe with the hook DLL, config, core manifest, and options
# template embedded as resources.
param(
    [string]$Configuration = 'Release',
    [string]$OutDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts')
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$patcherSrc = $PSScriptRoot
$hookBuild = Join-Path $repo 'src\hook\build.ps1'
$hookDll = Join-Path $repo 'artifacts\hook\dinput.dll'
$ddrawDll = Join-Path $repo 'artifacts\hook\ddraw.dll'
$hookIni = Join-Path $repo 'artifacts\hook\mw4hook.ini'
$manifest = Join-Path $repo 'manifests\mw4-mercs-3.1c-core.sha256'
$optionsTemplate = Join-Path $repo 'templates\options.ini'

$vcvars = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if (-not $vcvars) {
    throw 'Visual Studio 2022 x86 build tools were not found.'
}

if (-not (Test-Path -LiteralPath $hookDll -PathType Leaf)) {
    Write-Output 'Hook DLL not built yet; building it first...'
    & pwsh -NoProfile -File $hookBuild
    if ($LASTEXITCODE -ne 0) {
        throw "Hook build failed with exit code $LASTEXITCODE."
    }
}
if (-not (Test-Path -LiteralPath $hookDll -PathType Leaf)) {
    throw "Hook DLL not found: $hookDll"
}
if (-not (Test-Path -LiteralPath $ddrawDll -PathType Leaf)) {
    Write-Output 'Frontend scaler not built yet; building it first...'
    & pwsh -NoProfile -File $hookBuild
    if ($LASTEXITCODE -ne 0) {
        throw "Hook build failed with exit code $LASTEXITCODE."
    }
}
if (-not (Test-Path -LiteralPath $ddrawDll -PathType Leaf)) {
    throw "Frontend scaler not found: $ddrawDll"
}

$payload = Join-Path $OutDir 'patcher\payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Copy-Item -LiteralPath $hookDll -Destination (Join-Path $payload 'dinput.dll') -Force
Copy-Item -LiteralPath $ddrawDll -Destination (Join-Path $payload 'ddraw.dll') -Force
Copy-Item -LiteralPath $hookIni -Destination (Join-Path $payload 'mw4hook.ini') -Force
Copy-Item -LiteralPath $manifest -Destination (Join-Path $payload 'core.sha256') -Force
Copy-Item -LiteralPath $optionsTemplate -Destination (Join-Path $payload 'options.ini') -Force

function To-RcPath([string]$path) {
    return ([IO.Path]::GetFullPath($path)).Replace('\', '/')
}

$rcPath = Join-Path $OutDir 'patcher\resources.rc'
$rc = @(
    '#include "resource.h"',
    ('IDR_HOOK_DLL RCDATA "{0}"' -f (To-RcPath (Join-Path $payload 'dinput.dll'))),
    ('IDR_DDRAW_DLL RCDATA "{0}"' -f (To-RcPath (Join-Path $payload 'ddraw.dll'))),
    ('IDR_HOOK_INI RCDATA "{0}"' -f (To-RcPath (Join-Path $payload 'mw4hook.ini'))),
    ('IDR_CORE_MANIFEST RCDATA "{0}"' -f (To-RcPath (Join-Path $payload 'core.sha256'))),
    ('IDR_OPTIONS_TEMPLATE RCDATA "{0}"' -f (To-RcPath (Join-Path $payload 'options.ini')))
) -join "`r`n"
Set-Content -LiteralPath $rcPath -Value $rc -Encoding ascii

$objDir = Join-Path $OutDir 'patcher\obj'
New-Item -ItemType Directory -Path $objDir -Force | Out-Null
$exe = Join-Path $OutDir 'mw4patch.exe'
$resFile = Join-Path $objDir 'resources.res'

$rcCommand = "rc /nologo /I `"$patcherSrc`" /fo `"$resFile`" `"$rcPath`""
$rcOutput = cmd /c "call `"$vcvars`" x86 >nul && $rcCommand" 2>&1
$rcOutput | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    throw "Resource compilation failed with exit code $LASTEXITCODE."
}

$optFlags = if ($Configuration -eq 'Debug') { '/Od /Zi /RTC1' } else { '/O2' }
$command = @(
    'cl /nologo /MT /W4 /WX- /EHsc /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601'
    $optFlags
    "/I `"$patcherSrc`""
    "/Fo:`"$objDir\\`""
    "/Fd:`"$objDir\mw4patch.pdb`""
    "`"$patcherSrc\main.cpp`""
    "`"$resFile`""
    "/Fe:`"$exe`""
    '/link advapi32.lib ole32.lib uuid.lib shell32.lib user32.lib /SUBSYSTEM:CONSOLE'
    # Embed a manifest that requests no elevation. Without one, Windows'
    # installer detection sees "patch" in the name and forces a UAC prompt.
    "/MANIFEST:EMBED /MANIFESTUAC:`"level='asInvoker' uiAccess='false'`""
) -join ' '

$full = "call `"$vcvars`" x86 >nul && $command"
$output = cmd /c $full 2>&1
$output | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    throw "mw4patch build failed with exit code $LASTEXITCODE."
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Build did not produce $exe"
}

# GUI installer: same source, MW4PATCH_GUI, Windows subsystem, visual styles.
$setup = Join-Path $OutDir 'MW4Mercs-WindowedPatch-Setup.exe'
$guiObj = Join-Path $OutDir 'patcher\obj-gui'
New-Item -ItemType Directory -Path $guiObj -Force | Out-Null
$guiCommand = @(
    'cl /nologo /MT /W4 /WX- /EHsc /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /DMW4PATCH_GUI'
    $optFlags
    "/I `"$patcherSrc`""
    "/Fo:`"$guiObj\\`""
    "/Fd:`"$guiObj\setup.pdb`""
    "`"$patcherSrc\main.cpp`""
    "`"$resFile`""
    "/Fe:`"$setup`""
    '/link advapi32.lib ole32.lib uuid.lib shell32.lib user32.lib gdi32.lib comctl32.lib /SUBSYSTEM:WINDOWS'
    "/MANIFEST:EMBED /MANIFESTUAC:`"level='asInvoker' uiAccess='false'`""
    "`"/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'`""
) -join ' '
$output = cmd /c "call `"$vcvars`" x86 >nul && $guiCommand" 2>&1
$output | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    throw "Setup build failed with exit code $LASTEXITCODE."
}

Write-Output ''
Write-Output "Built: $setup"
Write-Output ("SHA-256: " + (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant())
Write-Output ''
Write-Output "Built: $exe"
Write-Output ("Size : " + (Get-Item -LiteralPath $exe).Length + " bytes")
Write-Output ("SHA-256: " + (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant())

# Publish a copy to a stable, easy-to-find location.
$dist = Join-Path $repo 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$published = Join-Path $dist 'mw4patch.exe'
Copy-Item -LiteralPath $exe -Destination $published -Force
Write-Output "Published: $published"
