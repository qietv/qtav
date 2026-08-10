Set-StrictMode -Version Latest

function Assert-QtavCiWindowsHost {
    if ($env:OS -ne 'Windows_NT') {
        throw 'The QtAVCore CI PowerShell drivers require Windows.'
    }
    if (-not [Environment]::Is64BitOperatingSystem -or
        -not [Environment]::Is64BitProcess) {
        throw 'Run the QtAVCore CI drivers in 64-bit PowerShell on 64-bit Windows.'
    }
}

function Resolve-QtavCiCommand {
    param(
        [Parameter(Mandatory)]
        [string]$Name
    )

    $Command = Get-Command $Name -CommandType Application `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $Command) {
        throw "Required command was not found: $Name"
    }
    return $Command.Source
}

function Resolve-QtavCiWindowsCMake {
    $Command = Get-Command 'cmake.exe' -CommandType Application `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($Command) {
        return $Command.Source
    }

    $Candidates = @()
    if (${env:ProgramFiles}) {
        $Candidates += Get-ChildItem -Path (
            Join-Path ${env:ProgramFiles} `
                'Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
        ) -File -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -ExpandProperty FullName
    }
    $CMake = $Candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $CMake) {
        throw 'cmake.exe was not found on PATH or in Visual Studio.'
    }
    return $CMake
}

function Invoke-QtavCiCommand {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter(Mandatory)]
        [object[]]$ArgumentList,

        [Parameter(Mandatory)]
        [string]$Description
    )

    Write-Host "::group::$Description"
    try {
        & $FilePath @ArgumentList
        if ($LASTEXITCODE -ne 0) {
            throw "$Description failed with exit code $LASTEXITCODE"
        }
    } finally {
        Write-Host '::endgroup::'
    }
}

function Write-QtavCiToolVersion {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,

        [string[]]$ArgumentList = @('--version')
    )

    Write-Host "Tool: $FilePath"
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Tool version probe failed: $FilePath"
    }
}

function Invoke-QtavCiDependencyVerification {
    param(
        [Parameter(Mandatory)]
        [string]$CMake,

        [Parameter(Mandatory)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory)]
        [string]$InstallRoot,

        [Parameter(Mandatory)]
        [string]$Triplet
    )

    Invoke-QtavCiCommand `
        -FilePath $CMake `
        -ArgumentList @(
            "-DINSTALL_ROOT=$InstallRoot"
            "-DTRIPLET=$Triplet"
            '-P'
            (Join-Path $RepositoryRoot 'ffmpeg/cmake/verify-install.cmake')
        ) `
        -Description "Verify $Triplet dependency package"
}
