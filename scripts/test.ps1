# Runs the native test suite for DataForge Studio
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot
$testExe = Join-Path $repoRoot "build\bin\$Platform\$Configuration\Tests.exe"

if (-not (Test-Path $testExe)) {
    Write-Warning "Tests.exe not found at: $testExe"
    Write-Host "Triggering build first..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -Platform $Platform
}

if (-not (Test-Path $testExe)) {
    throw "Test executable not found: $testExe"
}

Write-Host "==========================================================" -ForegroundColor Green
Write-Host " Running DataForge Studio Test Suite ($Configuration | $Platform)" -ForegroundColor Green
Write-Host " Binary: $testExe" -ForegroundColor Gray
Write-Host "==========================================================" -ForegroundColor Green

Invoke-CheckedNativeTool -Executable $testExe -Arguments @()

Write-Host "`nAll tests completed successfully!" -ForegroundColor Green
