# Build FidelityFX SDK 3.1.2 (DX11 fork) FSR3 components and backends as
# static libs.
#
# Components: ffx_fsr3_<arch>, ffx_fsr3upscaler_<arch>,
#             ffx_frameinterpolation_<arch>, ffx_opticalflow_<arch>
# Backends:   ffx_backend_dx11_<arch> (required by OptiScaler),
#             ffx_backend_dx12_<arch>, ffx_backend_vk_<arch> (optional extras)
#
# ARM64EC outputs are staged into <repo>\arm64ec\libs\fsr31.

[CmdletBinding()]
param(
    [ValidateSet('arm64ec', 'x64')][string]$Arch = 'arm64ec',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    # OptiScaler links ffx_backend_dx11_arm64ec.lib; FSR3.1 DX12/VK are loaded
    # through the amd_fidelityfx_* runtime DLLs, so only DX11 is built here.
    # Pass -Backends dx11,dx12,vk to build the other backends explicitly.
    [ValidateSet('dx11', 'dx12', 'vk')][string[]]$Backends = @('dx11'),
    [string]$BuildRoot = 'C:\Users\eirik\Documents\opti-build-src',
    [string]$OutRoot,
    [string]$VSPath,
    [switch]$Clean,
    [int]$MaxCpu = 4,
    [switch]$SkipToolchainCheck
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$ctx = Initialize-DepsEnvironment -Name 'fsr31' -Arch $Arch -Configuration $Configuration `
    -BuildRoot $BuildRoot -OutRoot $OutRoot -VSPath $VSPath -Clean:$Clean `
    -MaxCpu $MaxCpu -SkipToolchainCheck:$SkipToolchainCheck

Assert-Path -Path $ctx.SourceFsr31 -What 'FidelityFX-SDK-DX11 source clone'

$workDir = Join-Path $BuildRoot ("work-{0}\fsr31" -f $Arch)
$sdkSource = Join-Path $workDir 'sdk'
$buildDir = Join-Path $BuildRoot ("build-fsr31-{0}" -f $Arch)

if ($Clean) {
    Write-Log "Cleaning $workDir and $buildDir" $ctx.LogFile
    Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Force -Path $workDir | Out-Null
Copy-Tree -Source (Join-Path $ctx.SourceFsr31 'sdk') -Destination $sdkSource -LogFile $ctx.LogFile

Enable-Arm64ecPlatform -CMakeLists (Join-Path $sdkSource 'CMakeLists.txt') -Prefix 'FFX'

$shaderStore = Join-Path $sdkSource 'tools\binary_store'
Assert-File -Path (Join-Path $shaderStore 'FidelityFX_SC.exe') -What 'FidelityFX_SC.exe'
foreach ($runtime in @('dxcompiler.dll', 'dxil.dll', 'glslangValidator.exe')) {
    Assert-File -Path (Join-Path $shaderStore $runtime) -What "shader compiler runtime $runtime"
}
if (($Backends -contains 'vk') -and -not $ctx.VulkanSdk) {
    throw 'Vulkan SDK was not found (needed by the FSR3.1.2 VK backend). Set VULKAN_SDK or install the Vulkan SDK.'
}

$dx11Flag = if ($Backends -contains 'dx11') { 'ON' } else { 'OFF' }
$dx12Flag = if ($Backends -contains 'dx12') { 'ON' } else { 'OFF' }
$vkFlag = if ($Backends -contains 'vk') { 'ON' } else { 'OFF' }
$cacheArgs = @(
    '-DFFX_FSR=ON',
    '-DFFX_FSR3=ON',
    '-DFFX_FSR3UPSCALER=ON',
    '-DFFX_FI=ON',
    '-DFFX_OF=ON',
    "-DFFX_API_DX11=$dx11Flag",
    "-DFFX_API_DX12=$dx12Flag",
    "-DFFX_API_VK=$vkFlag",
    '-DFFX_API_CUSTOM=OFF',
    '-DFFX_AUTO_COMPILE_SHADERS=ON'
)
Invoke-CMakeConfigure -Ctx $ctx -SourceDir $sdkSource -BuildDir $buildDir -CacheArgs $cacheArgs

$componentTargets = @(
    "ffx_fsr3_$Arch",
    "ffx_fsr3upscaler_$Arch",
    "ffx_frameinterpolation_$Arch",
    "ffx_opticalflow_$Arch"
)
$backendTargets = @()
foreach ($backend in $Backends) {
    $backendTargets += "ffx_backend_${backend}_$Arch"
}
$targets = $componentTargets + $backendTargets
Invoke-CMakeBuild -Ctx $ctx -BuildDir $buildDir -Targets $targets

$builtDir = Join-Path $sdkSource 'bin\ffx_sdk'
$stageDir = Join-Path $ctx.OutRoot 'fsr31'
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

Write-Log "FSR3.1.2 build complete. Outputs staged in $stageDir" $ctx.LogFile
