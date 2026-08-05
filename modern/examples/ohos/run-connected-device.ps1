[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$BuildDirectory,
    [string]$BundleName = 'com.qtav.core.ohos',
    [int]$TimeoutSeconds = 45,
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExampleRoot = $PSScriptRoot
if (-not $ProjectRoot) {
    $ProjectRoot = Join-Path $ExampleRoot 'hap'
}
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)

if (-not $SkipBuild) {
    $BuildArguments = @('-ProjectRoot', $ProjectRoot)
    if ($BuildDirectory) {
        $BuildArguments += @('-BuildDirectory', $BuildDirectory)
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
do {
    Start-Sleep -Seconds 1
    $RelevantLog = (& $Hdc shell hilog -x 2>&1 | Out-String)
    if ($RelevantLog -match 'QTAV_OHOS_RESULT FAIL') {
        $Lines = $RelevantLog -split "`r?`n" |
            Where-Object { $_ -match 'QtAVCoreOHOS|QTAV_OHOS_RESULT' }
        $Lines | ForEach-Object { Write-Host $_ }
        throw 'Connected OHOS QtAVCore validation reported FAIL'
    }
} while (
    $RelevantLog -notmatch 'QTAV_OHOS_RESULT PASS' -and
    [DateTime]::UtcNow -lt $Deadline)

$Lines = $RelevantLog -split "`r?`n" |
    Where-Object { $_ -match 'QtAVCoreOHOS|QTAV_OHOS_RESULT' }
$Lines | ForEach-Object { Write-Host $_ }
if ($RelevantLog -notmatch 'QTAV_OHOS_RESULT PASS') {
    throw "Connected OHOS validation timed out after $TimeoutSeconds seconds"
}
Write-Host 'QtAVCore OHOS connected-device validation: PASS'
