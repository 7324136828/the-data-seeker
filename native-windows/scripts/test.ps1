param([ValidateSet('Debug','Release')][string]$Configuration='Release',[ValidateSet('x64')][string]$Platform='x64',[string]$VisualStudioPath='', [string]$Toolset='v145',[string]$SdkVersion='10.0.26100.0',[switch]$SkipBuild)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $SkipBuild) {& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Platform $Platform -VisualStudioPath $VisualStudioPath -Toolset $Toolset -SdkVersion $SdkVersion}
$testExe=Join-Path (Get-RepoRoot) "build\bin\$Platform\$Configuration\Tests.exe"
$guiExe=Join-Path (Get-RepoRoot) "build\bin\$Platform\$Configuration\DataForgeStudio.exe"
if (-not (Test-Path -LiteralPath $testExe) -or -not (Test-Path -LiteralPath $guiExe)) {throw 'Native test/application executable is missing.'}
Write-Host "Testing native $Configuration | $Platform"
Invoke-CheckedNativeTool -Executable $testExe
Write-Host 'Testing native editor controls, database view and theme contrast'
Invoke-CheckedNativeTool -Executable $guiExe -Arguments @('--self-test') -Gui
Write-Host 'Native GUI self-test passed (exit 0).'