# Packages DataForge Studio for production release
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [string]$Version = "1.0.0"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot "common.ps1")

$repoRoot = Get-RepoRoot
$binDir = Join-Path $repoRoot "build\bin\$Platform\$Configuration"
$exePath = Join-Path $binDir "DataForgeStudio.exe"

if (-not (Test-Path $exePath)) {
    Write-Warning "DataForgeStudio.exe not found at $exePath"
    Write-Host "Triggering Release build..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -Platform $Platform
}

if (-not (Test-Path $exePath)) {
    throw "Executable does not exist: $exePath"
}

$packageName = "DataForgeStudio-v$Version-windows-$Platform"
$distDir = Join-Path $repoRoot "dist"
$stageDir = Join-Path $distDir $packageName
$zipPath = Join-Path $distDir "$packageName.zip"

Write-Host "==========================================================" -ForegroundColor Green
Write-Host " Packaging DataForge Studio Release ($packageName)" -ForegroundColor Green
Write-Host " Staging: $stageDir" -ForegroundColor Gray
Write-Host " Target:  $zipPath" -ForegroundColor Gray
Write-Host "==========================================================" -ForegroundColor Green

if (Test-Path $stageDir) {
    Remove-Item -Recurse -Force $stageDir
}
if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}

New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

# Copy binaries and assets
Copy-Item $exePath -Destination $stageDir
Copy-Item (Join-Path $repoRoot "README.md") -Destination $stageDir -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md") -Destination $stageDir -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repoRoot "config.example.json") -Destination $stageDir -ErrorAction SilentlyContinue

# Create ZIP archive
Write-Host "Creating ZIP release archive..." -ForegroundColor Cyan
Compress-Archive -Path "$stageDir\*" -DestinationPath $zipPath -CompressionLevel Optimal

Write-Host "`nRelease packaging completed successfully!" -ForegroundColor Green
Write-Host "ZIP Archive: $zipPath ($(([System.IO.FileInfo]::new($zipPath)).Length) bytes)" -ForegroundColor Yellow
Get-ChildItem $stageDir | Select-Object Name, Length | Format-Table -AutoSize
