# Build FidelityFX FSR2 2.2.1 (DX11 fork) API + DX11/DX12/VK backends as static libs.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build-fsr2.ps1 -Arch arm64ec
#   powershell -ExecutionPolicy Bypass -File build-fsr2.ps1 -Arch x64   # baseline
#
# ARM64EC outputs are staged into <repo>\arm64ec\libs\fsr2:
#   ffx_fsr2_api_arm64ec.lib, ffx_fsr2_api_dx11_arm64ec.lib,
#   ffx_fsr2_api_dx12_arm64ec.lib, ffx_fsr2_api_vk_arm64ec.lib

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

$ctx = Initialize-DepsEnvironment -Name 'fsr2' -Arch $Arch -Configuration $Configuration `
    -BuildRoot $BuildRoot -OutRoot $OutRoot -VSPath $VSPath -Clean:$Clean `
    -MaxCpu $MaxCpu -SkipToolchainCheck:$SkipToolchainCheck

Assert-Path -Path $ctx.SourceFsr2 -What 'FidelityFX-FSR2-DX11 source clone'

$workDir = Join-Path $BuildRoot ("work-{0}\fsr2" -f $Arch)
$buildDir = Join-Path $BuildRoot ("build-fsr2-{0}" -f $Arch)

if ($Clean) {
    Write-Log "Cleaning $workDir and $buildDir" $ctx.LogFile
    Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
}

Copy-Tree -Source $ctx.SourceFsr2 -Destination $workDir -LogFile $ctx.LogFile

$apiSource = Join-Path $workDir 'src\ffx-fsr2-api'
Assert-Path -Path $apiSource -What 'FSR2 API source directory'
Enable-Arm64ecPlatform -CMakeLists (Join-Path $apiSource 'CMakeLists.txt') -Prefix 'FSR2'

$shaderCompiler = Join-Path $workDir 'tools\sc\FidelityFX_SC.exe'
Assert-File -Path $shaderCompiler -What 'FidelityFX_SC.exe'
foreach ($runtime in @('dxcompiler.dll', 'dxil.dll', 'glslangValidator.exe')) {
    Assert-File -Path (Join-Path $workDir "tools\sc\$runtime") -What "shader compiler runtime $runtime"
}
if (-not $ctx.VulkanSdk) {
    throw 'Vulkan SDK was not found (needed by the FSR2 VK backend). Set VULKAN_SDK or install the Vulkan SDK.'
}

$cacheArgs = @(
    '-DFFX_FSR2_API_DX11=ON',
    '-DFFX_FSR2_API_DX12=ON',
    '-DFFX_FSR2_API_VK=ON',
    '-DFSR2_AUTO_COMPILE_SHADERS=ON'
)
Invoke-CMakeConfigure -Ctx $ctx -SourceDir $apiSource -BuildDir $buildDir -CacheArgs $cacheArgs

$targets = @(
    "ffx_fsr2_api_$Arch",
    "ffx_fsr2_api_dx11_$Arch",
    "ffx_fsr2_api_dx12_$Arch",
    "ffx_fsr2_api_vk_$Arch"
)
Invoke-CMakeBuild -Ctx $ctx -BuildDir $buildDir -Targets $targets

$builtDir = Join-Path $apiSource 'bin\ffx_fsr2_api'
$stageDir = Join-Path $ctx.OutRoot 'fsr2'
$suffix = $ctx.ConfigSuffix
$stagedPaths = @()
foreach ($target in $targets) {
    $fileName = "{0}{1}.lib" -f $target, $suffix
    $built = Find-BuiltLib -PreferredPath (Join-Path $builtDir $fileName) `
        -BuildDir $buildDir -Configuration $Configuration -Name $fileName -LogFile $ctx.LogFile
    $destination = Join-Path $stageDir $fileName
    Copy-StagedLib -Source $built -Destination $destination -LogFile $ctx.LogFile
    $stagedPaths += $destination
}
Assert-LibrarySetArchitecture -Paths $stagedPaths -Ctx $ctx

Write-Log "FSR2 2.2.1 build complete. Outputs staged in $stageDir" $ctx.LogFile
