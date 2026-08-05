[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$BuildDirectory,
    [string]$MediaSource,
    [int]$Parallel = [Environment]::ProcessorCount,
    [switch]$SkipQtAVBuild,
    [switch]$NoPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExampleRoot = $PSScriptRoot
$ModernRoot = (Resolve-Path (Join-Path $ExampleRoot '../..')).Path
$RepositoryRoot = (Resolve-Path (Join-Path $ModernRoot '..')).Path
$TemplateRoot = Join-Path $ExampleRoot 'hap'
if (-not $ProjectRoot) {
    $ProjectRoot = $TemplateRoot
}
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $RepositoryRoot 'build/modern-ohos-hap'
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)

if (-not (Test-Path (Join-Path $ProjectRoot 'build-profile.json5'))) {
    throw "The HAP project is incomplete: $ProjectRoot"
}

if (-not $SkipQtAVBuild) {
    & (Join-Path $ModernRoot 'scripts/build-ohos.ps1') `
        -SkipDependencies `
        -NoInstall `
        -BuildExamples `
        -BuildDirectory $BuildDirectory `
        -Parallel $Parallel
    if ($LASTEXITCODE -ne 0) {
        throw 'QtAVCore OHOS example build failed'
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
            'backends/audio/ohaudio/libqtav_audio_ohaudio.so.2.0.0'),
        'libqtav_audio_ohaudio.so.2'
    ),
    @(
        (Join-Path $BuildDirectory `
            'backends/hwaccel/ohcodec/libqtav_hw_ohcodec.so.2.0.0'),
        'libqtav_hw_ohcodec.so.2'
    ),
    @(
        (Join-Path $BuildDirectory 'examples/ohos/libentry.so'),
        'libentry.so'
    )
)
foreach ($output in $NativeOutputs) {
    if (-not (Test-Path -LiteralPath $output[0])) {
        throw "Missing OHOS native build output: $($output[0])"
    }
}

if ([IO.Path]::GetFullPath($TemplateRoot) -ne $ProjectRoot) {
    $SourceCopies = @(
        'entry/src/main/ets/pages/Index.ets',
        'entry/src/main/cpp/types/libentry/index.d.ts',
        'entry/src/main/cpp/types/libentry/oh-package.json5'
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

$RawFileDirectory = Join-Path `
    $ProjectRoot 'entry/src/main/resources/rawfile'
New-Item -ItemType Directory -Force -Path $RawFileDirectory | Out-Null
$PackagedMedia = Join-Path $RawFileDirectory 'qtav-ohos-test.mp4'
if ($MediaSource) {
    $MediaSource = [IO.Path]::GetFullPath($MediaSource)
    if (-not (Test-Path -LiteralPath $MediaSource)) {
        throw "The requested test media does not exist: $MediaSource"
    }
    Copy-Item -Force -LiteralPath $MediaSource -Destination $PackagedMedia
} else {
    $Ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if (-not $Ffmpeg) {
        throw 'A host ffmpeg executable is required to generate HAP test media'
    }
    & $Ffmpeg.Source `
        -hide_banner `
        -loglevel error `
        -y `
        -f lavfi `
        -i 'testsrc2=size=320x180:rate=30' `
        -f lavfi `
        -i 'sine=frequency=440:sample_rate=48000' `
        -t 1 `
        -c:v libx264 `
        -preset veryfast `
        -crf 18 `
        -pix_fmt yuv420p `
        -c:a aac `
        -b:a 96k `
        -shortest `
        $PackagedMedia
    if ($LASTEXITCODE -ne 0) {
        throw 'Host FFmpeg could not generate the HAP test media'
    }
}

if ($NoPackage) {
    return
}

$Hvigor = 'C:/Program Files/Huawei/DevEco Studio/tools/hvigor/bin/hvigorw.bat'
if (-not (Test-Path -LiteralPath $Hvigor)) {
    throw "DevEco Studio Hvigor was not found: $Hvigor"
}
$DevEcoSdkHome = 'C:/Program Files/Huawei/DevEco Studio/sdk'
$DevEcoJavaHome = 'C:/Program Files/Huawei/DevEco Studio/jbr'
if (-not (Test-Path -LiteralPath $DevEcoSdkHome)) {
    throw "DevEco Studio SDK was not found: $DevEcoSdkHome"
}
if (-not (Test-Path -LiteralPath (Join-Path $DevEcoJavaHome 'bin/java.exe'))) {
    throw "DevEco Studio JBR was not found: $DevEcoJavaHome"
}
$PreviousDevEcoSdkHome = [Environment]::GetEnvironmentVariable(
    'DEVECO_SDK_HOME', 'Process')
$PreviousJavaHome = [Environment]::GetEnvironmentVariable(
    'JAVA_HOME', 'Process')
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
        throw 'Hvigor HAP packaging failed'
    }
} finally {
    Pop-Location
    [Environment]::SetEnvironmentVariable(
        'DEVECO_SDK_HOME', $PreviousDevEcoSdkHome, 'Process')
    [Environment]::SetEnvironmentVariable(
        'JAVA_HOME', $PreviousJavaHome, 'Process')
    $env:Path = $PreviousPath
}

$HapDirectory = Join-Path $ProjectRoot `
    'entry/build/default/outputs/default'
$Hap = Get-ChildItem -File -LiteralPath $HapDirectory -Filter '*.hap' |
    Sort-Object `
        @{ Expression = {
            $_.Name -match '(?i)(^|-)signed\.hap$'
        }; Descending = $true },
        LastWriteTime -Descending |
    Select-Object -First 1
if (-not $Hap) {
    throw "Hvigor produced no HAP under $HapDirectory"
}
Write-Host "HAP: $($Hap.FullName)"
