[CmdletBinding()]
param(
    [string]$SdkRoot,
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
$Triplet = 'arm64-ohos-23-static'
. (Join-Path $FfmpegRoot 'scripts/ohos-windows-common.ps1')

$ResolvedSdkRoot = Resolve-QtavOhosSdkRoot -SdkRoot $SdkRoot
if (-not $DependencyInstallRoot) {
    $DependencyInstallRoot = Join-Path $FfmpegRoot `
        "build/$Triplet/vcpkg_installed"
}
if (-not $DependencyWorkRoot) {
    $DependencyWorkRoot = Join-Path $RepositoryRoot `
        'build/ci/ohos/vcpkg-work'
}
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $RepositoryRoot 'build/ci/ohos'
}
$DependencyInstallRoot = [IO.Path]::GetFullPath($DependencyInstallRoot)
$DependencyWorkRoot = [IO.Path]::GetFullPath($DependencyWorkRoot)
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($DependencyWorkRoot -match '\s' -or $BuildRoot -match '\s') {
    throw 'OHOS CI work and build roots must not contain spaces.'
}

if (-not $SkipDependencies) {
    & (Join-Path $FfmpegRoot 'scripts/build-ohos.ps1') `
        -SdkRoot $ResolvedSdkRoot `
        -InstallRoot $DependencyInstallRoot `
        -WorkRoot $DependencyWorkRoot
    if ($LASTEXITCODE -ne 0) {
        throw 'OHOS dependency build failed.'
    }
}

$EffectiveSdkRoot = Use-QtavOhosSdkWithoutSpaces `
    -SdkRoot $ResolvedSdkRoot `
    -AliasRoot (Join-Path $DependencyWorkRoot 'ohos-sdk')
$Tools = Enable-QtavOhosWindowsBuildTools -SdkRoot $EffectiveSdkRoot
$env:OHOS_SDK_ROOT = $EffectiveSdkRoot
$VcpkgToolchain = Join-Path $FfmpegRoot `
    'vcpkg/scripts/buildsystems/vcpkg.cmake'
$OhosChainloadToolchain = Join-Path $FfmpegRoot `
    'triplets/toolchains/ohos-native-sdk.cmake'

Write-QtavCiToolVersion -FilePath $Tools.CMake
Write-QtavCiToolVersion -FilePath $Tools.Ninja
Write-Host "OHOS SDK:        $EffectiveSdkRoot"
Write-Host 'OHOS ABI/API:    arm64-v8a / 23'
Write-Host "Dependency root: $DependencyInstallRoot"
Write-Host "Build root:      $BuildRoot"
git submodule status ffmpeg/vcpkg
if ($LASTEXITCODE -ne 0) {
    throw 'Could not record the pinned vcpkg submodule revision.'
}

Invoke-QtavCiDependencyVerification `
    -CMake $Tools.CMake `
    -RepositoryRoot $RepositoryRoot `
    -InstallRoot $DependencyInstallRoot `
    -Triplet $Triplet

foreach ($LibraryType in @('Shared', 'Static')) {
    $LibraryTypeLower = $LibraryType.ToLowerInvariant()
    $BuildDirectory = Join-Path $BuildRoot $LibraryTypeLower
    $InstallPrefix = Join-Path $BuildDirectory 'install'
    $ConsumerBuildDirectory = Join-Path $BuildDirectory 'install-consumer'

    & (Join-Path $ModernRoot 'scripts/build-ohos.ps1') `
        -SdkRoot $ResolvedSdkRoot `
        -DependencyInstallRoot $DependencyInstallRoot `
        -DependencyWorkRoot $DependencyWorkRoot `
        -BuildDirectory $BuildDirectory `
        -InstallPrefix $InstallPrefix `
        -BuildType Release `
        -LibraryType $LibraryType `
        -Parallel $Parallel `
        -BuildTests `
        -BuildExamples `
        -SkipDependencies
    if ($LASTEXITCODE -ne 0) {
        throw "QtAVCore OHOS $LibraryType cross-build failed."
    }

    $ToolchainArguments = @(
        '-G'
        'Ninja'
        '-UQTAV_LIBPLACEBO_*'
        '-U__pkg_config_checked_QTAV_LIBPLACEBO'
        '-DCMAKE_BUILD_TYPE=Release'
        "-DCMAKE_MAKE_PROGRAM=$($Tools.Ninja)"
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
        "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$OhosChainloadToolchain"
        "-DVCPKG_TARGET_TRIPLET=$Triplet"
        "-DVCPKG_INSTALLED_DIR=$DependencyInstallRoot"
        '-DVCPKG_MANIFEST_MODE=OFF'
        '-DOHOS_ARCH=arm64-v8a'
        '-DOHOS_SDK_NATIVE_PLATFORM=ohos-23'
        '-DOHOS_STL=c++_static'
    )

    Invoke-QtavCiCommand `
        -FilePath $Tools.CMake `
        -ArgumentList @(
            '-S'
            (Join-Path $ModernRoot 'examples/ohos/install-consumer')
            '-B'
            $ConsumerBuildDirectory
            $ToolchainArguments
            "-DQtAVCore_DIR=$InstallPrefix/lib/cmake/QtAVCore"
        ) `
        -Description "Configure OHOS $LibraryType package consumer"

    Invoke-QtavCiCommand `
        -FilePath $Tools.CMake `
        -ArgumentList @(
            '--build'
            $ConsumerBuildDirectory
            '--parallel'
            $Parallel
        ) `
        -Description "Link OHOS $LibraryType package consumer"
}
