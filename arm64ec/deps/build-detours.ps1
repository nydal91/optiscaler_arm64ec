# Build Microsoft Detours for ARM64EC (or x64 baseline) with the official
# nmake build, then validate the machine type of every object in the archive
# before staging. This matters because an x64 cl.exe silently ignores
# /arm64EC (warning D9002) and still produces a lib, which must never be
# staged as an ARM64EC dependency.
#
# ARM64EC outputs are staged into <repo>\arm64ec\libs\detours:
#   detours.lib
#   include\detours.h, include\detver.h

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

$ctx = Initialize-DepsEnvironment -Name 'detours' -Arch $Arch -Configuration $Configuration `
    -BuildRoot $BuildRoot -OutRoot $OutRoot -VSPath $VSPath -Clean:$Clean `
    -MaxCpu $MaxCpu -SkipToolchainCheck:$SkipToolchainCheck

if (-not (Test-Path -LiteralPath (Join-Path $ctx.SourceDetours 'Makefile'))) {
    Write-Log "Detours sources not found at $($ctx.SourceDetours); cloning microsoft/Detours" $ctx.LogFile
    Invoke-GitClone -Repository 'https://github.com/microsoft/Detours.git' `
        -Destination $ctx.SourceDetours -LogFile $ctx.LogFile
}
Assert-File -Path (Join-Path $ctx.SourceDetours 'system.mak') -What 'Detours system.mak'
$sourceDir = Join-Path $ctx.SourceDetours 'src'
Assert-File -Path (Join-Path $sourceDir 'makefile') -What 'Detours src/makefile'

$processor = if ($Arch -eq 'arm64ec') { 'ARM64EC' } else { 'X64' }
$detoursConfig = if ($Configuration -eq 'Debug') { 'Debug' } else { '' }

# Never reuse objects: a previous failed attempt may contain x64 objects in an
# obj.ARM64EC directory, and nmake would happily reuse them.
# The Detours makefile keeps intermediate objects in src\obj.<processor> and
# writes the library/header copy to <repo>\lib.<processor> and \bin.<processor>.
$objDir = Join-Path $sourceDir ("obj.{0}{1}" -f $processor, $detoursConfig)
$libDir = Join-Path $ctx.SourceDetours ("lib.{0}{1}" -f $processor, $detoursConfig)
$binDir = Join-Path $ctx.SourceDetours ("bin.{0}{1}" -f $processor, $detoursConfig)
foreach ($dir in @($objDir, $libDir, $binDir)) {
    if (Test-Path -LiteralPath $dir) {
        Write-Log "Removing stale build directory $dir" $ctx.LogFile
        Remove-Item -LiteralPath $dir -Recurse -Force
    }
}

$devArch = $ctx.DevArch
$nmakeLine = "set `"PATH=$($ctx.InstallerDir);%PATH%`" && call `"$($ctx.VsDevCmd)`" -arch=$devArch -host_arch=amd64 -no_logo >nul 2>&1 && cd /d `"$sourceDir`" && nmake /nologo DETOURS_TARGET_PROCESSOR=$processor"
if ($detoursConfig) {
    $nmakeLine += " DETOURS_CONFIG=$detoursConfig"
}
Invoke-NativeLogged -Exe $env:ComSpec -Arguments @('/d', '/c', $nmakeLine) `
    -Label "nmake Detours ($processor)" -LogFile $ctx.LogFile

$builtLib = Join-Path $libDir 'detours.lib'
Assert-File -Path $builtLib -What 'built detours.lib'

$objects = @(Get-ChildItem -LiteralPath $objDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -eq '.obj' })
if ($objects.Count -eq 0) {
    throw "Detours build produced no object files in $objDir; cannot validate machine type. See log: $($ctx.LogFile)"
}

