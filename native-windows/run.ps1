param([ValidateSet('Debug','Release')][string]$Configuration='Release',[ValidateSet('x64')][string]$Platform='x64',[switch]$BuildIfMissing,[switch]$NoWait,[string]$VisualStudioPath='', [string]$Toolset='v145',[string]$SdkVersion='10.0.26100.0')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts\common.ps1')
$exe=Join-Path (Get-RepoRoot) "build\bin\$Platform\$Configuration\DataForgeStudio.exe"
if (-not (Test-Path -LiteralPath $exe) -and $BuildIfMissing) {& (Join-Path $PSScriptRoot 'scripts\build.ps1') -Configuration $Configuration -Platform $Platform -VisualStudioPath $VisualStudioPath -Toolset $Toolset -SdkVersion $SdkVersion}
if (-not (Test-Path -LiteralPath $exe)) {throw "Application missing. Run build.bat -Configuration $Configuration -Platform $Platform, or use -BuildIfMissing."}
Write-Host "Launching $exe"
$process=Start-Process -FilePath $exe -WorkingDirectory ([IO.Path]::GetDirectoryName($exe)) -PassThru
if (-not $NoWait) {$process.WaitForExit();exit $process.ExitCode}