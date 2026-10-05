param([ValidateSet('Release')][string]$Configuration='Release',[ValidateSet('x64')][string]$Platform='x64',[ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?$')][string]$Version='1.2.0',[string]$VisualStudioPath='', [string]$Toolset='v145',[string]$SdkVersion='10.0.26100.0')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$repoRoot=Get-RepoRoot
$toolchain=Get-NativeToolchain -VisualStudioPath $VisualStudioPath -Toolset $Toolset -SdkVersion $SdkVersion
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Platform $Platform -VisualStudioPath $VisualStudioPath -Toolset $Toolset -SdkVersion $SdkVersion
& (Join-Path $PSScriptRoot 'test.ps1') -Configuration $Configuration -Platform $Platform -SkipBuild
if (-not (Test-Path -LiteralPath $toolchain.Redistributable)) {throw 'Official local x64 Visual C++ Redistributable missing; no runtime is silently downloaded.'}
$signature=Get-AuthenticodeSignature -LiteralPath $toolchain.Redistributable
if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {throw 'Official runtime signature verification failed.'}
$packageName="DataForgeStudio-v$Version-windows-$Platform"
$distDir=[IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
if ((Test-Path -LiteralPath $distDir) -and ((Get-Item -LiteralPath $distDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw 'Refusing redirected dist directory.'}
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
$stage=Assert-ManagedBuildPath (Join-Path $repoRoot ('build\package-stage-'+[guid]::NewGuid().ToString('N')))
New-Item -ItemType Directory -Path $stage | Out-Null
$zip=Join-Path $distDir "$packageName.zip"
$tempZip=Join-Path $distDir ($packageName+'.pending-'+[guid]::NewGuid().ToString('N')+'.zip')
try {
    $releaseDir=Join-Path $repoRoot "build\bin\$Platform\Release"
    $application=Join-Path $releaseDir 'DataForgeStudio.exe'
    $actualVersion=(Get-Item -LiteralPath $application).VersionInfo.ProductVersion
    if ($actualVersion -ne ($Version+'.0')) {throw 'Requested package version does not match the built executable resource.'}
    $runtimeDlls=@('duckdb.dll','bson2.dll','mongoc2.dll')
    $licenseFiles=@('duckdb-LICENSE.txt','mongo-c-driver-COPYING.txt','mongo-c-driver-THIRD_PARTY_NOTICES.txt','quickjs-ng-LICENSE.txt','yaml-cpp-LICENSE.txt','yaml-cpp-dragonbox-NOTICE.txt')
    Copy-Item -LiteralPath $application -Destination $stage
    foreach ($dll in $runtimeDlls) {$source=Join-Path $releaseDir $dll;if (-not (Test-Path -LiteralPath $source) -or (Get-Item -LiteralPath $source).Length -le 0) {throw "Required native provider DLL missing: $dll"};Copy-Item -LiteralPath $source -Destination $stage}
    foreach ($file in @('README.md','LICENSE','THIRD_PARTY_NOTICES.md','config.example.json','secrets.md')) {$source=Join-Path $repoRoot $file;if (-not (Test-Path -LiteralPath $source)) {throw "Required release document missing: $file"};Copy-Item -LiteralPath $source -Destination $stage}
    $docDir=Join-Path $stage 'docs'
    New-Item -ItemType Directory -Path $docDir | Out-Null
    foreach ($doc in @('architecture.md','feature-parity.md','migration-notes.md','verification.md')) {$source=Join-Path $repoRoot "docs\$doc";if (-not (Test-Path -LiteralPath $source)) {throw "Release documentation missing: $doc"};Copy-Item -LiteralPath $source -Destination $docDir}
    $licenseDir=Join-Path $stage 'licenses'
    New-Item -ItemType Directory -Path $licenseDir | Out-Null
    foreach ($license in $licenseFiles) {$source=Join-Path $repoRoot "licenses\$license";if (-not (Test-Path -LiteralPath $source)) {throw "Required dependency license missing: $license"};Copy-Item -LiteralPath $source -Destination $licenseDir}
    $runtimeDir=Join-Path $stage 'runtime'
    New-Item -ItemType Directory -Path $runtimeDir | Out-Null
    Copy-Item -LiteralPath $toolchain.Redistributable -Destination $runtimeDir
    $instructions=@"
DataForge Studio $Version - native Windows x64

Extract the complete ZIP. If the current Microsoft Visual C++ x64 runtime is
absent, run runtime\vc_redist.x64.exe and review Microsoft's installer terms.
Runtime installation is explicit. Start DataForgeStudio.exe on Windows 10/11
x64 (Windows 10 version 1809 or newer). The application requires no Python,
React, Node.js, browser or PowerShell for ordinary operation. The optional
mock API server is implemented inside the native application. Keep the three
provider DLLs beside the executable. Remote SQL connectors require the
appropriate vendor's Windows x64 ODBC driver; SQLite and DuckDB are included.
User data is stored under Local AppData; removing this portable application
folder does not erase that data. This ZIP contains an unsigned application.
Build: $Toolset; MSVC tools $($toolchain.ToolsVersion); SDK $SdkVersion; Release x64 /MD.
Deployment test limitations are recorded in docs\verification.md.
"@
    Set-Content -LiteralPath (Join-Path $stage 'START-HERE.txt') -Value $instructions -Encoding UTF8
    & (Join-Path $PSScriptRoot 'scan_secrets.ps1') -Root $stage -FailOnFindings -SkipEstablishedScanner
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $tempZip -CompressionLevel Optimal
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=[IO.Compression.ZipFile]::OpenRead($tempZip)
    try {
        $names=@($archive.Entries | ForEach-Object {$_.FullName.Replace('\','/')})
        $expectedFiles=@('DataForgeStudio.exe','README.md','LICENSE','THIRD_PARTY_NOTICES.md','config.example.json','secrets.md','START-HERE.txt','docs/architecture.md','docs/feature-parity.md','docs/migration-notes.md','docs/verification.md','runtime/vc_redist.x64.exe')+@($runtimeDlls)+@($licenseFiles | ForEach-Object {'licenses/'+$_})
        $fileNames=@($names | Where-Object {-not $_.EndsWith('/')})
        if (@(Compare-Object ($expectedFiles | Sort-Object) ($fileNames | Sort-Object)).Count -or @($fileNames | Group-Object | Where-Object {$_.Count -ne 1}).Count) {throw 'Release ZIP differs from the exact public-file allowlist.'}
        foreach ($entry in $archive.Entries) {if (-not $entry.FullName.Replace('\','/').EndsWith('/') -and $entry.Length -le 0) {throw "Release ZIP contains an empty file: $($entry.FullName)"}}
        if (@($names | Where-Object {$_ -match '(^|[/\\])\.\.(?:[/\\]|$)|(^|[/\\])(?:\.env|config\.local\.json|credentials|logs|history|databases)(?:[/\\]|$)|\.(pdb|dmp|obj|lib)$'}).Count) {throw 'Unexpected private/build artifact in ZIP.'}
    } finally {$archive.Dispose()}
    $extracted=Assert-ManagedBuildPath (Join-Path $stage 'verification-extracted')
    [IO.Compression.ZipFile]::ExtractToDirectory($tempZip,$extracted)
    Write-Host 'Verifying the extracted release application and bundled native provider DLLs'
    Invoke-CheckedNativeTool -Executable (Join-Path $extracted 'DataForgeStudio.exe') -Arguments @('--self-test') -Gui
    Move-Item -LiteralPath $tempZip -Destination $zip -Force
    $hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath ($zip+'.sha256') -Encoding ASCII -Value "$hash  $packageName.zip"
    Write-Host "Verified Release package: $zip"
    Write-Host "SHA256: $hash"
} finally {
    if (Test-Path -LiteralPath $tempZip) {Remove-Item -LiteralPath $tempZip -Force}
    $checked=Assert-ManagedBuildPath $stage
    if (Test-Path -LiteralPath $checked) {Remove-Item -LiteralPath $checked -Recurse -Force}
}
