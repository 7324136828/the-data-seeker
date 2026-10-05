$patterns = @(
    'ak_live_',
    'ak_test_',
    'BEGIN PRIVATE KEY',
    'password\s*[:=]\s*"[^"]+"',
    'df_token_'
)

$files = Get-ChildItem -Recurse -File | Where-Object {
    $_.FullName -notmatch '\\\.git\\' -and
    $_.FullName -notmatch '\\build\\' -and
    $_.FullName -notmatch '\\dist\\' -and
    $_.FullName -notmatch '\\\.gemini\\' -and
    $_.Extension -notmatch '\.(exe|obj|pdb|zip|ilk|tlog|idb|res|lib|ico)$'
}

Write-Host "Scanning $($files.Count) files across repository..."

$findings = @()
foreach ($f in $files) {
    $lines = Get-Content $f.FullName -ErrorAction SilentlyContinue
    if ($lines) {
        for ($i = 0; $i -lt $lines.Count; $i++) {
            foreach ($p in $patterns) {
                if ($lines[$i] -match $p) {
                    $rel = (Resolve-Path -Relative $f.FullName)
                    $findings += [PSCustomObject]@{
                        File = $rel
                        Line = ($i + 1)
                        Pattern = $p
                    }
                    Write-Host "Match in $rel : $($i + 1) -> $p"
                }
            }
        }
    }
}

Write-Host "`nTotal findings: $($findings.Count)"
