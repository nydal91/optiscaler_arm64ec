# Build OptiScaler for ARM64EC.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File arm64ec\build-optiscaler.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File arm64ec\build-optiscaler.ps1 -Configuration ReleaseDebug
#   powershell -NoProfile -ExecutionPolicy Bypass -File arm64ec\build-optiscaler.ps1 -SkipDeps
#
# Output:
#   arm64ec\Release\a\               (Release package: OptiScaler.dll + runtime files)
#   arm64ec\ReleaseDebug\a\          (ReleaseDebug: OptiScaler.dll only)

[CmdletBinding()]
param(
    [ValidateSet('Release', 'ReleaseDebug')][string]$Configuration = 'Release',
    [string]$VsPath,
    [string]$BuildRoot = 'C:\Users\eirik\Documents\opti-build-src',
    [switch]$SkipDeps,
    [switch]$Clean,
    [int]$MaxCpu = 8
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# GetFullPath rather than Resolve-Path: on a UNC path, Windows PowerShell's
# Resolve-Path keeps the provider prefix and the '..', which MSBuild rejects.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$logDir = Join-Path $BuildRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Write-Step([string]$Message) {
    Write-Host ''
    Write-Host ("=== " + $Message)
}

# The pre-build event runs 'git rev-parse' to stamp the commit. On a network
# share git usually sees a different owner and refuses the repository, which
# fails the build. Allow this one repository, for this process tree only,
# without touching the user's git config.
function Add-GitSafeDirectory([string]$Path) {
    $index = 0
    if ($env:GIT_CONFIG_COUNT) { $index = [int]$env:GIT_CONFIG_COUNT }
    Set-Item -LiteralPath "Env:GIT_CONFIG_KEY_$index" -Value 'safe.directory'
    Set-Item -LiteralPath "Env:GIT_CONFIG_VALUE_$index" -Value ('%(prefix)/' + $Path.Replace('\', '/'))
    $env:GIT_CONFIG_COUNT = ($index + 1).ToString()
}

function Resolve-VisualStudio([string]$Requested) {
    if ($Requested) {
        if (-not (Test-Path -LiteralPath $Requested)) {
            throw "Visual Studio path does not exist: $Requested"
        }
        return $Requested
    }
    $default = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
    if (Test-Path -LiteralPath $default) {
        return $default
    }
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -version '[17.0,18.0)' -latest -products '*' -property installationPath
        if ($LASTEXITCODE -eq 0 -and $found) {
            $candidate = ($found | Select-Object -First 1).Trim()
            if ($candidate -and (Test-Path -LiteralPath $candidate)) {
                return $candidate
            }
        }
    }
    throw 'Visual Studio 2022 was not found. Pass -VsPath explicitly.'
}

function Assert-Arm64ecToolchain([string]$Vs) {
    $arm64Cl = Get-ChildItem -LiteralPath (Join-Path $Vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\arm64\cl.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (-not $arm64Cl) {
        throw ('ARM64 toolchain not found under {0}. Install the VS 2022 individual components ' +
               '"MSVC v143 - VS 2022 C++ ARM64/ARM64EC build tools" and ' +
               '"MSVC v143 - VS 2022 C++ ARM64EC build tools".' -f $Vs)
    }
    $arm64ecPlatform = Join-Path $Vs 'MSBuild\Microsoft\VC\v170\Platforms\ARM64EC'
    if (-not (Test-Path -LiteralPath $arm64ecPlatform)) {
        throw "ARM64EC MSBuild platform not found under $arm64ecPlatform. Install the ARM64EC build-tools component."
    }
    Write-Host "ARM64EC compiler: $arm64Cl"
}

function Invoke-Logged([string]$Exe, [string[]]$Arguments, [string]$LogFile, [string]$Label) {
    Write-Host ("--- " + $Label)
    Write-Host ("    " + $Exe + " " + ($Arguments -join ' '))
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @Arguments 2>&1 | Tee-Object -FilePath $LogFile -Append
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($null -eq $code) { $code = 0 }
    if ($code -ne 0) {
        throw "$Label failed with exit code $code. See $LogFile"
    }
}

if (([uri]$repoRoot).IsUnc) {
    Add-GitSafeDirectory $repoRoot
}

$vs = Resolve-VisualStudio $VsPath
Assert-Arm64ecToolchain $vs

$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild not found: $msbuild"
}

if (-not $SkipDeps) {
    Write-Step 'Building ARM64EC dependency libraries (FreeType, Detours, FSR2, FSR2.1.2, FSR3.1.2)'
    $depsScript = Join-Path $PSScriptRoot 'deps\build-all.ps1'
    if (-not (Test-Path -LiteralPath $depsScript)) {
        throw "Dependency script not found: $depsScript"
    }
    $depsArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $depsScript,
                  '-Arch', 'arm64ec', '-Configuration', 'Release', '-BuildRoot', $BuildRoot,
                  '-MaxCpu', $MaxCpu.ToString())
    if ($Clean) { $depsArgs += '-Clean' }
    $depsLog = Join-Path $logDir ('deps-arm64ec-' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.log')
    Invoke-Logged -Exe 'powershell.exe' -Arguments $depsArgs -LogFile $depsLog -Label 'ARM64EC dependencies'
}
else {
    Write-Step 'Skipping dependency build (-SkipDeps)'
}

$required = @(
    'fsr2\ffx_fsr2_api_arm64ec.lib',
    'fsr2\ffx_fsr2_api_dx11_arm64ec.lib',
    'fsr2\ffx_fsr2_api_dx12_arm64ec.lib',
    'fsr2\ffx_fsr2_api_vk_arm64ec.lib',
    'fsr2_212\ffx_fsr2_212_api_arm64ec.lib',
    'fsr2_212\ffx_fsr2_212_api_dx12_arm64ec.lib',
    'fsr2_212\ffx_fsr2_212_api_vk_arm64ec.lib',
    'fsr31\ffx_fsr3_arm64ec.lib',
    'fsr31\ffx_fsr3upscaler_arm64ec.lib',
    'fsr31\ffx_frameinterpolation_arm64ec.lib',
    'fsr31\ffx_opticalflow_arm64ec.lib',
    'fsr31\ffx_backend_dx11_arm64ec.lib',
    'detours\detours.lib',
    'detours\include\detours\detours.h',
    'freetype\freetype.lib',
    'vulkan\vulkan-1.lib',
    'd3dx\d3dx11.lib'
)
$missing = @()
foreach ($rel in $required) {
    $path = Join-Path $PSScriptRoot ('libs\' + $rel)
    if (-not (Test-Path -LiteralPath $path)) { $missing += $path }
}
if ($missing.Count -gt 0) {
    Write-Step 'Missing ARM64EC dependency outputs'
    $missing | ForEach-Object { Write-Host ("  " + $_) }
    throw 'Run without -SkipDeps (or run arm64ec\deps\build-all.ps1) to produce the missing libraries.'
}

Write-Step ('Building OptiScaler ' + $Configuration + '|ARM64EC')
$buildLog = Join-Path $logDir ('optiscaler-arm64ec-' + $Configuration + '-' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.log')
$solutionDirArg = $repoRoot.Replace('\', '/') + '/'
$msbuildArgs = @(
    (Join-Path $repoRoot 'OptiScaler\OptiScaler.vcxproj'),
    "-p:Configuration=$Configuration",
    '-p:Platform=ARM64EC',
    "-p:SolutionDir=$solutionDirArg",
    '-t:Build',
    '-v:minimal',
    '-nologo',
    "-m:$MaxCpu"
)
Invoke-Logged -Exe $msbuild -Arguments $msbuildArgs -LogFile $buildLog -Label 'OptiScaler ARM64EC build'

$outDir = if ($Configuration -eq 'Release') {
    Join-Path $PSScriptRoot 'Release\a'
} else {
    Join-Path $PSScriptRoot ($Configuration + '\a')
}
$dll = Join-Path $outDir 'OptiScaler.dll'
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Build completed but $dll was not produced."
}

Write-Step 'Verifying PE architecture'
$dumpbin = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory |
    ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if ($dumpbin) {
    & $dumpbin /headers $dll | Select-String -Pattern 'machine \(' | Select-Object -First 2
}

Write-Step 'Done'
Write-Host ("Output: " + $outDir)
Write-Host ("DLL:    " + $dll)
