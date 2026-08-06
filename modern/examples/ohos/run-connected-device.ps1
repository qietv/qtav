[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$BuildDirectory,
    [string]$H264MediaSource,
    [string]$HEVCMediaSource,
    [int]$HEVCMediaDurationSeconds = 0,
    [string]$BundleName = 'com.qtav.core.ohos',
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
Write-Host 'QtAVCore OHOS connected-device validation: PASS'
