[CmdletBinding()]
param(
    [string]$InstallRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-NativeTool {
    param(
        [Parameter(Mandatory)]
        [string]$Name
    )

    $command = Get-Command "$Name.exe" -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) {
        return $command.Source
    }

    $hints = @()
    if ($env:VSINSTALLDIR) {
        $hints += "$env:VSINSTALLDIR/VC/Tools/Llvm/x64/bin/$Name.exe"
        $hints += "$env:VSINSTALLDIR/VC/Tools/Llvm/bin/$Name.exe"
    }
    if ($env:VCToolsInstallDir) {
        $hints += "$env:VCToolsInstallDir/../../Llvm/x64/bin/$Name.exe"
        $hints += "$env:VCToolsInstallDir/../../Llvm/bin/$Name.exe"
    }
    if (${env:ProgramFiles}) {
        $hints += "${env:ProgramFiles}/LLVM/bin/$Name.exe"
    }
    if (${env:ProgramFiles}) {
        $hints += Get-ChildItem `
            -Path "${env:ProgramFiles}/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/$Name.exe" `
            -File `
            -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
    }

    return $hints |
        Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
        Select-Object -First 1
}

if ($env:OS -ne 'Windows_NT') {
    throw 'Windows dependencies can be built only on Windows.'
}
if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'The Windows dependency build requires 64-bit Windows.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Run the Windows dependency build from a 64-bit PowerShell process.'
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$VcpkgRoot = Join-Path $ProjectRoot 'vcpkg'
$Triplet = 'x64-windows-static-md'
if (-not $InstallRoot) {
    $InstallRoot = Join-Path $ProjectRoot "build/$Triplet/vcpkg_installed"
}
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)

$CMake = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $CMake) {
    throw 'cmake.exe was not found on PATH.'
}

$ClangCl = Find-NativeTool -Name 'clang-cl'
$LldLink = Find-NativeTool -Name 'lld-link'
if (-not $ClangCl -or -not $LldLink) {
    throw @'
Visual Studio clang-cl and lld-link are required. Install the "C++ Clang tools
for Windows" component, then rerun this script. Required Visual Studio
component IDs include Microsoft.VisualStudio.Component.VC.Llvm.Clang and
Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset.
'@
}
if (-not (Test-Path (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'))) {
    throw 'vcpkg submodule is missing; run: git submodule update --init ffmpeg/vcpkg'
}

Write-Host "Triplet:     $Triplet"
Write-Host "Install root: $InstallRoot"
Write-Host "clang-cl:    $ClangCl"
Write-Host "lld-link:    $LldLink"

$VcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $VcpkgExe)) {
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }
}

$env:VCPKG_DISABLE_METRICS = '1'
$VcpkgArguments = @(
    'install'
    "--x-manifest-root=$ProjectRoot"
    "--x-install-root=$InstallRoot"
    "--triplet=$Triplet"
    "--overlay-ports=$ProjectRoot/ports"
    "--overlay-triplets=$ProjectRoot/triplets"
)
& $VcpkgExe @VcpkgArguments
if ($LASTEXITCODE -ne 0) { throw 'vcpkg install failed' }

$CMakeArguments = @(
    "-DINSTALL_ROOT=$InstallRoot"
    "-DTRIPLET=$Triplet"
    '-P'
    (Join-Path $ProjectRoot 'cmake/verify-install.cmake')
)
& $CMake.Source @CMakeArguments
if ($LASTEXITCODE -ne 0) { throw 'dependency verification failed' }
