# Builds the DataForge Studio Visual Studio solution
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
$solutionPath = Join-Path $repoRoot "cpp\DataForgeStudio.sln"
$msBuild = Find-MSBuild

Write-Host "==========================================================" -ForegroundColor Green
Write-Host " Building DataForge Studio ($Configuration | $Platform)" -ForegroundColor Green
Write-Host " Solution: $solutionPath" -ForegroundColor Gray
Write-Host " MSBuild:  $msBuild" -ForegroundColor Gray
Write-Host "==========================================================" -ForegroundColor Green

$args = @(
    $solutionPath,
    "-p:Configuration=$Configuration",
    "-p:Platform=$Platform",
    "-v:minimal",
    "-m"
)

Invoke-CheckedNativeTool -Executable $msBuild -Arguments $args

Write-Host "`nBuild completed successfully!" -ForegroundColor Green
Write-Host "Output: $repoRoot\build\bin\$Platform\$Configuration\DataForgeStudio.exe" -ForegroundColor Yellow
