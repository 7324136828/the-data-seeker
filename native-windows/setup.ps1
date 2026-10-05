param([ValidateSet('Debug','Release')][string]$Configuration='Release',[ValidateSet('x64')][string]$Platform='x64',[string]$VisualStudioPath='', [string]$Toolset='v145',[string]$SdkVersion='10.0.26100.0')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts\common.ps1')
$toolchain=Get-NativeToolchain -VisualStudioPath $VisualStudioPath -Toolset $Toolset -SdkVersion $SdkVersion
Write-Host "MSBuild: $($toolchain.MSBuild)"
Write-Host "Compiler tools: $($toolchain.ToolsVersion) / $Toolset / x64; SDK: $SdkVersion"
$probeDir=Assert-ManagedBuildPath (Join-Path (Get-RepoRoot) ('build\toolchain-probe-'+[guid]::NewGuid().ToString('N')))
New-Item -ItemType Directory -Path $probeDir | Out-Null
try {
    $source=Join-Path $probeDir 'probe.cpp'
    Set-Content -LiteralPath $source -Encoding ASCII -Value @('#include <windows.h>','#include <winsqlite/winsqlite3.h>','#include <vector>','int main() { std::vector<int> values{1}; return values.empty() || sqlite3_libversion_number() <= 0; }')
    $resource=Join-Path $probeDir 'probe.rc'
    Set-Content -LiteralPath $resource -Encoding ASCII -Value '101 RCDATA { 1, 2, 3 }'
    $resFile=Join-Path $probeDir 'probe.res'
    Invoke-CheckedNativeTool -Executable $toolchain.ResourceCompiler -Arguments @('/nologo','/fo',$resFile,$resource)
    $sdkInclude=Join-Path $toolchain.SdkRoot "Include\$SdkVersion"
    $sdkLib=Join-Path $toolchain.SdkRoot "Lib\$SdkVersion"
    $probeExe=Join-Path $probeDir 'probe.exe'
    $runtime='/MD'
    if ($Configuration -eq 'Debug') {$runtime='/MDd'}
    $compileArguments=@('/nologo','/EHsc','/std:c++17','/utf-8',$runtime,'/I',(Join-Path $toolchain.ToolsRoot 'include'))
    foreach ($part in @('ucrt','shared','um')) {$compileArguments+=@('/I',(Join-Path $sdkInclude $part))}
    $compileArguments+=@(('/Fe'+$probeExe),('/Fo'+(Join-Path $probeDir 'probe.obj')),$source,$resFile,'/link',('/LIBPATH:'+(Join-Path $toolchain.ToolsRoot 'lib\x64')),('/LIBPATH:'+(Join-Path $sdkLib 'um\x64')),('/LIBPATH:'+(Join-Path $sdkLib 'ucrt\x64')),'winsqlite3.lib','kernel32.lib')
    Invoke-CheckedNativeTool -Executable $toolchain.Compiler -Arguments $compileArguments
    Invoke-CheckedNativeTool -Executable $toolchain.ManifestTool -Arguments @('-nologo','-manifest',(Join-Path (Get-RepoRoot) 'cpp\resources\DataForgeStudio.manifest'),('-outputresource:'+$probeExe+';#1'))
    Invoke-CheckedNativeTool -Executable $probeExe
    Write-Host 'Verified compiler, linker, resource compiler, manifest tool, C++17, x64 SDK libraries, and Windows SQLite runtime.'
} finally {$checked=Assert-ManagedBuildPath $probeDir;if (Test-Path -LiteralPath $checked) {Remove-Item -LiteralPath $checked -Recurse -Force}}
Write-Host "Setup verified for $Configuration | $Platform. No tools installed or existing settings overwritten. Next: build.bat -Configuration $Configuration -Platform $Platform"