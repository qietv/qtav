[CmdletBinding()]
param(
    [string]$DependencyInstallRoot,
    [string]$BuildRoot,
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$Toolset = 'ClangCL',
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
$Triplet = 'x64-windows-static-md'
if (-not $DependencyInstallRoot) {
    $DependencyInstallRoot = Join-Path $FfmpegRoot `
        "build/$Triplet/vcpkg_installed"
}
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $RepositoryRoot 'build/ci/windows'
}
$DependencyInstallRoot = [IO.Path]::GetFullPath($DependencyInstallRoot)
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$CMake = Resolve-QtavCiWindowsCMake
$CTest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $CTest -PathType Leaf)) {
    throw "ctest.exe was not found beside CMake: $CTest"
}
$CMakeDirectory = Split-Path -Parent $CMake
if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $CMakeDirectory) {
    $env:PATH = "$CMakeDirectory$([IO.Path]::PathSeparator)$env:PATH"
}
$VcpkgToolchain = Join-Path $FfmpegRoot `
    'vcpkg/scripts/buildsystems/vcpkg.cmake'

Write-QtavCiToolVersion -FilePath $CMake
Write-QtavCiToolVersion -FilePath $CTest
Write-Host "Generator:       $Generator"
Write-Host "Toolset:         $Toolset"
Write-Host "Dependency root: $DependencyInstallRoot"
Write-Host "Build root:      $BuildRoot"
git submodule status ffmpeg/vcpkg
if ($LASTEXITCODE -ne 0) {
    throw 'Could not record the pinned vcpkg submodule revision.'
}

if (-not $SkipDependencies) {
    & (Join-Path $FfmpegRoot 'scripts/build-windows.ps1') `
        -InstallRoot $DependencyInstallRoot
    if ($LASTEXITCODE -ne 0) {
        throw 'Windows dependency build failed.'
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
    $BuildShared = if ($LibraryType -eq 'Shared') { 'ON' } else { 'OFF' }

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            '-S'
            $ModernRoot
            '-B'
            $BuildDirectory
            '-G'
            $Generator
            '-A'
            'x64'
            '-T'
            $Toolset
            '-UQTAV_LIBPLACEBO_*'
            '-U__pkg_config_checked_QTAV_LIBPLACEBO'
            "-DCMAKE_INSTALL_PREFIX=$InstallPrefix"
            "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
            "-DVCPKG_TARGET_TRIPLET=$Triplet"
            "-DVCPKG_INSTALLED_DIR=$DependencyInstallRoot"
            '-DVCPKG_MANIFEST_MODE=OFF'
            "-DBUILD_SHARED_LIBS=$BuildShared"
            '-DQTAV_CORE_BUILD_TESTS=ON'
            '-DQTAV_CORE_BUILD_EXAMPLES=ON'
        ) `
        -Description "Configure Windows $LibraryType Release"

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            '--build'
            $BuildDirectory
            '--config'
            'Release'
            '--parallel'
            $Parallel
        ) `
        -Description "Build Windows $LibraryType Release"

    Invoke-QtavCiCommand `
        -FilePath $CTest `
        -ArgumentList @(
            '--test-dir'
            $BuildDirectory
            '-C'
            'Release'
            '--output-on-failure'
            '--no-tests=error'
            '--output-junit'
            (Join-Path $BuildDirectory 'ctest-results.xml')
        ) `
        -Description "Test Windows $LibraryType Release"

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            '--install'
            $BuildDirectory
            '--config'
            'Release'
            '--prefix'
            $InstallPrefix
        ) `
        -Description "Install Windows $LibraryType Release"
}
