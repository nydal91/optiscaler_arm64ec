# Shared helpers for building OptiScaler's ARM64EC (and x64 baseline)
# static-library dependencies.
#
# All scripts log to <BuildRoot>\logs and throw on any error.

Set-StrictMode -Version Latest

$script:DepsRoot        = $PSScriptRoot
$script:RepoRoot        = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script:DefaultBuildRoot = 'C:\Users\eirik\Documents\opti-build-src'
$script:DefaultVsPath   = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$script:VsWherePath     = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$script:InstallerDir    = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'

function Write-Log {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$LogFile
    )
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Write-Host $line
    if ($LogFile) {
        Add-Content -LiteralPath $LogFile -Value $line -Encoding UTF8
    }
}

function Assert-Path {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$What = 'path'
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing $What`: $Path"
    }
}

function Assert-File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$What = 'file'
    )
    Assert-Path -Path $Path -What $What
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Expected $What to be a file`: $Path"
    }
}

function Resolve-VisualStudioPath {
    param([string]$VSPath)

    if ($VSPath) {
        if (-not (Test-Path -LiteralPath $VSPath)) {
            throw "The requested Visual Studio path does not exist: $VSPath"
        }
        return $VSPath
    }

    if (Test-Path -LiteralPath $script:DefaultVsPath) {
        return $script:DefaultVsPath
    }

    if (Test-Path -LiteralPath $script:VsWherePath) {
        $found = & $script:VsWherePath -version '[17.0,18.0)' -latest -products '*' -property installationPath
        if ($LASTEXITCODE -eq 0 -and $found) {
            $candidate = ($found | Select-Object -First 1).Trim()
            if ($candidate -and (Test-Path -LiteralPath $candidate)) {
                return $candidate
            }
        }
    }

    throw 'Visual Studio 2022 (17.x) was not found. Pass -VSPath explicitly.'
}

function Resolve-CMakePath {
    param([Parameter(Mandatory = $true)][string]$VSPath)

    $bundled = Join-Path $VSPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $bundled) {
        return $bundled
    }
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    throw "cmake.exe was not found (looked for VS-bundled CMake at $bundled and on PATH)."
}

function Resolve-VulkanSdk {
    if ($env:VULKAN_SDK -and (Test-Path -LiteralPath (Join-Path $env:VULKAN_SDK 'Include\vulkan\vulkan.h'))) {
        return $env:VULKAN_SDK
    }
    $root = 'C:\VulkanSDK'
    if (Test-Path -LiteralPath $root) {
        $candidate = Get-ChildItem -LiteralPath $root -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { $_.FullName } |
            Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Include\vulkan\vulkan.h') } |
            Select-Object -First 1
        if ($candidate) {
            return $candidate
        }
    }
    return $null
}

function Test-Arm64ecToolchain {
    param(
        [Parameter(Mandatory = $true)][string]$VSPath,
        [string]$LogFile
    )

    # ARM64EC is an MSBuild platform backed by the ARM64 cross compiler
    # (Hostx64\arm64\cl.exe with /arm64EC), not a separate Hostx64\arm64ec
    # compiler directory and not a Windows SDK um\arm64ec import-lib folder.
    $platformDir = Join-Path $VSPath 'MSBuild\Microsoft\VC\v170\Platforms\ARM64EC'
    if (-not (Test-Path -LiteralPath $platformDir)) {
        throw ("ARM64EC MSBuild platform support is not installed (missing '{0}'). " +
               "Install the Visual Studio individual component 'MSVC v143 - VS 2022 C++ ARM64EC build tools (Latest)'. " +
               "No files were produced.") -f $platformDir
    }
    Write-Log "ARM64EC MSBuild platform found: $platformDir" $LogFile

    $msvcRoot = Join-Path $VSPath 'VC\Tools\MSVC'
    $clCandidates = @()
    $libArm64 = @()
    $libArm64ec = @()
    if (Test-Path -LiteralPath $msvcRoot) {
        foreach ($versionDir in (Get-ChildItem -LiteralPath $msvcRoot -Directory)) {
            $clCandidates += Join-Path $versionDir.FullName 'bin\Hostx64\arm64\cl.exe'
            $clCandidates += Join-Path $versionDir.FullName 'bin\Hostx64\arm64ec\cl.exe'
            $libArm64 += Join-Path $versionDir.FullName 'lib\arm64'
            $libArm64ec += Join-Path $versionDir.FullName 'lib\arm64ec'
        }
    }
    $clPath = $clCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $clPath) {
        throw ("ARM64 cross compiler was not found (looked for VC\Tools\MSVC\<version>\bin\Hostx64\arm64\cl.exe under '{0}'). " +
               "Install the Visual Studio individual component 'MSVC v143 - VS 2022 C++ ARM64 build tools (Latest)' " +
               "alongside the ARM64EC build tools. No files were produced.") -f $VSPath
    }
    Write-Log "ARM64 cross compiler found: $clPath" $LogFile

    $arm64Lib = $libArm64 | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    $arm64ecLib = $libArm64ec | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $arm64Lib -and -not $arm64ecLib) {
        throw ("MSVC ARM64/ARM64EC libraries were not found under '{0}'. Reinstall the ARM64 and ARM64EC build tools. " +
               "No files were produced.") -f $msvcRoot
    }
    if ($arm64Lib) {
        Write-Log "MSVC ARM64 libraries found: $arm64Lib" $LogFile
    }
    if ($arm64ecLib) {
        Write-Log "MSVC ARM64EC libraries found: $arm64ecLib" $LogFile
    }

    $sdkLibRoot = 'C:\Program Files (x86)\Windows Kits\10\Lib'
    $sdkArm64 = $null
    if (Test-Path -LiteralPath $sdkLibRoot) {
        $sdkArm64 = Get-ChildItem -LiteralPath $sdkLibRoot -Directory |
            ForEach-Object { Join-Path $_.FullName 'um\arm64' } |
            Where-Object { Test-Path -LiteralPath (Join-Path $_ 'softintrin.lib') } |
            Select-Object -First 1
    }
    if (-not $sdkArm64) {
        throw ("Windows SDK ARM64 libraries were not found (no {0}\*\um\arm64\softintrin.lib). " +
               "Install the Windows SDK ARM64 support. No files were produced.") -f $sdkLibRoot
    }
    Write-Log "Windows SDK ARM64 libraries found: $sdkArm64" $LogFile
}

