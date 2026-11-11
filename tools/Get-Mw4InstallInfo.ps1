[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Install,

    [switch]$Json
)

$ErrorActionPreference = 'Stop'

$resolved = Resolve-Path -LiteralPath $Install -ErrorAction Stop
$Install = $resolved.Path

$exe = Join-Path $Install 'MW4Mercs.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "MW4Mercs.exe was not found under '$Install'."
}

function Get-IniValue {
    param(
        [hashtable]$Ini,
        [string]$Key
    )

    if ($Ini.ContainsKey($Key)) {
        return $Ini[$Key]
    }

    return $null
}

function ConvertTo-IniInt {
    param(
        [hashtable]$Ini,
        [string]$Key,
        [int]$Default = 0
    )

    $raw = Get-IniValue -Ini $Ini -Key $Key
    $value = 0

    if ($null -ne $raw -and [int]::TryParse([string]$raw, [ref]$value)) {
        return $value
    }

    return $Default
}

$exeItem = Get-Item -LiteralPath $exe
$version = $exeItem.VersionInfo.FileVersion
$sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()

$knownBuilds = @{
    '72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41' = 'MekTek 3.1c (ModDB canonical)'
    '48bda0136af669efe017f2679e0c6108c3f8b72fe371f6b2440f22fde735a8b5' = 'MekTek 3.1c (MTX-chain install; non-canonical)'
}

$build = if ($knownBuilds.ContainsKey($sha256)) {
    $knownBuilds[$sha256]
}
else {
    'Unknown build'
}

$optionsPath = Join-Path $Install 'options.ini'
$optionsPresent = Test-Path -LiteralPath $optionsPath -PathType Leaf
$options = @{}
$width = 0
$height = 0
$bitDepth = 0
$videoDriverIndex = -1
$optionsValid = $false

if ($optionsPresent) {
    foreach ($line in Get-Content -LiteralPath $optionsPath) {
        if ($line -match '^\s*([^;#=]+?)\s*=\s*(.*?)\s*$') {
            $options[$matches[1].Trim().ToLowerInvariant()] = $matches[2].Trim()
        }
    }

    $width = ConvertTo-IniInt -Ini $options -Key 'screenwidth'
    $height = ConvertTo-IniInt -Ini $options -Key 'screenheight'
    $bitDepth = ConvertTo-IniInt -Ini $options -Key 'bitdepth'
    $videoDriverIndex = ConvertTo-IniInt -Ini $options -Key 'videodriverindex' -Default -1

    $optionsValid = (
        $width -gt 0 -and
        $height -gt 0 -and
        ($bitDepth -eq 16 -or $bitDepth -eq 32) -and
        $videoDriverIndex -ge 0
    )
}

$core = Join-Path $Install 'RESOURCE/core.mw4'
$props = Join-Path $Install 'RESOURCE/props.mw4'
$textures = Join-Path $Install 'RESOURCE/textures.mw4'

$result = [pscustomobject]@{
    Install          = $Install
    Exe              = $exe
    Version          = $version
    Sha256           = $sha256
    KnownBuild       = $build
    OptionsIni       = [pscustomobject]@{
        Path             = $optionsPath
        Present          = $optionsPresent
        Valid            = $optionsValid
        Width            = $width
        Height           = $height
        BitDepth         = $bitDepth
        VideoDriverIndex = $videoDriverIndex
    }
    ResourceArchives = [pscustomobject]@{
        Core     = Test-Path -LiteralPath $core -PathType Leaf
        Props    = Test-Path -LiteralPath $props -PathType Leaf
        Textures = Test-Path -LiteralPath $textures -PathType Leaf
    }
}

if ($Json) {
    $result | ConvertTo-Json -Depth 6
    return
}

Write-Output ''
Write-Output "Install          : $($result.Install)"
Write-Output "Executable       : $($result.Exe)"
Write-Output "Version          : $($result.Version)"
Write-Output "SHA-256          : $($result.Sha256)"
Write-Output "Known build      : $($result.KnownBuild)"
Write-Output ''
Write-Output 'options.ini'
Write-Output "  Present        : $($result.OptionsIni.Present)"
Write-Output "  Valid          : $($result.OptionsIni.Valid)"
Write-Output "  Resolution     : $($result.OptionsIni.Width)x$($result.OptionsIni.Height)"
Write-Output "  Bit depth      : $($result.OptionsIni.BitDepth)"
Write-Output "  Video driver   : $($result.OptionsIni.VideoDriverIndex)"
Write-Output ''
Write-Output 'Resource archives'
Write-Output "  core.mw4       : $($result.ResourceArchives.Core)"
Write-Output "  props.mw4      : $($result.ResourceArchives.Props)"
Write-Output "  textures.mw4   : $($result.ResourceArchives.Textures)"
