param([ValidateSet('Debug','Release')][string]$Configuration='Release',[ValidateSet('x64')][string]$Platform='x64',[switch]$BuildIfMissing,[switch]$NoWait,[string]$VisualStudioPath='', [string]$Toolset='v145',[string]$SdkVersion='10.0.26100.0')
& (Join-Path $PSScriptRoot 'native-windows/run.ps1') @PSBoundParameters
