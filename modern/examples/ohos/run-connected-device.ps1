[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$BuildDirectory,
    [string]$H264MediaSource,
    [string]$HEVCMediaSource,
    [string]$VVCMediaSource,
    [int]$HEVCMediaDurationSeconds = 0,
    [string]$BundleName,
    [ValidateSet(0, 5, 84)]
    [int]$RequireDolbyVisionProfile = 0,
    [int]$TimeoutSeconds = 90,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExampleRoot = $PSScriptRoot
if (-not $ProjectRoot) {
    $ProjectRoot = Join-Path $ExampleRoot 'hap'
}
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)

if ($VVCMediaSource) {
    $VVCMediaSource = [IO.Path]::GetFullPath($VVCMediaSource)
    if (-not (Test-Path -LiteralPath $VVCMediaSource -PathType Leaf)) {
        throw "The requested VVC test media does not exist: $VVCMediaSource"
    }
    $VVCProbe = Get-Command ffprobe -ErrorAction SilentlyContinue
    if (-not $VVCProbe) {
        throw 'ffprobe is required to validate VVC test media'
    }
    $VVCProbeOutput = @(
        & $VVCProbe.Source `
            -v error `
            -select_streams 'v:0' `
            -show_entries `
                'stream=codec_name,codec_tag_string,width,height,r_frame_rate,nb_frames' `
            -of json `
            $VVCMediaSource
    )
    if ($LASTEXITCODE -ne 0) {
        throw "ffprobe could not inspect VVC media: $VVCMediaSource"
    }
    $VVCProbeData = ($VVCProbeOutput -join "`n") | ConvertFrom-Json
    $VVCStream = @($VVCProbeData.streams) | Select-Object -First 1
    if (-not $VVCStream -or
        $VVCStream.codec_name -ne 'vvc' -or
        $VVCStream.codec_tag_string -ne 'vvc1' -or
        [int]$VVCStream.width -ne 1280 -or
        [int]$VVCStream.height -ne 720 -or
        $VVCStream.r_frame_rate -ne '60/1' -or
        [int]$VVCStream.nb_frames -ne 600) {
        throw ('VVC validation requires the complete 600-frame ' +
            '1280x720/60 vvc1 sample')
    }
    Write-Host 'VVC 600-frame media preflight: PASS'
}

if (-not $BundleName) {
    $AppConfigPath = Join-Path $ProjectRoot 'AppScope/app.json5'
    $AppConfig = Get-Content -LiteralPath $AppConfigPath -Raw
    $BundleNameMatch = [regex]::Match(
        $AppConfig,
        '"bundleName"\s*:\s*"([^"\r\n]+)"')
    if (-not $BundleNameMatch.Success) {
        throw "Could not read bundleName from $AppConfigPath"
    }
    $BundleName = $BundleNameMatch.Groups[1].Value
}

if ($RequireDolbyVisionProfile -ne 0) {
    if (-not $HEVCMediaSource) {
        throw '-RequireDolbyVisionProfile requires -HEVCMediaSource'
    }
    $Probe = Get-Command ffprobe -ErrorAction SilentlyContinue
    if (-not $Probe) {
        throw 'ffprobe is required to validate Dolby Vision test media'
    }
    $ProbeOutput = @(
        & $Probe.Source `
            -v error `
            -select_streams 'v:0' `
            -show_entries stream_side_data `
            -of json `
            $HEVCMediaSource
    )
    if ($LASTEXITCODE -ne 0) {
        throw "ffprobe could not inspect Dolby Vision media: $HEVCMediaSource"
    }
    $ProbeData = ($ProbeOutput -join "`n") | ConvertFrom-Json
    $Stream = @($ProbeData.streams) | Select-Object -First 1
    $SideData = @(
        $Stream |
            Select-Object `
                -ExpandProperty side_data_list `
                -ErrorAction SilentlyContinue
    )
    $Dovi = $SideData |
        Where-Object { $_.side_data_type -eq 'DOVI configuration record' } |
        Select-Object -First 1
    $ExpectedProfile = if ($RequireDolbyVisionProfile -eq 84) { 8 } else { 5 }
    $ExpectedCompatibility = if ($RequireDolbyVisionProfile -eq 84) { 4 } else { 0 }
    if (-not $Dovi -or
        [int]$Dovi.dv_profile -ne $ExpectedProfile -or
        [int]$Dovi.dv_bl_signal_compatibility_id -ne $ExpectedCompatibility -or
        [int]$Dovi.rpu_present_flag -ne 1 -or
        [int]$Dovi.el_present_flag -ne 0 -or
        [int]$Dovi.bl_present_flag -ne 1) {
        throw ("The requested Profile $RequireDolbyVisionProfile validation " +
            'requires a residual-disabled base-layer sample with an RPU and ' +
            "compatibility id $ExpectedCompatibility")
    }
    Write-Host ("Dolby Vision Profile $RequireDolbyVisionProfile media " +
        'preflight: PASS')
}

if (-not $SkipBuild) {
    $BuildArguments = @{
        ProjectRoot = $ProjectRoot
    }
    if ($BuildDirectory) {
        $BuildArguments.BuildDirectory = $BuildDirectory
    }
    if ($H264MediaSource) {
        $BuildArguments.H264MediaSource = $H264MediaSource
    }
    if ($HEVCMediaSource) {
        $BuildArguments.HEVCMediaSource = $HEVCMediaSource
    }
    if ($VVCMediaSource) {
        $BuildArguments.VVCMediaSource = $VVCMediaSource
    }
    if ($HEVCMediaDurationSeconds -gt 0) {
        $BuildArguments.HEVCMediaDurationSeconds =
            $HEVCMediaDurationSeconds
    }
    & (Join-Path $ExampleRoot 'build-ohos-hap.ps1') @BuildArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'OHOS HAP build failed'
    }
}

