[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$BuildDirectory,
    [int]$Parallel = [Environment]::ProcessorCount,
    [switch]$SkipQtAVBuild,
    [switch]$NoPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExampleRoot = $PSScriptRoot
$ModernRoot = (Resolve-Path (Join-Path $ExampleRoot '../..')).Path
$RepositoryRoot = (Resolve-Path (Join-Path $ModernRoot '..')).Path
$TemplateRoot = Join-Path $ExampleRoot 'player-hap'
if (-not $ProjectRoot) {
    $ProjectRoot = $TemplateRoot
}
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $RepositoryRoot `
        'build/modern-ohos-player-hap'
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$DevEcoSdkHome = 'C:/Program Files/Huawei/DevEco Studio/sdk'
$LibCxxShared = Join-Path $DevEcoSdkHome `
    'default/openharmony/native/llvm/lib/aarch64-linux-ohos/libc++_shared.so'

if (-not (Test-Path -LiteralPath `
        (Join-Path $ProjectRoot 'build-profile.json5'))) {
    throw "The HAP project is incomplete: $ProjectRoot"
}
if (-not (Test-Path -LiteralPath $LibCxxShared)) {
    throw "The OHOS arm64 C++ runtime was not found: $LibCxxShared"
}

if (-not $SkipQtAVBuild) {
    & (Join-Path $ModernRoot 'scripts/build-ohos.ps1') `
        -SkipDependencies `
        -NoInstall `
        -BuildExamples `
        -BuildDirectory $BuildDirectory `
        -Parallel $Parallel
    if ($LASTEXITCODE -ne 0) {
        throw 'QtAVCore OHOS player build failed'
    }
}

$NativeOutputs = @(
    @(
        (Join-Path $BuildDirectory 'core/libqtav_core.so.2.0.0'),
        'libqtav_core.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/render/vulkan/libqtav_render_vulkan.so.2.0.0'),
        'libqtav_render_vulkan.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/render/vulkan/ohos/libqtav_render_vulkan_ohos.so.2.0.0'),
        'libqtav_render_vulkan_ohos.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/render/opengl/libqtav_render_opengl.so.2.0.0'),
        'libqtav_render_opengl.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/render/opengl/ohos/libqtav_render_opengl_ohos.so.2.0.0'),
        'libqtav_render_opengl_ohos.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/render/mobile/libqtav_render_mobile.so.2.0.0'),
        'libqtav_render_mobile.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/audio/resample/libqtav_audio_resample.so.2.0.0'),
        'libqtav_audio_resample.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/audio/timestretch/libqtav_audio_timestretch.so.2.0.0'),
        'libqtav_audio_timestretch.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/audio/ohaudio/libqtav_audio_ohaudio.so.2.0.0'),
        'libqtav_audio_ohaudio.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/hwaccel/ohcodec/libqtav_hw_ohcodec.so.2.0.0'),
        'libqtav_hw_ohcodec.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/interop/ohcodec_opengl/libqtav_interop_ohcodec_opengl.so.2.0.0'),
        'libqtav_interop_ohcodec_opengl.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/interop/ohcodec_vulkan/libqtav_interop_ohcodec_vulkan.so.2.0.0'),
        'libqtav_interop_ohcodec_vulkan.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'examples/ohos/libqtav_player.so'),
        'libqtav_player.so'
    ),
    @(
        $LibCxxShared,
        'libc++_shared.so'
    )
)

foreach ($output in $NativeOutputs) {
    if (-not (Test-Path -LiteralPath $output[0])) {
        throw "Missing OHOS native build output: $($output[0])"
    }
}

if ([IO.Path]::GetFullPath($TemplateRoot) -ne $ProjectRoot) {
    $SourceCopies = @(
        'entry/build-profile.json5',
        'entry/hvigorfile.ts',
        'entry/oh-package.json5',
        'entry/oh-package-lock.json5',
        'entry/src/main/cpp/types/libqtav_player/index.d.ts',
        'entry/src/main/cpp/types/libqtav_player/oh-package.json5',
        'entry/src/main/ets/entryability/EntryAbility.ets',
        'entry/src/main/ets/pages/Index.ets',
        'entry/src/main/module.json5',
        'entry/src/main/resources/base/element/color.json',
        'entry/src/main/resources/base/element/string.json',
        'entry/src/main/resources/base/media/app_icon.svg',
        'entry/src/main/resources/base/profile/main_pages.json'
    )
    foreach ($relativePath in $SourceCopies) {
        $source = Join-Path $TemplateRoot $relativePath
        $destination = Join-Path $ProjectRoot $relativePath
        $destinationDirectory = Split-Path -Parent $destination
        New-Item -ItemType Directory -Force `
            -Path $destinationDirectory | Out-Null
        Copy-Item -Force -LiteralPath $source -Destination $destination
    }
}

$LibrariesDirectory = Join-Path $ProjectRoot 'entry/libs/arm64-v8a'
New-Item -ItemType Directory -Force `
    -Path $LibrariesDirectory | Out-Null
foreach ($output in $NativeOutputs) {
    Copy-Item -Force -LiteralPath $output[0] `
        -Destination (Join-Path $LibrariesDirectory $output[1])
}

if ($NoPackage) {
    Write-Host "Staged OHOS player libraries: $LibrariesDirectory"
    return
}

$Hvigor =
    'C:/Program Files/Huawei/DevEco Studio/tools/hvigor/bin/hvigorw.bat'
if (-not (Test-Path -LiteralPath $Hvigor)) {
    throw "DevEco Studio Hvigor was not found: $Hvigor"
}
$DevEcoJavaHome = 'C:/Program Files/Huawei/DevEco Studio/jbr'
if (-not (Test-Path -LiteralPath $DevEcoSdkHome)) {
    throw "DevEco Studio SDK was not found: $DevEcoSdkHome"
}
if (-not (Test-Path -LiteralPath `
        (Join-Path $DevEcoJavaHome 'bin/java.exe'))) {
    throw "DevEco Studio JBR was not found: $DevEcoJavaHome"
}

$PreviousDevEcoSdkHome = [Environment]::GetEnvironmentVariable(
    'DEVECO_SDK_HOME',
    'Process')
$PreviousJavaHome = [Environment]::GetEnvironmentVariable(
    'JAVA_HOME',
    'Process')
$PreviousPath = $env:Path
$env:DEVECO_SDK_HOME = $DevEcoSdkHome
$env:JAVA_HOME = $DevEcoJavaHome
$env:Path = (Join-Path $DevEcoJavaHome 'bin') + ';' + $env:Path
Push-Location $ProjectRoot
try {
    & $Hvigor `
        --mode module `
        -p product=default `
        -p module=entry@default `
        -p buildMode=debug `
        assembleHap `
        --no-daemon
    if ($LASTEXITCODE -ne 0) {
        throw 'Hvigor OHOS player HAP packaging failed'
    }
} finally {
    Pop-Location
    [Environment]::SetEnvironmentVariable(
        'DEVECO_SDK_HOME',
        $PreviousDevEcoSdkHome,
        'Process')
    [Environment]::SetEnvironmentVariable(
        'JAVA_HOME',
        $PreviousJavaHome,
        'Process')
    $env:Path = $PreviousPath
}

$HapDirectory = Join-Path $ProjectRoot `
    'entry/build/default/outputs/default'
$Hap = Get-ChildItem -File -LiteralPath $HapDirectory -Filter '*.hap' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $Hap) {
    throw "Hvigor produced no HAP under $HapDirectory"
}
Write-Host "HAP: $($Hap.FullName)"
Write-Host 'This script never installs the HAP. Configure signing first, then run deployment only after explicit confirmation.'
