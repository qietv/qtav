[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$BuildDirectory,
    [string]$MediaSource,
    [string]$H264MediaSource,
    [string]$HEVCMediaSource,
    [int]$HEVCMediaDurationSeconds = 0,
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

if ($MediaSource) {
    if ($H264MediaSource) {
        throw '-MediaSource and -H264MediaSource cannot be used together'
    }
    $H264MediaSource = $MediaSource
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
$PackagedH264Media = Join-Path `
    $RawFileDirectory 'qtav-ohos-test-h264.mp4'
$PackagedHEVCMedia = Join-Path `
    $RawFileDirectory 'qtav-ohos-test-hevc.mp4'

$Ffmpeg = $null
if (-not $H264MediaSource -or -not $HEVCMediaSource -or
    $HEVCMediaDurationSeconds -gt 0) {
    $Ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if (-not $Ffmpeg) {
        throw 'A host ffmpeg executable is required to generate HAP test media'
    }
}

function Stage-QtAVOhosMedia {
    param(
        [string]$Source,
        [Parameter(Mandatory)]
        [string]$Destination,
        [Parameter(Mandatory)]
        [ValidateSet('H264', 'HEVC')]
        [string]$Codec,
        [int]$DurationSeconds = 0
    )

    if ($Source) {
        $Source = [IO.Path]::GetFullPath($Source)
        if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
            throw "The requested $Codec test media does not exist: $Source"
        }
        if ($DurationSeconds -le 0) {
            Copy-Item -Force -LiteralPath $Source -Destination $Destination
            return
        }
        if (-not $Ffmpeg) {
            throw 'A host ffmpeg executable is required to trim HAP test media'
        }
        & $Ffmpeg.Source `
            -hide_banner `
            -loglevel error `
            -y `
            -i $Source `
            -t $DurationSeconds `
            -map '0:v:0' `
            -map '0:a?' `
            -c copy `
            -strict unofficial `
            -movflags '+faststart' `
            $Destination
        if ($LASTEXITCODE -ne 0) {
            throw "Host FFmpeg could not trim the $Codec HAP test media"
        }
        return
    }

    $VideoArguments = if ($Codec -eq 'H264') {
        @(
            '-c:v', 'libx264',
            '-preset', 'veryfast',
            '-crf', '18',
            '-g', '30',
            '-keyint_min', '30',
            '-sc_threshold', '0'
        )
    } else {
        @(
            '-c:v', 'libx265',
            '-preset', 'fast',
            '-crf', '22',
            '-tag:v', 'hvc1',
            '-x265-params',
            'pools=1:frame-threads=1:wpp=0:log-level=error:keyint=30:min-keyint=30:scenecut=0'
        )
    }
    $ToneFrequency = if ($Codec -eq 'H264') { 440 } else { 660 }
    $FfmpegArguments = @(
        '-hide_banner',
        '-loglevel', 'error',
        '-y',
        '-f', 'lavfi',
        '-i', 'testsrc2=size=320x180:rate=30',
        '-f', 'lavfi',
        '-i', "sine=frequency=${ToneFrequency}:sample_rate=48000",
        '-t', '6',
        '-threads', '1'
    ) + $VideoArguments + @(
        '-pix_fmt', 'yuv420p',
        '-c:a', 'aac',
        '-b:a', '96k',
        '-movflags', '+faststart',
        '-shortest',
        $Destination
    )
    & $Ffmpeg.Source @FfmpegArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Host FFmpeg could not generate the $Codec HAP test media"
    }
}

Stage-QtAVOhosMedia `
    -Source $H264MediaSource `
    -Destination $PackagedH264Media `
    -Codec H264
Stage-QtAVOhosMedia `
    -Source $HEVCMediaSource `
    -Destination $PackagedHEVCMedia `
    -Codec HEVC `
    -DurationSeconds $HEVCMediaDurationSeconds

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