$Hdc = 'C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe'
if (-not (Test-Path -LiteralPath $Hdc)) {
    throw "DevEco Studio HDC was not found: $Hdc"
}
$Targets = @(
    @(& $Hdc list targets) |
        Where-Object { $_ -and $_ -notmatch '\[Empty\]' }
)
if ($Targets.Count -ne 1) {
    throw "Exactly one connected OHOS device is required; found $($Targets.Count)"
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
    throw "No packaged HAP exists under $HapDirectory"
}

& $Hdc install -r $Hap.FullName
if ($LASTEXITCODE -ne 0) {
    throw ('HAP installation failed. If the device is showing an approval ' +
        'prompt, approve it manually before running this script again.')
}

& $Hdc shell hilog -r | Out-Null
& $Hdc shell aa force-stop $BundleName | Out-Null
& $Hdc shell aa start -a EntryAbility -b $BundleName
if ($LASTEXITCODE -ne 0) {
    throw "Could not launch $BundleName/EntryAbility"
}

$Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$RelevantLog = ''
$LifecycleOrchestrated = $false
do {
    Start-Sleep -Seconds 1
    $RelevantLog = (& $Hdc shell hilog -x 2>&1 | Out-String)
    if ($RelevantLog -match 'QTAV_OHOS_RESULT FAIL') {
        $Lines = $RelevantLog -split "`r?`n" |
            Where-Object {
                $_ -match 'QtAVCoreOHOS|QTAV_OHOS_RESULT|QTAV_OHOS_LIFECYCLE'
            }
        $Lines | ForEach-Object { Write-Host $_ }
        throw 'Connected OHOS QtAVCore validation reported FAIL'
    }
    if (-not $LifecycleOrchestrated -and
        $RelevantLog -match 'QTAV_OHOS_LIFECYCLE BACKGROUND_REQUEST') {
        $LifecycleOrchestrated = $true
        Write-Host 'QtAVCore requested a real background/foreground cycle'
        & $Hdc shell uitest uiInput keyEvent Home
        if ($LASTEXITCODE -ne 0) {
            throw 'Could not send the HOME key to background the OHOS app'
        }
        $MinimumBackgroundUntil = [DateTime]::UtcNow.AddSeconds(1)
        $ReadyToForeground = $false
        do {
            Start-Sleep -Milliseconds 250
            $RelevantLog = (& $Hdc shell hilog -x 2>&1 | Out-String)
            if ($RelevantLog -match 'QTAV_OHOS_RESULT FAIL') {
                $Lines = $RelevantLog -split "`r?`n" |
                    Where-Object {
                        $_ -match 'QtAVCoreOHOS|QTAV_OHOS_RESULT|QTAV_OHOS_LIFECYCLE'
                    }
                $Lines | ForEach-Object { Write-Host $_ }
                throw 'Connected OHOS QtAVCore validation reported FAIL'
            }
            $BackgroundObserved = $RelevantLog -match `
                'QTAV_OHOS_LIFECYCLE BACKGROUND_OBSERVED'
            $SurfaceRemoved = $RelevantLog -match `
                'QTAV_OHOS_CHECKPOINT OHCODEC_SURFACE_REMOVED'
            $ReadyToForeground = $BackgroundObserved -and
                $SurfaceRemoved -and
                [DateTime]::UtcNow -ge $MinimumBackgroundUntil
        } while (-not $ReadyToForeground -and
                 [DateTime]::UtcNow -lt $Deadline)
        if (-not $ReadyToForeground) {
            throw ('OHOS background transition did not report both ' +
                'BACKGROUND_OBSERVED and OHCODEC_SURFACE_REMOVED before ' +
                'the validation deadline')
        }
        & $Hdc shell aa start -a EntryAbility -b $BundleName
        if ($LASTEXITCODE -ne 0) {
            throw "Could not return $BundleName/EntryAbility to the foreground"
        }
    }
} while (
    $RelevantLog -notmatch 'QTAV_OHOS_RESULT PASS' -and
    [DateTime]::UtcNow -lt $Deadline)

