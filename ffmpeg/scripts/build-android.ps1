[CmdletBinding()]
param(
    [string]$SdkRoot,
    [string]$NdkRoot,
    [string]$InstallRoot,
    [string]$WorkRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-QtavAndroidSdkRoot {
    param([string]$RequestedRoot)

    $Candidates = @()
    if ($RequestedRoot) {
        $Candidates += $RequestedRoot
    }
    if ($env:ANDROID_SDK_ROOT) {
        $Candidates += $env:ANDROID_SDK_ROOT
    }
    if ($env:ANDROID_HOME) {
        $Candidates += $env:ANDROID_HOME
    }
    if ($env:LOCALAPPDATA) {
        $Candidates += Join-Path $env:LOCALAPPDATA 'Android/Sdk'
    }

    foreach ($Candidate in $Candidates | Select-Object -Unique) {
        if ($Candidate -and (Test-Path -LiteralPath $Candidate -PathType Container)) {
            return (Resolve-Path -LiteralPath $Candidate).Path
        }
    }
    throw @'
The Android SDK was not found. Pass -SdkRoot or set ANDROID_SDK_ROOT or
ANDROID_HOME to the SDK managed by Android Studio. This script does not install
Android SDK or NDK packages.
'@
}

function Resolve-QtavAndroidNdkRoot {
    param(
        [string]$RequestedRoot,
        [Parameter(Mandatory)]
        [string]$ResolvedSdkRoot
    )

    $Candidates = @()
    if ($RequestedRoot) {
        $Candidates += $RequestedRoot
    }
    if ($env:ANDROID_NDK_HOME) {
        $Candidates += $env:ANDROID_NDK_HOME
    }
    if ($env:ANDROID_NDK_ROOT) {
        $Candidates += $env:ANDROID_NDK_ROOT
    }

    $NdkDirectory = Join-Path $ResolvedSdkRoot 'ndk'
    if (Test-Path -LiteralPath $NdkDirectory -PathType Container) {
        $Candidates += Get-ChildItem -LiteralPath $NdkDirectory -Directory |
            Sort-Object {
                $Version = $null
                if ([Version]::TryParse($_.Name, [ref]$Version)) {
                    return $Version
                }
                return [Version]'0.0'
            } -Descending |
            Select-Object -ExpandProperty FullName
    }
    $Candidates += Join-Path $ResolvedSdkRoot 'ndk-bundle'

    foreach ($Candidate in $Candidates | Select-Object -Unique) {
        if (-not $Candidate) {
            continue
        }
        $Toolchain = Join-Path $Candidate 'build/cmake/android.toolchain.cmake'
        if (Test-Path -LiteralPath $Toolchain -PathType Leaf) {
            return (Resolve-Path -LiteralPath $Candidate).Path
        }
    }
    throw @'
No installed Android NDK contains build/cmake/android.toolchain.cmake. Select an
NDK in Android Studio, or pass an already installed NDK with -NdkRoot. This
script does not install or replace the NDK.
'@
}

function Resolve-QtavHostCMake {
    param([Parameter(Mandatory)][string]$ResolvedSdkRoot)

    $Command = Get-Command cmake.exe -CommandType Application `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($Command) {
        return $Command.Source
    }

    $CMakeRoot = Join-Path $ResolvedSdkRoot 'cmake'
    if (Test-Path -LiteralPath $CMakeRoot -PathType Container) {
        $Candidates = Get-ChildItem -LiteralPath $CMakeRoot -Directory |
            Sort-Object {
                $Version = $null
                if ([Version]::TryParse($_.Name, [ref]$Version)) {
                    return $Version
                }
                return [Version]'0.0'
            } -Descending
        foreach ($Candidate in $Candidates) {
            $Executable = Join-Path $Candidate.FullName 'bin/cmake.exe'
            if (Test-Path -LiteralPath $Executable -PathType Leaf) {
                return $Executable
            }
        }
    }
    throw @'
cmake.exe was not found on PATH or in the Android SDK. Install/select CMake in
Android Studio before running this script; the script does not install tools.
'@
}

if ($env:OS -ne 'Windows_NT') {
    throw 'This Android PowerShell entry point can run only on Windows.'
}
if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'The Android dependency build requires 64-bit Windows.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Run the Android dependency build from a 64-bit PowerShell process.'
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$VcpkgRoot = Join-Path $ProjectRoot 'vcpkg'
$Triplet = 'arm64-android-28-static'
if (-not $InstallRoot) {
    $InstallRoot = Join-Path $ProjectRoot "build/$Triplet/vcpkg_installed"
}
if (-not $WorkRoot) {
    $WorkRoot = Join-Path $ProjectRoot "build/$Triplet/vcpkg-work-windows"
}
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if ($WorkRoot -match '\s') {
    throw 'The Android dependency work root must not contain spaces.'
}

$ResolvedSdkRoot = Resolve-QtavAndroidSdkRoot -RequestedRoot $SdkRoot
$ResolvedNdkRoot = Resolve-QtavAndroidNdkRoot `
    -RequestedRoot $NdkRoot `
    -ResolvedSdkRoot $ResolvedSdkRoot
$CMake = Resolve-QtavHostCMake -ResolvedSdkRoot $ResolvedSdkRoot

if (-not (Test-Path (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'))) {
    throw 'vcpkg submodule is missing; run: git submodule update --init ffmpeg/vcpkg'
}

New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
$env:ANDROID_SDK_ROOT = $ResolvedSdkRoot
$env:ANDROID_HOME = $ResolvedSdkRoot
$env:ANDROID_NDK_HOME = $ResolvedNdkRoot
$env:ANDROID_NDK_ROOT = $ResolvedNdkRoot
$env:VCPKG_DISABLE_METRICS = '1'

$NdkRevision = Get-Content -LiteralPath (Join-Path $ResolvedNdkRoot 'source.properties') `
    -ErrorAction SilentlyContinue |
    Where-Object { $_ -match '^Pkg\.Revision\s*=\s*(.+)$' } |
    ForEach-Object { $Matches[1].Trim() } |
    Select-Object -First 1

Write-Host "Triplet:      $Triplet"
Write-Host "Android SDK:  $ResolvedSdkRoot"
Write-Host "Android NDK:  $ResolvedNdkRoot"
if ($NdkRevision) {
    Write-Host "NDK revision: $NdkRevision"
}
Write-Host "CMake:        $CMake"
Write-Host "Install root: $InstallRoot"
Write-Host "Work root:    $WorkRoot"

$VcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $VcpkgExe -PathType Leaf)) {
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
}

$VcpkgArguments = @(
    'install'
    "--x-manifest-root=$ProjectRoot"
    "--x-install-root=$InstallRoot"
    "--x-buildtrees-root=$(Join-Path $WorkRoot 'buildtrees')"
    "--x-packages-root=$(Join-Path $WorkRoot 'packages')"
    "--downloads-root=$(Join-Path $WorkRoot 'downloads')"
    "--triplet=$Triplet"
    "--overlay-ports=$ProjectRoot/ports"
    "--overlay-triplets=$ProjectRoot/triplets"
)
& $VcpkgExe @VcpkgArguments
if ($LASTEXITCODE -ne 0) { throw 'Android dependency build failed' }

$CMakeArguments = @(
    "-DINSTALL_ROOT=$InstallRoot"
    "-DTRIPLET=$Triplet"
    '-P'
    (Join-Path $ProjectRoot 'cmake/verify-install.cmake')
)
& $CMake @CMakeArguments
if ($LASTEXITCODE -ne 0) { throw 'Android dependency verification failed' }
