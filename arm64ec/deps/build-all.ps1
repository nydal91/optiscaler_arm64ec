# Build and stage all OptiScaler static-library dependencies.
#
# ARM64EC release build (the main use case):
#   powershell -NoProfile -ExecutionPolicy Bypass -File build-all.ps1 -Arch arm64ec
#
# x64 baseline:
#   powershell -NoProfile -ExecutionPolicy Bypass -File build-all.ps1 -Arch x64
#
# Each dependency script is idempotent and logs to <BuildRoot>\logs.

[CmdletBinding()]
param(
    [ValidateSet('arm64ec', 'x64')][string]$Arch = 'arm64ec',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidateSet('dx11', 'dx12', 'vk')][string[]]$Backends = @('dx11'),
    [string]$BuildRoot = 'C:\Users\eirik\Documents\opti-build-src',
    [string]$OutRoot,
    [string]$VSPath,
    [switch]$Clean,
    [int]$MaxCpu = 4,
    [switch]$SkipToolchainCheck,
    [switch]$SkipFreeType,
    [switch]$SkipDetours,
    [switch]$SkipFsr2,
    [switch]$SkipFsr212,
    [switch]$SkipFsr31
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$commonParams = @{
    Arch          = $Arch
    Configuration = $Configuration
    BuildRoot     = $BuildRoot
    MaxCpu        = $MaxCpu
}
if ($OutRoot) {
    $commonParams.OutRoot = $OutRoot
}
if ($VSPath) {
    $commonParams.VSPath = $VSPath
}
if ($Clean) {
    $commonParams.Clean = $true
}
if ($SkipToolchainCheck) {
    $commonParams.SkipToolchainCheck = $true
}

$resolvedOutRoot = if ($OutRoot) {
    $OutRoot
} elseif ($Arch -eq 'arm64ec') {
    Join-Path $script:RepoRoot 'arm64ec\libs'
} else {
    Join-Path $BuildRoot 'out-deps-x64'
}

$jobs = @()
if (-not $SkipFreeType) { $jobs += 'build-freetype.ps1' }
if (-not $SkipDetours)  { $jobs += 'build-detours.ps1' }
if (-not $SkipFsr2)     { $jobs += 'build-fsr2.ps1' }
if (-not $SkipFsr212)   { $jobs += 'build-fsr2-212.ps1' }
if (-not $SkipFsr31)    { $jobs += 'build-fsr31.ps1' }

Write-Host ("Building {0} dependencies for {1} {2}" -f $jobs.Count, $Arch, $Configuration)
foreach ($job in $jobs) {
    $scriptPath = Join-Path $PSScriptRoot $job
    Assert-File -Path $scriptPath -What 'dependency build script'
    Write-Host ''
    Write-Host ("================ {0} ================" -f $job)
    if ($job -eq 'build-fsr31.ps1') {
        $jobParams = $commonParams.Clone()
        $jobParams.Backends = $Backends
        & $scriptPath @jobParams
    } else {
        & $scriptPath @commonParams
    }
    if (-not $?) {
        throw "Dependency build '$job' failed. See $BuildRoot\logs for the full log."
    }
}

# Fail loudly if any required output is missing, even if a child script exited
# successfully (for example after a partial build in an unusual environment).
$suffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
$required = @()
if (-not $SkipFreeType) {
    $required += Join-Path $resolvedOutRoot 'freetype\freetype.lib'
}
if (-not $SkipDetours) {
    $required += Join-Path $resolvedOutRoot 'detours\detours.lib'
    $required += Join-Path $resolvedOutRoot 'detours\include\detours\detours.h'
    $required += Join-Path $resolvedOutRoot 'detours\include\detours\detver.h'
}
if (-not $SkipFsr2) {
    foreach ($name in @('ffx_fsr2_api', 'ffx_fsr2_api_dx11', 'ffx_fsr2_api_dx12', 'ffx_fsr2_api_vk')) {
        $required += Join-Path $resolvedOutRoot ("fsr2\{0}_{1}{2}.lib" -f $name, $Arch, $suffix)
    }
}
if (-not $SkipFsr212) {
    foreach ($name in @('ffx_fsr2_212_api', 'ffx_fsr2_212_api_dx12', 'ffx_fsr2_212_api_vk')) {
        $required += Join-Path $resolvedOutRoot ("fsr2_212\{0}_{1}{2}.lib" -f $name, $Arch, $suffix)
    }
}
if (-not $SkipFsr31) {
    $required += Join-Path $resolvedOutRoot ("fsr31\ffx_fsr3_{0}{1}.lib" -f $Arch, $suffix)
    $required += Join-Path $resolvedOutRoot ("fsr31\ffx_fsr3upscaler_{0}{1}.lib" -f $Arch, $suffix)
    $required += Join-Path $resolvedOutRoot ("fsr31\ffx_frameinterpolation_{0}{1}.lib" -f $Arch, $suffix)
    $required += Join-Path $resolvedOutRoot ("fsr31\ffx_opticalflow_{0}{1}.lib" -f $Arch, $suffix)
    if ($Backends -contains 'dx11') {
        $required += Join-Path $resolvedOutRoot ("fsr31\ffx_backend_dx11_{0}{1}.lib" -f $Arch, $suffix)
    }
    if ($Backends -contains 'dx12') {
        $required += Join-Path $resolvedOutRoot ("fsr31\ffx_backend_dx12_{0}{1}.lib" -f $Arch, $suffix)
    }
    if ($Backends -contains 'vk') {
        $required += Join-Path $resolvedOutRoot ("fsr31\ffx_backend_vk_{0}{1}.lib" -f $Arch, $suffix)
    }
}
foreach ($path in ($required | Select-Object -Unique)) {
    Assert-File -Path $path -What 'required staged output'
}

Write-Host ''
Write-Host ("All requested dependencies built successfully into {0}" -f $resolvedOutRoot)
Get-ChildItem -LiteralPath $resolvedOutRoot -Recurse -File |
    Sort-Object FullName |
    Select-Object @{Name = 'Library'; Expression = { $_.FullName }}, @{Name = 'Bytes'; Expression = { $_.Length } } |
    Format-Table -AutoSize
