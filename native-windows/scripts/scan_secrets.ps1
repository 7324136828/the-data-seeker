param([string]$Root='', [string]$ReportPath='', [switch]$FailOnFindings, [switch]$SkipEstablishedScanner)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (-not $Root) {$Root=Join-Path $PSScriptRoot '..'}
$Root=(Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
# This script never prints or reports matched values, source excerpts, or encodings.
$rules=@(
    @{Category='private-key';Pattern='-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'},
    @{Category='cloud-access-key';Pattern='\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'},
    @{Category='provider-token';Pattern='\b(?:ghp_|gho_|github_pat_|sk_live_|rk_live_|xox[baprs]-)[A-Za-z0-9_-]{16,}\b'},
    @{Category='credential-assignment';Pattern='(?i)\b(?:password|passwd|client_secret|api_key|apikey|access_token|auth_token)["'']?\s*\]?\s*[:=]\s*["''][^"''\r\n]{8,}["'']'},
    @{Category='authorization-literal';Pattern='(?i)(?:Bearer|Basic)\s+[A-Za-z0-9+/_.=-]{16,}'},
    @{Category='connection-credential';Pattern='(?i)(?:mongodb(?:\+srv)?|postgres(?:ql)?|mysql)://[^\s/:]+:[^\s/@]+@'}
)
$binaryExtensions=@('.exe','.dll','.lib','.obj','.pdb','.zip','.7z','.ico','.png','.jpg','.jpeg','.gif','.woff','.woff2','.ttf','.eot','.pdf','.sqlite','.db','.res','.tlog','.idb','.ilk','.dmp','.mp4')
$excludedDirectories=@('.git','.vs','build','dist','node_modules','.venv','venv','__pycache__','downloaded_docs')
$files=New-Object 'System.Collections.Generic.List[object]'
$pending=New-Object 'System.Collections.Generic.Stack[string]'
$pending.Push($Root)
while ($pending.Count) {
    foreach ($child in @(Get-ChildItem -LiteralPath $pending.Pop() -Force)) {
        if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) {continue}
        if ($child.PSIsContainer) {if ($child.Name -notin $excludedDirectories) {$pending.Push($child.FullName)}}
        elseif ($child.Extension.ToLowerInvariant() -notin $binaryExtensions -and $child.Length -le 8MB) {$files.Add($child)}
    }
}
$findings=New-Object 'System.Collections.Generic.List[object]'
foreach ($file in $files) {
    $relative=$file.FullName.Substring($Root.Length+1).Replace('\','/')
    $lines=@(Get-Content -LiteralPath $file.FullName -Encoding UTF8)
    for ($i=0;$i -lt $lines.Count;++$i) {foreach ($rule in $rules) {if ($lines[$i] -match $rule.Pattern) {$findings.Add([pscustomobject]@{File=$relative;Line=$i+1;Category=$rule.Category;Detection='conservative-pattern';Status='review-required'})}}}
}
$establishedStatus='Established scanner intentionally skipped for this pass.'
if (-not $SkipEstablishedScanner) {
    $gitleaks=Get-Command 'gitleaks.exe','gitleaks' -ErrorAction SilentlyContinue | Select-Object -First 1
    $trufflehog=Get-Command 'trufflehog.exe','trufflehog' -ErrorAction SilentlyContinue | Select-Object -First 1
    $establishedStatus='Gitleaks unavailable; no established scanner ran.'
    if ($trufflehog) {$establishedStatus+=' Trufflehog found but compatible redaction options not verified.'} else {$establishedStatus+=' Trufflehog unavailable.'}
    if ($gitleaks) {
        $helpText=(& $gitleaks.Source dir --help 2>$null | Out-String)
        if ($helpText -match '--redact' -and $helpText -match '--report-path') {
            $safeReport=Join-Path ([IO.Path]::GetTempPath()) ('dataforge-gitleaks-'+[guid]::NewGuid().ToString('N')+'.json')
            try {
                & $gitleaks.Source dir $Root --redact=100 --no-banner --report-format json --report-path $safeReport *> $null
                $scanExit=$LASTEXITCODE
                if ($scanExit -notin @(0,1)) {throw "Gitleaks failed with exit $scanExit."}
                if (Test-Path -LiteralPath $safeReport) {foreach ($entry in @(Get-Content -LiteralPath $safeReport -Raw | ConvertFrom-Json)) {$findings.Add([pscustomobject]@{File=$entry.File;Line=$entry.StartLine;Category=$entry.RuleID;Detection='gitleaks-redacted';Status='review-required'})}}
                $establishedStatus="Redacted Gitleaks current-files scan exit $scanExit; Git history not scanned."
            } finally {if (Test-Path -LiteralPath $safeReport) {Remove-Item -LiteralPath $safeReport -Force}}
        } else {$establishedStatus='Gitleaks found but compatible redaction flags unverified; not run.'}
    }
}
$result=[pscustomobject]@{AuditDate=(Get-Date -Format 'yyyy-MM-dd');Root=$Root;Scope='Current files <=8MB, excluding Git/history, generated/build/distribution/dependency caches, downloaded external reference documentation, binary files and reparse points.';FilesScanned=$files.Count;EstablishedScanner=$establishedStatus;Findings=@($findings.ToArray())}
if ($ReportPath) {$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8}
Write-Host "Current-files credential pattern scan: $($files.Count) text files, $($findings.Count) locations requiring classification."
Write-Host $establishedStatus
if ($findings.Count) {$findings.ToArray() | Select-Object File,Line,Category,Detection | Format-Table -AutoSize}
if ($FailOnFindings -and $findings.Count) {throw 'Credential scan requires classification; matched values were not printed.'}