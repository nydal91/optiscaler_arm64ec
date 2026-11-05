# Build FreeType 2.13.3 as a static library without optional dependencies
# (no zlib, bzip2, png, harfbuzz or brotli), matching the vendored headers in
# OptiScaler\external\freetype.
#
# The vendored external\freetype tree contains headers plus a prebuilt x64
# freetype.lib only, so the 2.13.3 sources are fetched on demand.
#
# ARM64EC output is staged into <repo>\arm64ec\libs\freetype\freetype.lib.

[CmdletBinding()]
param(
    [ValidateSet('arm64ec', 'x64')][string]$Arch = 'arm64ec',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [string]$BuildRoot = 'C:\Users\eirik\Documents\opti-build-src',
    [string]$OutRoot,
    [string]$VSPath,
    [switch]$Clean,
    [int]$MaxCpu = 4,
    [switch]$SkipToolchainCheck
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$ctx = Initialize-DepsEnvironment -Name 'freetype' -Arch $Arch -Configuration $Configuration `
    -BuildRoot $BuildRoot -OutRoot $OutRoot -VSPath $VSPath -Clean:$Clean `
    -MaxCpu $MaxCpu -SkipToolchainCheck:$SkipToolchainCheck

$cmakeLists = Join-Path $ctx.SourceFreetype 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $cmakeLists)) {
    Write-Log "FreeType sources not found at $($ctx.SourceFreetype); cloning VER-2-13-3" $ctx.LogFile
    Invoke-GitClone -Repository 'https://gitlab.freedesktop.org/freetype/freetype.git' `
        -Destination $ctx.SourceFreetype -Branch 'VER-2-13-3' -LogFile $ctx.LogFile
}
Assert-File -Path $cmakeLists -What 'FreeType CMakeLists.txt'

$versionText = [System.IO.File]::ReadAllText($cmakeLists)
if ($versionText -notmatch 'set\(VERSION_MAJOR "2"\)' -or
    $versionText -notmatch 'set\(VERSION_MINOR "13"\)' -or
    $versionText -notmatch 'set\(VERSION_PATCH "3"\)') {
    throw "FreeType sources at $($ctx.SourceFreetype) are not version 2.13.3 (expected VERSION 2.13.3)."
}

$buildDir = Join-Path $BuildRoot ("build-freetype-{0}" -f $Arch)
if ($Clean) {
    Write-Log "Cleaning $buildDir" $ctx.LogFile
    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
}

$cacheArgs = @(
    '-DBUILD_SHARED_LIBS=OFF',
    '-DFT_DISABLE_ZLIB=TRUE',
    '-DFT_DISABLE_BZIP2=TRUE',
    '-DFT_DISABLE_PNG=TRUE',
    '-DFT_DISABLE_HARFBUZZ=TRUE',
    '-DFT_DISABLE_BROTLI=TRUE'
)
Invoke-CMakeConfigure -Ctx $ctx -SourceDir $ctx.SourceFreetype -BuildDir $buildDir -CacheArgs $cacheArgs
Invoke-CMakeBuild -Ctx $ctx -BuildDir $buildDir -Targets @('freetype')

$built = Find-BuiltLib -PreferredPath (Join-Path $buildDir "$Configuration\freetype.lib") `
    -BuildDir $buildDir -Configuration $Configuration -Name 'freetype.lib' -LogFile $ctx.LogFile

$stageDir = Join-Path $ctx.OutRoot 'freetype'
$stagedLib = Join-Path $stageDir 'freetype.lib'
Copy-StagedLib -Source $built -Destination $stagedLib -LogFile $ctx.LogFile
Assert-LibrarySetArchitecture -Paths @($stagedLib) -Ctx $ctx

Write-Log "FreeType 2.13.3 build complete. Output staged in $stageDir" $ctx.LogFile