function Initialize-DepsEnvironment {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][ValidateSet('arm64ec', 'x64')][string]$Arch,
        [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
        [string]$BuildRoot = $script:DefaultBuildRoot,
        [string]$OutRoot,
        [string]$VSPath,
        [switch]$Clean,
        [int]$MaxCpu = 4,
        [switch]$SkipToolchainCheck
    )

    $VSPath = Resolve-VisualStudioPath -VSPath $VSPath
    $cmake = Resolve-CMakePath -VSPath $VSPath
    $vsDevCmd = Join-Path $VSPath 'Common7\Tools\VsDevCmd.bat'
    Assert-File -Path $vsDevCmd -What 'VsDevCmd.bat'

    $isArm64ec = ($Arch -eq 'arm64ec')
    if ($isArm64ec -and -not $SkipToolchainCheck) {
        Test-Arm64ecToolchain -VSPath $VSPath
    }

    $vulkanSdk = Resolve-VulkanSdk
    if ($vulkanSdk) {
        $env:VULKAN_SDK = $vulkanSdk
    }

    if (-not $OutRoot) {
        if ($isArm64ec) {
            $OutRoot = Join-Path $script:RepoRoot 'arm64ec\libs'
        } else {
            $OutRoot = Join-Path $BuildRoot 'out-deps-x64'
        }
    }

    $logDir = Join-Path $BuildRoot 'logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $logFile = Join-Path $logDir ("{0}-{1}-{2}.log" -f $Name, $Arch, $Configuration)

    $platformArg = if ($isArm64ec) { 'ARM64EC' } else { 'x64' }
    # ARM64EC builds use the ARM64 cross dev environment; Detours/make adds
    # /arm64EC itself and CMake/MSBuild select the ARM64EC platform props.
    $devArch = if ($isArm64ec) { 'arm64' } else { 'amd64' }
    $configSuffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }

    $ctx = [pscustomobject]@{
        Name          = $Name
        Arch          = $Arch
        Configuration = $Configuration
        ConfigSuffix  = $configSuffix
        BuildRoot     = $BuildRoot
        OutRoot       = $OutRoot
        RepoRoot      = $script:RepoRoot
        VSPath        = $VSPath
        CMake         = $cmake
        VsDevCmd      = $vsDevCmd
        InstallerDir  = $script:InstallerDir
        PlatformArg   = $platformArg
        DevArch       = $devArch
        MaxCpu        = $MaxCpu
        Clean         = [bool]$Clean
        VulkanSdk     = $vulkanSdk
        LogFile       = $logFile
        SourceFsr2    = Join-Path $BuildRoot 'FidelityFX-FSR2-DX11'
        SourceFsr212  = Join-Path $BuildRoot 'FidelityFX-FSR2-212'
        SourceFsr31   = Join-Path $BuildRoot 'FidelityFX-SDK-DX11'
        SourceDetours = Join-Path $BuildRoot 'Detours'
        SourceFreetype = Join-Path $BuildRoot 'freetype-2.13.3'
    }

    Write-Log '============================================================' $logFile
    Write-Log ("START {0} arch={1} config={2} platform={3}" -f $Name, $Arch, $Configuration, $platformArg) $logFile
    Write-Log ("VS={0}" -f $VSPath) $logFile
    Write-Log ("CMake={0}" -f $cmake) $logFile
    Write-Log ("OutRoot={0}" -f $OutRoot) $logFile
    if ($vulkanSdk) {
        Write-Log ("VulkanSDK={0}" -f $vulkanSdk) $logFile
    }
    return $ctx
}

