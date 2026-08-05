[CmdletBinding()]
param(
    [string]$SdkRoot,
    [string]$DependencyInstallRoot,
    [string]$DependencyWorkRoot,
    [string]$BuildDirectory,
    [string]$InstallPrefix,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$BuildType = 'Release',
    [ValidateSet('Static', 'Shared')]
    [string]$LibraryType = 'Shared',
    [int]$Parallel = [Environment]::ProcessorCount,
    [switch]$BuildTests,
    [switch]$BuildExamples,
    [switch]$SkipDependencies,
    [switch]$NoInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$ModernRoot = Join-Path $RepositoryRoot 'modern'
$FfmpegRoot = Join-Path $RepositoryRoot 'ffmpeg'
$Triplet = 'arm64-ohos-23-static'
. (Join-Path $FfmpegRoot 'scripts/ohos-windows-common.ps1')

Assert-QtavOhosWindowsHost

if (-not $DependencyInstallRoot) {
    $DependencyInstallRoot = Join-Path $FfmpegRoot `
        "build/$Triplet/vcpkg_installed"
}
if (-not $DependencyWorkRoot) {
    $DependencyWorkRoot = Join-Path $FfmpegRoot `
        "build/$Triplet/vcpkg-work"
}
$DependencyInstallRoot = [IO.Path]::GetFullPath($DependencyInstallRoot)
$DependencyWorkRoot = [IO.Path]::GetFullPath($DependencyWorkRoot)

$ResolvedSdkRoot = Resolve-QtavOhosSdkRoot -SdkRoot $SdkRoot
if (-not $SkipDependencies) {
    & (Join-Path $FfmpegRoot 'scripts/build-ohos.ps1') `
        -SdkRoot $ResolvedSdkRoot `
        -InstallRoot $DependencyInstallRoot `
        -WorkRoot $DependencyWorkRoot
    if ($LASTEXITCODE -ne 0) { throw 'OHOS dependency build failed' }
}

$EffectiveSdkRoot = Use-QtavOhosSdkWithoutSpaces `
    -SdkRoot $ResolvedSdkRoot `
    -AliasRoot (Join-Path $DependencyWorkRoot 'ohos-sdk')
$Tools = Enable-QtavOhosWindowsBuildTools -SdkRoot $EffectiveSdkRoot
$env:OHOS_SDK_ROOT = $EffectiveSdkRoot

$DependencyPrefix = Join-Path $DependencyInstallRoot $Triplet
if (-not (Test-Path (Join-Path $DependencyPrefix 'include/libavcodec/avcodec.h'))) {
    throw "The OHOS dependency package is incomplete: $DependencyPrefix"
}

$LibraryTypeLower = $LibraryType.ToLowerInvariant()
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $RepositoryRoot `
        "build/modern-ohos-$LibraryTypeLower"
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if (-not $InstallPrefix) {
    $InstallPrefix = Join-Path $BuildDirectory 'install'
}
$InstallPrefix = [IO.Path]::GetFullPath($InstallPrefix)

$BuildShared = if ($LibraryType -eq 'Shared') { 'ON' } else { 'OFF' }
$BuildTestsValue = if ($BuildTests) { 'ON' } else { 'OFF' }
$BuildExamplesValue = if ($BuildExamples) { 'ON' } else { 'OFF' }
$VcpkgToolchain = Join-Path $FfmpegRoot `
    'vcpkg/scripts/buildsystems/vcpkg.cmake'

Write-Host "OHOS SDK:     $EffectiveSdkRoot"
Write-Host "Dependencies: $DependencyPrefix"
Write-Host "Build:        $BuildDirectory"
Write-Host "Install:      $InstallPrefix"
Write-Host "Library type: $LibraryType"

$ConfigureArguments = @(
    '-S'
    $ModernRoot
    '-B'
    $BuildDirectory
    '-G'
    'Ninja'
    "-DCMAKE_BUILD_TYPE=$BuildType"
    "-DCMAKE_INSTALL_PREFIX=$InstallPrefix"
    "-DCMAKE_MAKE_PROGRAM=$($Tools.Ninja)"
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
    "-DVCPKG_TARGET_TRIPLET=$Triplet"
    "-DVCPKG_OVERLAY_TRIPLETS=$FfmpegRoot/triplets"
    "-DVCPKG_INSTALLED_DIR=$DependencyInstallRoot"
    '-DOHOS_ARCH=arm64-v8a'
    '-DOHOS_SDK_NATIVE_PLATFORM=ohos-23'
    '-DOHOS_STL=c++_static'
    "-DBUILD_SHARED_LIBS=$BuildShared"
    "-DQTAV_CORE_BUILD_TESTS=$BuildTestsValue"
    "-DQTAV_CORE_BUILD_EXAMPLES=$BuildExamplesValue"
)
& $Tools.CMake @ConfigureArguments
if ($LASTEXITCODE -ne 0) { throw 'QtAVCore OHOS configuration failed' }

& $Tools.CMake --build $BuildDirectory --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw 'QtAVCore OHOS build failed' }

if (-not $NoInstall) {
    & $Tools.CMake --install $BuildDirectory --config $BuildType
    if ($LASTEXITCODE -ne 0) { throw 'QtAVCore OHOS installation failed' }
}