$Lines = $RelevantLog -split "`r?`n" |
    Where-Object {
        $_ -match 'QtAVCoreOHOS|QTAV_OHOS_RESULT|QTAV_OHOS_LIFECYCLE'
    }
$Lines | ForEach-Object { Write-Host $_ }
if ($RelevantLog -notmatch 'QTAV_OHOS_RESULT PASS') {
    throw "Connected OHOS validation timed out after $TimeoutSeconds seconds"
}
if ($RequireDolbyVisionProfile -ne 0) {
    $DoviMatch = [regex]::Match(
        $RelevantLog,
        'QTAV_OHOS_OHCODEC_OPENGL_RESULT PASS[^\r\n]*hevcRendered=(\d+)[^\r\n]*doviQueued=(\d+)[^\r\n]*doviMatched=(\d+)[^\r\n]*doviReleased=(\d+)')
    if (-not $DoviMatch.Success) {
        throw ('OHOS Dolby Vision validation did not emit the required ' +
            'OHCodec/OpenGL RPU correlation counters')
    }
    $HevcRendered = [uint64]$DoviMatch.Groups[1].Value
    $DoviQueued = [uint64]$DoviMatch.Groups[2].Value
    $DoviMatched = [uint64]$DoviMatch.Groups[3].Value
    $DoviReleased = [uint64]$DoviMatch.Groups[4].Value
    if ($HevcRendered -eq 0 -or
        $DoviQueued -ne $HevcRendered -or
        $DoviMatched -ne $HevcRendered -or
        $DoviReleased -ne $HevcRendered) {
        throw ("OHOS Dolby Vision Profile $RequireDolbyVisionProfile " +
            "RPU/output correlation failed: rendered=$HevcRendered " +
            "queued=$DoviQueued matched=$DoviMatched " +
            "released=$DoviReleased")
    }
    Write-Host ("QtAVCore OHOS Dolby Vision Profile " +
        "$RequireDolbyVisionProfile validation: PASS " +
        "(rendered=$HevcRendered, RPU released=$DoviReleased)")
}
if ($VVCMediaSource) {
    $VVCMatch = [regex]::Match(
        $RelevantLog,
        'QTAV_OHOS_VVC_RESULT PASS frames=600 decoder=([^\s]+) wrapper=vvc_ohcodec[^\r\n]*eos=1[^\r\n]*pauseResume=1[^\r\n]*seek=1[^\r\n]*flush=1[^\r\n]*stop=1[^\r\n]*surfaceRecreation=1[^\r\n]*staleRejected=1[^\r\n]*maxPending=1[^\r\n]*fallbackEvents=1[^\r\n]*hardwareFallback=0[^\r\n]*cpuMap=0 transfer=0 staging=0 upload=0')
    if (-not $VVCMatch.Success) {
        throw 'OHOS VVC validation did not emit the required acceptance counters'
    }
    if ($VVCMatch.Groups[1].Value -ne 'OMX.hisi.video.decoder.vvc') {
        throw ('The recorded Pura X Max did not select its named hardware ' +
            "VVC decoder: $($VVCMatch.Groups[1].Value)")
    }
    Write-Host ('QtAVCore OHOS VVC validation: PASS ' +
        "(decoder=$($VVCMatch.Groups[1].Value), frames=600)")
}
Write-Host 'QtAVCore OHOS connected-device validation: PASS'
