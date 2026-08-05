[CmdletBinding()]
param(
    [string]$SdkRoot,
    [string]$InstallRoot,
    [string]$WorkRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ohos-windows-common.ps1')

Assert-QtavOhosWindowsHost

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$VcpkgRoot = Join-Path $ProjectRoot 'vcpkg'
$Triplet = 'arm64-ohos-23-static'
if (-not $InstallRoot) {
    $InstallRoot = Join-Path $ProjectRoot "build/$Triplet/vcpkg_installed"
}
if (-not $WorkRoot) {
    $WorkRoot = Join-Path $ProjectRoot "build/$Triplet/vcpkg-work"
}
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if ($WorkRoot -match '\s') {
    throw 'The FFmpeg OHOS work root must not contain spaces. Pass -WorkRoot with a space-free path.'
}

$ResolvedSdkRoot = Resolve-QtavOhosSdkRoot -SdkRoot $SdkRoot
$EffectiveSdkRoot = Use-QtavOhosSdkWithoutSpaces `
    -SdkRoot $ResolvedSdkRoot `
    -AliasRoot (Join-Path $WorkRoot 'ohos-sdk')
$Tools = Enable-QtavOhosWindowsBuildTools -SdkRoot $EffectiveSdkRoot

if (-not (Test-Path (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'))) {
    throw 'vcpkg submodule is missing; run: git submodule update --init ffmpeg/vcpkg'
}

New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
$env:OHOS_SDK_ROOT = $EffectiveSdkRoot
$env:VCPKG_DISABLE_METRICS = '1'

Write-Host "Triplet:      $Triplet"
Write-Host "OHOS SDK:     $EffectiveSdkRoot"
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
if ($LASTEXITCODE -ne 0) { throw 'OHOS dependency build failed' }

$CMakeArguments = @(
    "-DINSTALL_ROOT=$InstallRoot"
    "-DTRIPLET=$Triplet"
    '-P'
    (Join-Path $ProjectRoot 'cmake/verify-install.cmake')
)
& $Tools.CMake @CMakeArguments
if ($LASTEXITCODE -ne 0) { throw 'OHOS dependency verification failed' }
