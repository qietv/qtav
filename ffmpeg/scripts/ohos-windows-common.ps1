function Assert-QtavOhosWindowsHost {
    if ($env:OS -ne 'Windows_NT') {
        throw 'This OHOS PowerShell entry point can run only on Windows.'
    }
    if (-not [Environment]::Is64BitOperatingSystem) {
        throw 'The OHOS build requires 64-bit Windows.'
    }
    if (-not [Environment]::Is64BitProcess) {
        throw 'Run the OHOS build from a 64-bit PowerShell process.'
    }
}

function Resolve-QtavOhosSdkRoot {
    param(
        [string]$SdkRoot
    )

    $Candidates = @()
    if ($SdkRoot) {
        $Candidates += $SdkRoot
    }
    if ($env:OHOS_SDK_ROOT) {
        $Candidates += $env:OHOS_SDK_ROOT
    }
    if ($env:OHOS_NDK) {
        $Candidates += Split-Path -Parent $env:OHOS_NDK
    }
    if ($env:DEVECO_SDK_HOME) {
        $Candidates += Join-Path $env:DEVECO_SDK_HOME 'default/openharmony'
    }
    if (${env:ProgramFiles}) {
        $Candidates += Join-Path ${env:ProgramFiles} `
            'Huawei/DevEco Studio/sdk/default/openharmony'
    }

    foreach ($Candidate in $Candidates | Select-Object -Unique) {
        if (-not $Candidate) {
            continue
        }
        $Toolchain = Join-Path $Candidate 'native/build/cmake/ohos.toolchain.cmake'
        if (Test-Path -LiteralPath $Toolchain -PathType Leaf) {
            return (Resolve-Path -LiteralPath $Candidate).Path
        }
    }

    throw @'
The OpenHarmony native SDK was not found. Pass -SdkRoot with the directory
that contains native/, or set OHOS_SDK_ROOT or DEVECO_SDK_HOME.
'@
}

function Use-QtavOhosSdkWithoutSpaces {
    param(
        [Parameter(Mandatory)]
        [string]$SdkRoot,

        [Parameter(Mandatory)]
        [string]$AliasRoot
    )

    if ($SdkRoot -notmatch '\s') {
        return $SdkRoot
    }

    $AliasRoot = [IO.Path]::GetFullPath($AliasRoot)
    $AliasParent = Split-Path -Parent $AliasRoot
    New-Item -ItemType Directory -Path $AliasParent -Force | Out-Null

    if (Test-Path -LiteralPath $AliasRoot) {
        $Alias = Get-Item -LiteralPath $AliasRoot -Force
        if (-not ($Alias.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "OHOS SDK alias exists but is not a junction: $AliasRoot"
        }
        $AliasTarget = @($Alias.Target) | Select-Object -First 1
        if (-not $AliasTarget) {
            throw "Could not resolve the OHOS SDK junction target: $AliasRoot"
        }
        $ResolvedAliasTarget = [IO.Path]::GetFullPath($AliasTarget).TrimEnd('\')
        $ResolvedSdkRoot = [IO.Path]::GetFullPath($SdkRoot).TrimEnd('\')
        if (-not $ResolvedAliasTarget.Equals(
                $ResolvedSdkRoot,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw @"
OHOS SDK alias already points to a different SDK:
  alias:  $AliasRoot
  target: $ResolvedAliasTarget
Remove the alias or choose a different -WorkRoot before changing SDKs.
"@
        }
    } else {
        New-Item -ItemType Junction -Path $AliasRoot -Target $SdkRoot | Out-Null
    }

    $AliasedToolchain = Join-Path $AliasRoot `
        'native/build/cmake/ohos.toolchain.cmake'
    if (-not (Test-Path -LiteralPath $AliasedToolchain -PathType Leaf)) {
        throw "The aliased OHOS toolchain is not readable: $AliasedToolchain"
    }
    return $AliasRoot
}

function Enable-QtavOhosWindowsBuildTools {
    param(
        [Parameter(Mandatory)]
        [string]$SdkRoot
    )

    $BuildTools = Join-Path $SdkRoot 'native/build-tools/cmake/bin'
    $CMake = Join-Path $BuildTools 'cmake.exe'
    $Ninja = Join-Path $BuildTools 'ninja.exe'
    if (-not (Test-Path -LiteralPath $CMake -PathType Leaf)) {
        $CMakeCommand = Get-Command cmake.exe -CommandType Application `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $CMakeCommand) {
            throw 'cmake.exe was not found in the OHOS SDK or on PATH.'
        }
        $CMake = $CMakeCommand.Source
    }
    if (-not (Test-Path -LiteralPath $Ninja -PathType Leaf)) {
        $NinjaCommand = Get-Command ninja.exe -CommandType Application `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $NinjaCommand) {
            throw 'ninja.exe was not found in the OHOS SDK or on PATH.'
        }
        $Ninja = $NinjaCommand.Source
    }

    $ToolDirectories = @(
        (Split-Path -Parent $CMake)
        (Split-Path -Parent $Ninja)
        (Join-Path $SdkRoot 'native/llvm/bin')
    ) | Select-Object -Unique
    $PathEntries = $env:PATH -split [IO.Path]::PathSeparator
    foreach ($ToolDirectory in $ToolDirectories) {
        if ($PathEntries -notcontains $ToolDirectory) {
            $env:PATH = "$ToolDirectory$([IO.Path]::PathSeparator)$env:PATH"
        }
    }

    return [pscustomobject]@{
        CMake = $CMake
        Ninja = $Ninja
    }
}