function Invoke-NativeLogged {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$LogFile,
        [string]$WorkingDirectory
    )

    Write-Log ">>> $Label" $LogFile
    $display = $Exe
    foreach ($arg in $Arguments) {
        $display += ' ' + $arg
    }
    Write-Log "    $display" $LogFile

    $previousEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $pushed = $false
    $code = 0
    try {
        if ($WorkingDirectory) {
            Push-Location -LiteralPath $WorkingDirectory
            $pushed = $true
        }
        & $Exe @Arguments 2>&1 | Tee-Object -FilePath $LogFile -Append
        $code = $LASTEXITCODE
    }
    finally {
        if ($pushed) {
            Pop-Location
        }
        $ErrorActionPreference = $previousEap
    }

    if ($null -eq $code) {
        $code = 0
    }
    if ($code -ne 0) {
        throw "$Label failed with exit code $code. See log: $LogFile"
    }
}

function Copy-Tree {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$LogFile,
        [string[]]$ExcludeDirs = @('.git')
    )

    Assert-Path -Path $Source -What 'source directory'
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null

    $robocopyArgs = @($Source, $Destination, '/MIR', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:2', '/W:2')
    foreach ($dir in $ExcludeDirs) {
        $robocopyArgs += @('/XD', $dir)
    }

    Write-Log ">>> Copying tree $Source -> $Destination" $LogFile
    & robocopy @robocopyArgs | Out-Null
    $code = $LASTEXITCODE
    # Robocopy uses 0..7 for success.
    if ($code -ge 8) {
        throw "robocopy failed with exit code $code ($Source -> $Destination). See log: $LogFile"
    }
}

function Enable-Arm64ecPlatform {
    param(
        [Parameter(Mandatory = $true)][string]$CMakeLists,
        [Parameter(Mandatory = $true)][ValidateSet('FSR2', 'FFX')][string]$Prefix
    )

    Assert-File -Path $CMakeLists -What 'CMakeLists.txt'
    $text = [System.IO.File]::ReadAllText($CMakeLists)
    if ($text -match 'STREQUAL "ARM64EC"') {
        return
    }

    $pattern = '(?s)if\(CMAKE_GENERATOR_PLATFORM STREQUAL "x64".*?FATAL_ERROR\s+"Unsupported target platform[^"]*"\)\s*\r?\n\s*endif\(\)'
    $regex = [System.Text.RegularExpressions.Regex]::new($pattern)
    $matches = $regex.Matches($text)
    if ($matches.Count -ne 1) {
        throw "Could not locate the platform-detection block in $CMakeLists (matches=$($matches.Count)); the source tree may have changed."
    }

    $replacement = @"
if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR CMAKE_EXE_LINKER_FLAGS STREQUAL "/machine:x64")
    set(${Prefix}_PLATFORM_NAME x64)
elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32" OR CMAKE_EXE_LINKER_FLAGS STREQUAL "/machine:X86")
    set(${Prefix}_PLATFORM_NAME x86)
elseif(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64EC" OR CMAKE_EXE_LINKER_FLAGS STREQUAL "/machine:arm64ec")
    set(${Prefix}_PLATFORM_NAME arm64ec)
else()
    message(FATAL_ERROR "Unsupported target platform - supporting x64, Win32 and ARM64EC")
endif()
"@

    $match = $matches[0]
    $patched = $text.Substring(0, $match.Index) + $replacement + $text.Substring($match.Index + $match.Length)
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($CMakeLists, $patched, $encoding)
}

function Invoke-CMakeConfigure {
    param(
        [Parameter(Mandatory = $true)]$Ctx,
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [string[]]$CacheArgs = @()
    )

    $cmakeArgs = @('-S', $SourceDir, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', $Ctx.PlatformArg) + $CacheArgs
    Invoke-NativeLogged -Exe $Ctx.CMake -Arguments $cmakeArgs -Label "CMake configure ($SourceDir)" -LogFile $Ctx.LogFile
}

function Invoke-CMakeBuild {
    param(
        [Parameter(Mandatory = $true)]$Ctx,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string[]]$Targets
    )

    $cmakeArgs = @('--build', $BuildDir, '--config', $Ctx.Configuration, '--target') + $Targets + @('--', ('/m:' + $Ctx.MaxCpu), '/v:m')
    Invoke-NativeLogged -Exe $Ctx.CMake -Arguments $cmakeArgs -Label ("CMake build ({0} target(s), {1})" -f $Targets.Count, $Ctx.Configuration) -LogFile $Ctx.LogFile
}

