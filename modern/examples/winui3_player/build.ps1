param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$QtAVBuildDir = ''
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = (Resolve-Path (Join-Path $projectDir '..\..\..')).Path

if ([string]::IsNullOrWhiteSpace($QtAVBuildDir)) {
    $QtAVBuildDir = Join-Path $repositoryRoot 'build\modern-shared'
}

$QtAVBuildDir = [System.IO.Path]::GetFullPath($QtAVBuildDir)
$cmakeCache = Join-Path $QtAVBuildDir 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cmakeCache)) {
    throw @"
QtAVBuildDir is not a configured CMake build:
  $QtAVBuildDir

Configure a shared QtAVCore build first, for example:
  cmake -S "$repositoryRoot\modern" -B "$QtAVBuildDir" -DBUILD_SHARED_LIBS=ON
"@
}

$sharedSetting = Select-String `
    -LiteralPath $cmakeCache `
    -Pattern '^BUILD_SHARED_LIBS:[^=]+=ON$' `
    -Quiet
if (!$sharedSetting) {
    throw "The WinUI demo requires BUILD_SHARED_LIBS=ON: $QtAVBuildDir"
}

$requiredTargets = @(
    'qtav_core',
    'qtav_platform_windows',
    'qtav_render_d3d11',
    'qtav_audio_wasapi',
    'qtav_audio_resample',
    'qtav_hw_d3d11va',
    'qtav_interop_d3d11'
)

& cmake `
    --build $QtAVBuildDir `
    --config $Configuration `
    --parallel `
    --target $requiredTargets
if ($LASTEXITCODE -ne 0) {
    throw "QtAVCore build failed with exit code $LASTEXITCODE."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}

$visualStudio = & $vswhere `
    -latest `
    -products '*' `
    -requires Microsoft.Component.MSBuild `
    -property installationPath
if ([string]::IsNullOrWhiteSpace($visualStudio)) {
    throw 'A Visual Studio installation with MSBuild was not found.'
}

$msbuild = Join-Path $visualStudio 'MSBuild\Current\Bin\MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild was not found: $msbuild"
}

$project = Join-Path $projectDir 'QtAVWinUI3.vcxproj'
& $msbuild `
    $project `
    /restore `
    /m `
    /p:Configuration=$Configuration `
    /p:Platform=x64 `
    /p:QtAVBuildDir=$QtAVBuildDir
if ($LASTEXITCODE -ne 0) {
    throw "WinUI 3 build failed with exit code $LASTEXITCODE."
}

$executable = Join-Path `
    $projectDir `
    "bin\x64\$Configuration\QtAVWinUI3.exe"
Write-Host "Built: $executable"
