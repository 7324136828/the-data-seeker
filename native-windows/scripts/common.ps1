# Windows PowerShell 5.1-compatible native build utilities.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Get-RepoRoot { (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path }
function ConvertTo-NativeArgument {
    param([AllowEmptyString()][string]$Value)
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    '"' + $escaped + '"'
}
function Invoke-CheckedNativeTool {
    param([Parameter(Mandatory = $true)][string]$Executable, [string[]]$Arguments = @(), [switch]$Gui)
    if ($Gui) {
        $startInfo = New-Object Diagnostics.ProcessStartInfo
        $startInfo.FileName=$Executable
        $startInfo.Arguments=@($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
        $startInfo.UseShellExecute=$false
        $startInfo.CreateNoWindow=$true
        $startInfo.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
        $startInfo.RedirectStandardOutput=$true
        $startInfo.RedirectStandardError=$true
        $process=New-Object Diagnostics.Process
        $process.StartInfo=$startInfo
        try {
            if (-not $process.Start()) { throw 'Native GUI test could not be started.' }
            $stdoutTask=$process.StandardOutput.ReadToEndAsync()
            $stderrTask=$process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(60000)) { $process.Kill(); [void]$process.WaitForExit(10000); throw 'Native GUI test timed out after 60 seconds; the owned test process was stopped.' }
            $stdout=$stdoutTask.GetAwaiter().GetResult()
            $stderr=$stderrTask.GetAwaiter().GetResult()
            if ($stdout) { Write-Host $stdout.TrimEnd() }
            if ($stderr) { Write-Host $stderr.TrimEnd() }
            $toolExitCode=$process.ExitCode
        } finally { $process.Dispose() }
    } else { & $Executable @Arguments; $toolExitCode=$LASTEXITCODE }
    if ($toolExitCode -ne 0) { throw "Native tool failed with exit code ${toolExitCode}: $Executable" }
}
function Assert-ManagedBuildPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $root=[IO.Path]::GetFullPath((Join-Path (Get-RepoRoot) 'build'))+[IO.Path]::DirectorySeparatorChar
    $full=[IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) {throw 'Refusing a path outside the isolated build directory.'}
    $cursor=$full
    while ($cursor -and $cursor.Length -ge ($root.Length-1)) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw 'Refusing a managed path containing a reparse point.'}
        $cursor=[IO.Path]::GetDirectoryName($cursor)
    }
    $full
}
function Get-NativeToolchain {
    param([string]$VisualStudioPath='', [string]$Toolset='v145', [string]$SdkVersion='10.0.26100.0')
    if ($Toolset -notmatch '^v14[0-9]$' -or $SdkVersion -notmatch '^10\.0\.[0-9]+\.0$') {throw 'Invalid MSVC toolset or Windows SDK version.'}
    $candidates=@()
    if ($VisualStudioPath) {$candidates=@([IO.Path]::GetFullPath($VisualStudioPath))}
    else {
        $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) {throw 'vswhere is missing. Install Visual Studio C++ desktop tools, or supply -VisualStudioPath.'}
        $candidates=@(& $vswhere -all -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        if ($LASTEXITCODE -ne 0) {throw 'Visual Studio discovery failed.'}
    }
    $sdkRegistry=Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
    if (-not $sdkRegistry -or -not $sdkRegistry.PSObject.Properties['KitsRoot10']) {throw 'Windows SDK installation was not found.'}
    $sdkRoot=$sdkRegistry.KitsRoot10
    foreach ($vsPath in $candidates) {
        $toolsetFiles=@(Get-ChildItem -Path (Join-Path $vsPath "MSBuild\Microsoft\VC\v*\Platforms\x64\PlatformToolsets\$Toolset\Toolset.props") -ErrorAction SilentlyContinue)
        if ($toolsetFiles.Count -eq 0) {continue}
        $versionFile=Join-Path $vsPath "VC\Auxiliary\Build\Microsoft.VCToolsVersion.$Toolset.default.txt"
        if (-not (Test-Path -LiteralPath $versionFile)) {$versionFile=Join-Path $vsPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'}
        if (-not (Test-Path -LiteralPath $versionFile)) {continue}
        $toolsVersion=(Get-Content -LiteralPath $versionFile -Raw).Trim()
        $toolsRoot=Join-Path $vsPath "VC\Tools\MSVC\$toolsVersion"
        $msbuild=Join-Path $vsPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
        $bin=Join-Path $toolsRoot 'bin\Hostx64\x64'
        $sdkBin=Join-Path $sdkRoot "bin\$SdkVersion\x64"
        $required=@($msbuild,(Join-Path $bin 'cl.exe'),(Join-Path $bin 'link.exe'),(Join-Path $bin 'dumpbin.exe'),(Join-Path $sdkBin 'rc.exe'),(Join-Path $sdkBin 'mt.exe'),(Join-Path $toolsRoot 'include\vector'),(Join-Path $toolsRoot 'lib\x64\libcmt.lib'),(Join-Path $sdkRoot "Include\$SdkVersion\um\Windows.h"),(Join-Path $sdkRoot "Include\$SdkVersion\um\winsqlite\winsqlite3.h"),(Join-Path $sdkRoot "Lib\$SdkVersion\um\x64\winsqlite3.lib"),(Join-Path $sdkRoot "Lib\$SdkVersion\ucrt\x64\ucrt.lib"))
        if (@($required | Where-Object {-not (Test-Path -LiteralPath $_)}).Count -ne 0) {continue}
        return [pscustomobject]@{VisualStudioPath=$vsPath;MSBuild=$msbuild;Toolset=$Toolset;ToolsVersion=$toolsVersion;ToolsRoot=$toolsRoot;Compiler=(Join-Path $bin 'cl.exe');Linker=(Join-Path $bin 'link.exe');Dumpbin=(Join-Path $bin 'dumpbin.exe');ResourceCompiler=(Join-Path $sdkBin 'rc.exe');ManifestTool=(Join-Path $sdkBin 'mt.exe');SdkRoot=$sdkRoot;SdkVersion=$SdkVersion;Redistributable=(Join-Path $vsPath "VC\Redist\MSVC\$toolsVersion\vc_redist.x64.exe")}
    }
    throw "No compatible x64 C++ installation has $Toolset and Windows SDK $SdkVersion. Import .vsconfig or supply validated installation/toolset/SDK overrides. Existing Win32 application does not require MFC."
}
function Find-MSBuild {(Get-NativeToolchain).MSBuild}
function Get-NativeInputSnapshot {
    $repo=Get-RepoRoot
    $extensions=@('.cpp','.c','.h','.hpp','.inc','.inl','.lib','.dll','.rc','.rc2','.ico','.manifest','.props','.targets','.sln','.vcxproj','.filters','.json')
    @(Get-ChildItem -LiteralPath (Join-Path $repo 'cpp') -Recurse -File -Force | Where-Object {$_.FullName -notmatch '[\\/]build[\\/]' -and $_.Extension.ToLowerInvariant() -in $extensions} | Sort-Object FullName | ForEach-Object {[pscustomobject]@{File=$_.FullName.Substring($repo.Length+1).Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;Length=$_.Length;LastWriteUtc=$_.LastWriteTimeUtc.ToString('o')}})
}