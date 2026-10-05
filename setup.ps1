param([ValidateSet('Debug','Release')][string]$Configuration='Release',[ValidateSet('x64')][string]$Platform='x64',[string]$VisualStudioPath='', [string]$Toolset='v145',[string]$SdkVersion='10.0.26100.0')
& (Join-Path $PSScriptRoot 'native-windows/setup.ps1') @PSBoundParameters
