[CmdletBinding()]
param(
    [string]$InstallRoot
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$VcpkgRoot = Join-Path $ProjectRoot 'vcpkg'
$Triplet = 'x64-windows-static-md'
if (-not $InstallRoot) {
    $InstallRoot = Join-Path $ProjectRoot "build/$Triplet/vcpkg_installed"
}

if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'The Windows dependency build requires 64-bit Windows.'
}
if (-not (Test-Path (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'))) {
    throw 'vcpkg submodule is missing; run: git submodule update --init ffmpeg/vcpkg'
}

$VcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $VcpkgExe)) {
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
}

& $VcpkgExe install `
    "--x-manifest-root=$ProjectRoot" `
    "--x-install-root=$InstallRoot" `
    "--triplet=$Triplet" `
    "--overlay-ports=$ProjectRoot/ports" `
    "--overlay-triplets=$ProjectRoot/triplets"
if ($LASTEXITCODE -ne 0) { throw 'vcpkg install failed' }

& cmake `
    "-DINSTALL_ROOT=$InstallRoot" `
    "-DTRIPLET=$Triplet" `
    -P (Join-Path $ProjectRoot 'cmake/verify-install.cmake')
if ($LASTEXITCODE -ne 0) { throw 'dependency verification failed' }