function Assert-LibrarySetArchitecture {
    param(
        [Parameter(Mandatory = $true)][string[]]$Paths,
        [Parameter(Mandatory = $true)]$Ctx
    )

    foreach ($path in $Paths) {
        Assert-File -Path $path -What 'library for architecture validation'
    }

    $quoted = ($Paths | ForEach-Object { '"{0}"' -f $_ }) -join ' '
    $cmdLine = "set `"PATH=$($Ctx.InstallerDir);%PATH%`" && call `"$($Ctx.VsDevCmd)`" -arch=$($Ctx.DevArch) -host_arch=amd64 -no_logo >nul 2>&1 && dumpbin /nologo /headers $quoted | findstr /i machine"
    $previousEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $dumpOutput = & $env:ComSpec /d /c $cmdLine 2>&1
        $dumpCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousEap
    }
    if ($dumpCode -ne 0) {
        throw "dumpbin architecture validation failed with exit code $dumpCode for: $($Paths -join ', '). See log: $($Ctx.LogFile)"
    }

    $dumpText = ($dumpOutput | ForEach-Object { "$_" }) -join "`n"
    $machineTypes = @()
    foreach ($match in [regex]::Matches($dumpText, '(?im)^\s*([0-9A-Fa-f]{4})\s+machine\s+\(([^)]+)\)')) {
        $machineTypes += ('{0} ({1})' -f $match.Groups[1].Value.ToUpperInvariant(), $match.Groups[2].Value)
    }
    $machineTypes = @($machineTypes | Select-Object -Unique)
    if ($machineTypes.Count -eq 0) {
        throw "Could not determine machine type of the built libraries: $($Paths -join ', '). See log: $($Ctx.LogFile)"
    }
    Write-Log ("Archive machine types: {0}" -f ($machineTypes -join ', ')) $Ctx.LogFile

    if ($Ctx.Arch -eq 'arm64ec') {
        $hasExpected = @($machineTypes | Where-Object { $_ -match 'ARM64EC|^A641' })
        $hasWrong = @($machineTypes | Where-Object { $_ -match '\(x64\)|^8664' })
        if ($hasExpected.Count -eq 0 -or $hasWrong.Count -gt 0) {
            throw ("Wrong architecture in built libraries (found: {0}). Expected ARM64EC objects. " +
                   "Refusing to accept them. See log: {1}") -f ($machineTypes -join ', '), $Ctx.LogFile
        }
    }
    else {
        $hasExpected = @($machineTypes | Where-Object { $_ -match '\(x64\)|^8664' })
        $hasWrong = @($machineTypes | Where-Object { $_ -match 'ARM64EC|^A641' })
        if ($hasExpected.Count -eq 0 -or $hasWrong.Count -gt 0) {
            throw ("Wrong architecture in built libraries (found: {0}). Expected x64 objects. " +
                   "Refusing to accept them. See log: {1}") -f ($machineTypes -join ', '), $Ctx.LogFile
        }
    }
}

function Find-BuiltLib {
    param(
        [Parameter(Mandatory = $true)][string]$PreferredPath,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$LogFile
    )

    if (Test-Path -LiteralPath $PreferredPath) {
        return $PreferredPath
    }
    $configPath = Join-Path (Join-Path $BuildDir $Configuration) $Name
    if (Test-Path -LiteralPath $configPath) {
        return $configPath
    }
    $found = Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter $Name -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $found) {
        throw "Expected library '$Name' was not produced under '$BuildDir'. See log: $LogFile"
    }
    return $found.FullName
}

function Copy-StagedLib {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$LogFile
    )

    Assert-File -Path $Source -What 'built library'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    Assert-File -Path $Destination -What 'staged library'

    $item = Get-Item -LiteralPath $Destination
    if ($item.Length -le 0) {
        throw "Staged library is empty: $Destination"
    }
    $hash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    Write-Log ("Staged {0} ({1:N0} bytes, SHA256 {2})" -f $Destination, $item.Length, $hash) $LogFile
}

function Invoke-GitClone {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string]$Branch,
        [Parameter(Mandatory = $true)][string]$LogFile
    )

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    $gitArgs = @('clone', '--depth', '1')
    if ($Branch) {
        $gitArgs += @('--branch', $Branch)
    }
    $gitArgs += @($Repository, $Destination)
    Invoke-NativeLogged -Exe 'git.exe' -Arguments $gitArgs -Label "git clone $Repository" -LogFile $LogFile
}
