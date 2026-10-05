param([ValidateSet('Debug','Release')][string]$Configuration='Release',[ValidateSet('x64')][string]$Platform='x64',[string]$VisualStudioPath='', [string]$Toolset='v145',[string]$SdkVersion='10.0.26100.0',[switch]$Rebuild)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$repoRoot=Get-RepoRoot
$toolchain=Get-NativeToolchain -VisualStudioPath $VisualStudioPath -Toolset $Toolset -SdkVersion $SdkVersion
$target='Build'
if ($Rebuild) {$target='Rebuild'}
Write-Host "Building isolated native DataForge Studio $Configuration | $Platform ($Toolset, MSVC $($toolchain.ToolsVersion), SDK $SdkVersion)"
$buildArguments=@((Join-Path $repoRoot 'cpp\DataForgeStudio.sln'),"-t:$target","-p:Configuration=$Configuration","-p:Platform=$Platform","-p:PlatformToolset=$Toolset","-p:VCToolsVersion=$($toolchain.ToolsVersion)","-p:WindowsTargetPlatformVersion=$SdkVersion",'-v:minimal','-m:2','-nologo')
$snapshotDirectory=Assert-ManagedBuildPath (Join-Path $repoRoot 'build\verification')
New-Item -ItemType Directory -Path $snapshotDirectory -Force | Out-Null
$before=@(Get-NativeInputSnapshot)
if (@($before | Where-Object {$_.Length -eq 0}).Count) {throw 'An empty native input was detected. Restore source before building.'}
$before | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $snapshotDirectory "native-inputs-$Configuration-$Platform-before.json") -Encoding UTF8
# Keep private exact input snapshots to recover from concurrent truncation.
# These ZIPs contain only the cpp source/project/resource allowlist, never user data.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sourceZip=Join-Path $snapshotDirectory ("native-source-$Configuration-$Platform-"+[guid]::NewGuid().ToString('N')+'.zip')
$zipStream=$null
$archive=$null
try {
    $zipStream=[IO.File]::Open($sourceZip,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $archive=New-Object -TypeName System.IO.Compression.ZipArchive -ArgumentList @($zipStream,[System.IO.Compression.ZipArchiveMode]::Create,$false)
    foreach ($input in $before) {
        $path=Join-Path $repoRoot $input.File
        $bytes=[IO.File]::ReadAllBytes($path)
        $sha=[Security.Cryptography.SHA256]::Create()
        try {$hash=[BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','')}
        finally {$sha.Dispose()}
        if ($hash -ne $input.Sha256) {throw 'Native source changed while creating the private input snapshot.'}
        $entry=$archive.CreateEntry($input.File,[IO.Compression.CompressionLevel]::Optimal)
        $entryStream=$entry.Open()
        try {$entryStream.Write($bytes,0,$bytes.Length)} finally {$entryStream.Dispose()}
    }
} finally {if ($archive) {$archive.Dispose()};if ($zipStream) {$zipStream.Dispose()}}
Write-Host "Private source input snapshot: $sourceZip"
$compileFailure=$null
try {Invoke-CheckedNativeTool -Executable $toolchain.MSBuild -Arguments $buildArguments} catch {$compileFailure=$_}
finally {$after=@(Get-NativeInputSnapshot);$after | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $snapshotDirectory "native-inputs-$Configuration-$Platform-after.json") -Encoding UTF8}
$changed=@(Compare-Object -ReferenceObject @($before | ForEach-Object {$_.File+'|'+$_.Sha256}) -DifferenceObject @($after | ForEach-Object {$_.File+'|'+$_.Sha256}))
if ($changed.Count) {throw 'Native inputs changed during MSBuild. Stop concurrent edits and rebuild. Location/hash-only snapshots are in build/verification.'}
if ($compileFailure) {throw $compileFailure}
$exe=Join-Path $repoRoot "build\bin\$Platform\$Configuration\DataForgeStudio.exe"
if (-not (Test-Path -LiteralPath $exe)) {throw 'MSBuild succeeded without creating the expected application.'}
Write-Host "Stable native inputs verified ($($before.Count) files). Build verified: $exe"
