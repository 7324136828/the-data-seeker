# Common utilities for DataForge Studio native build automation

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Find-MSBuild {
    # 1. Check vswhere
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
        if ($vsPath -and (Test-Path $vsPath)) {
            $msBuildCandidate = Join-Path $vsPath "MSBuild\Current\Bin\amd64\MSBuild.exe"
            if (Test-Path $msBuildCandidate) {
                return $msBuildCandidate
            }
            $msBuildCandidate = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $msBuildCandidate) {
                return $msBuildCandidate
            }
        }
    }

    # 2. Check well-known Visual Studio / Build Tools 2026/2022 paths
    $wellKnown = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
    )

    foreach ($path in $wellKnown) {
        if (Test-Path $path) {
            return $path
        }
    }

    # 3. Check PATH
    $cmd = Get-Command "MSBuild.exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    throw "MSBuild.exe could not be found. Please ensure Visual Studio or Visual Studio Build Tools with C++ workload is installed."
}

function Invoke-CheckedNativeTool {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Executable,
        [string[]] $Arguments = @()
    )

    Write-Host "[EXEC] $Executable $($Arguments -join ' ')" -ForegroundColor Cyan
    & $Executable @Arguments
    $toolExitCode = $LASTEXITCODE
    if ($toolExitCode -ne 0) {
        throw "Native tool failed with exit code ${toolExitCode}: $Executable"
    }
}