# Dump all objects in one dumpbin invocation and require the whole set to be
# ARM64EC with no x64 members. bin\Hostx64\arm64\cl.exe with /arm64EC emits
# A641 (ARM64EC) objects.
$quoted = ($objects | ForEach-Object { '"{0}"' -f $_.FullName }) -join ' '
$dumpLine = "set `"PATH=$($ctx.InstallerDir);%PATH%`" && call `"$($ctx.VsDevCmd)`" -arch=$devArch -host_arch=amd64 -no_logo >nul 2>&1 && dumpbin /nologo /headers $quoted"
$previousEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $dumpOutput = & $env:ComSpec /d /c $dumpLine 2>&1
    $dumpCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousEap
}
if ($dumpCode -ne 0) {
    throw "dumpbin failed with exit code $dumpCode while validating $builtLib. See log: $($ctx.LogFile)"
}

$dumpText = ($dumpOutput | ForEach-Object { "$_" }) -join "`n"
$machineMatches = [regex]::Matches($dumpText, '(?im)^\s*([0-9A-Fa-f]{4})\s+machine\s+\(([^)]+)\)\s*$')
$machineTypes = @()
foreach ($match in $machineMatches) {
    $machineTypes += ('{0} ({1})' -f $match.Groups[1].Value.ToUpperInvariant(), $match.Groups[2].Value)
}
if ($machineTypes.Count -eq 0) {
    # Older dumpbin builds may omit the descriptive text; accept the hex form.
    foreach ($match in [regex]::Matches($dumpText, '(?im)^\s*([0-9A-Fa-f]{4})\s+machine\b')) {
        $machineTypes += $match.Groups[1].Value.ToUpperInvariant()
    }
}
$machineTypes = @($machineTypes | Select-Object -Unique)
Write-Log ("Detours object machine types: {0}" -f ($machineTypes -join ', ')) $ctx.LogFile

if ($Arch -eq 'arm64ec') {
    $hasArm64ec = @($machineTypes | Where-Object { $_ -match 'ARM64EC|^A641' })
    $hasX64 = @($machineTypes | Where-Object { $_ -match '\(x64\)|^8664' })
    if ($machineTypes.Count -eq 0) {
        throw "Could not determine machine type for the Detours objects; refusing to stage $builtLib. See log: $($ctx.LogFile)"
    }
    if ($hasArm64ec.Count -eq 0 -or $hasX64.Count -gt 0) {
        throw ("Detours 'ARM64EC' build did NOT produce pure ARM64EC objects (found: {0}). " +
               "Refusing to stage an incorrectly-architected detours.lib. Verify that the ARM64 cross toolset " +
               "(bin\Hostx64\arm64\cl.exe) is used; cl.exe must not report D9002 for /arm64EC. See log: {1}") `
            -f ($machineTypes -join ', '), $ctx.LogFile
    }
}
else {
    $hasX64 = @($machineTypes | Where-Object { $_ -match '\(x64\)|^8664' })
    if ($hasX64.Count -eq 0) {
        throw "Detours x64 baseline build did not produce x64 objects (found: $($machineTypes -join ', ')). See log: $($ctx.LogFile)"
    }
}

$stageDir = Join-Path $ctx.OutRoot 'detours'
$stagedLib = Join-Path $stageDir 'detours.lib'
Copy-StagedLib -Source $builtLib -Destination $stagedLib -LogFile $ctx.LogFile
Assert-LibrarySetArchitecture -Paths @($stagedLib) -Ctx $ctx
# OptiScaler includes <detours/detours.h> with IncludePath ending at
# arm64ec\libs\detours\include, so the headers live one level deeper.
$stageInclude = Join-Path $stageDir 'include\detours'
Copy-Tree -Source (Join-Path $ctx.SourceDetours 'include') -Destination $stageInclude `
    -LogFile $ctx.LogFile -ExcludeDirs @()
Assert-File -Path (Join-Path $stageInclude 'detours.h') -What 'staged detours.h'
Assert-File -Path (Join-Path $stageInclude 'detver.h') -What 'staged detver.h'

Write-Log "Detours build complete. Outputs staged in $stageDir" $ctx.LogFile
