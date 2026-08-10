[CmdletBinding()]
param(
    [string]$SdkRoot,
    [string]$NdkRoot,
    [string]$DependencyInstallRoot,
    [string]$DependencyWorkRoot,
    [string]$BuildRoot,
    [int]$Parallel = [Environment]::ProcessorCount,
    [switch]$SkipDependencies
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')
Assert-QtavCiWindowsHost

$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$ModernRoot = Join-Path $RepositoryRoot 'modern'
$FfmpegRoot = Join-Path $RepositoryRoot 'ffmpeg'
$Triplet = 'arm64-android-28-static'
$NdkVersion = '29.0.14206865'
$CMakeVersion = '4.1.2'

if (-not $SdkRoot) {
    $SdkRoot = if ($env:ANDROID_SDK_ROOT) {
        $env:ANDROID_SDK_ROOT
    } elseif ($env:ANDROID_HOME) {
        $env:ANDROID_HOME
    } else {
        Join-Path $env:LOCALAPPDATA 'Android/Sdk'
    }
}
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
if (-not $NdkRoot) {
    $NdkRoot = Join-Path $SdkRoot "ndk/$NdkVersion"
}
$NdkRoot = [IO.Path]::GetFullPath($NdkRoot)
$AndroidToolchain = Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake'
if (-not (Test-Path -LiteralPath $AndroidToolchain -PathType Leaf)) {
    throw "Android NDK $NdkVersion toolchain not found: $AndroidToolchain"
}
$NdkRevision = Get-Content -LiteralPath (Join-Path $NdkRoot 'source.properties') |
    Where-Object { $_ -match '^Pkg\.Revision\s*=\s*(.+)$' } |
    ForEach-Object { $Matches[1].Trim() } |
    Select-Object -First 1
if ($NdkRevision -ne $NdkVersion) {
    throw "Android CI requires NDK $NdkVersion; found '$NdkRevision'."
}

$CMakeRoot = Join-Path $SdkRoot "cmake/$CMakeVersion/bin"
$CMake = Join-Path $CMakeRoot 'cmake.exe'
$Ninja = Join-Path $CMakeRoot 'ninja.exe'
if (-not (Test-Path -LiteralPath $CMake -PathType Leaf) -or
    -not (Test-Path -LiteralPath $Ninja -PathType Leaf)) {
    throw "Android SDK CMake $CMakeVersion is required under: $CMakeRoot"
}
if (-not $DependencyInstallRoot) {
    $DependencyInstallRoot = Join-Path $FfmpegRoot `
        "build/$Triplet/vcpkg_installed"
}
if (-not $DependencyWorkRoot) {
    $DependencyWorkRoot = Join-Path $RepositoryRoot `
        'build/ci/android/vcpkg-work'
}
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $RepositoryRoot 'build/ci/android'
}
$DependencyInstallRoot = [IO.Path]::GetFullPath($DependencyInstallRoot)
$DependencyWorkRoot = [IO.Path]::GetFullPath($DependencyWorkRoot)
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($DependencyWorkRoot -match '\s' -or $BuildRoot -match '\s') {
    throw 'Android CI work and build roots must not contain spaces.'
}
$VcpkgToolchain = Join-Path $FfmpegRoot `
    'vcpkg/scripts/buildsystems/vcpkg.cmake'

$env:ANDROID_SDK_ROOT = $SdkRoot
$env:ANDROID_HOME = $SdkRoot
$env:ANDROID_NDK_HOME = $NdkRoot
$env:ANDROID_NDK_ROOT = $NdkRoot

Write-QtavCiToolVersion -FilePath $CMake
Write-QtavCiToolVersion -FilePath $Ninja
Write-Host "Android SDK:     $SdkRoot"
Write-Host "Android NDK:     $NdkRoot"
Write-Host "NDK revision:    $NdkRevision"
Write-Host 'Android ABI/API: arm64-v8a / 28'
Write-Host "Dependency root: $DependencyInstallRoot"
Write-Host "Build root:      $BuildRoot"
git submodule status ffmpeg/vcpkg
if ($LASTEXITCODE -ne 0) {
    throw 'Could not record the pinned vcpkg submodule revision.'
}

if (-not $SkipDependencies) {
    & (Join-Path $FfmpegRoot 'scripts/build-android.ps1') `
        -SdkRoot $SdkRoot `
        -NdkRoot $NdkRoot `
        -InstallRoot $DependencyInstallRoot `
        -WorkRoot $DependencyWorkRoot
    if ($LASTEXITCODE -ne 0) {
        throw 'Android dependency build failed.'
    }
} else {
    Invoke-QtavCiDependencyVerification `
        -CMake $CMake `
        -RepositoryRoot $RepositoryRoot `
        -InstallRoot $DependencyInstallRoot `
        -Triplet $Triplet
}

foreach ($LibraryType in @('Shared', 'Static')) {
    $LibraryTypeLower = $LibraryType.ToLowerInvariant()
    $BuildDirectory = Join-Path $BuildRoot $LibraryTypeLower
    $InstallPrefix = Join-Path $BuildDirectory 'install'
    $ConsumerBuildDirectory = Join-Path $BuildDirectory 'install-consumer'
    $BuildShared = if ($LibraryType -eq 'Shared') { 'ON' } else { 'OFF' }

    $ToolchainArguments = @(
        '-G'
        'Ninja'
        '-UQTAV_LIBPLACEBO_*'
        '-U__pkg_config_checked_QTAV_LIBPLACEBO'
        "-DCMAKE_MAKE_PROGRAM=$Ninja"
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
        "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$AndroidToolchain"
        "-DVCPKG_TARGET_TRIPLET=$Triplet"
        "-DVCPKG_INSTALLED_DIR=$DependencyInstallRoot"
        '-DVCPKG_MANIFEST_MODE=OFF'
        '-DANDROID_ABI=arm64-v8a'
        '-DANDROID_PLATFORM=android-28'
        '-DANDROID_STL=c++_static'
        '-DCMAKE_BUILD_TYPE=Release'
    )

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            '-S'
            $ModernRoot
            '-B'
            $BuildDirectory
            $ToolchainArguments
            "-DCMAKE_INSTALL_PREFIX=$InstallPrefix"
            "-DBUILD_SHARED_LIBS=$BuildShared"
            '-DQTAV_CORE_BUILD_TESTS=ON'
            '-DQTAV_CORE_BUILD_EXAMPLES=OFF'
        ) `
        -Description "Configure Android $LibraryType Release"

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            '--build'
            $BuildDirectory
            '--parallel'
            $Parallel
        ) `
        -Description "Cross-build Android $LibraryType Release"

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @('--install', $BuildDirectory) `
        -Description "Install Android $LibraryType Release"

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            '-S'
            (Join-Path $ModernRoot 'examples/android/install-consumer')
            '-B'
            $ConsumerBuildDirectory
            $ToolchainArguments
            "-DQtAVCore_DIR=$InstallPrefix/lib/cmake/QtAVCore"
        ) `
        -Description "Configure Android $LibraryType package consumer"

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            '--build'
            $ConsumerBuildDirectory
            '--parallel'
            $Parallel
        ) `
        -Description "Link Android $LibraryType package consumer"
}
