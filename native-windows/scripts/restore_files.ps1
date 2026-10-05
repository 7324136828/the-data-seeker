$logPath = "C:\Users\zachn\.gemini\antigravity\brain\89a7d209-cce7-433d-a405-ae21207616b0\.system_generated\logs\transcript_full.jsonl"
$lines = Get-Content $logPath
Write-Host "Total lines in transcript:" $lines.Count

$targets = @(
    "HeaderBar.cpp",
    "ApiClientView.cpp",
    "ApiClientView.h",
    "DbStudioView.cpp",
    "DbStudioView.h",
    "App.cpp",
    "DataForgeStudio.vcxproj.filters"
)

foreach ($target in $targets) {
    $restored = $false
    # Search backwards for the most recent write_to_file or view_file containing the full file
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        $line = $lines[$i]
        if ($line.Contains("write_to_file") -and $line.Contains($target)) {
            try {
                $json = $line | ConvertFrom-Json
                if ($json.tool_calls) {
                    foreach ($tc in $json.tool_calls) {
                        if ($tc.name -eq "write_to_file" -and $tc.args.TargetFile.EndsWith($target)) {
                            $dest = Join-Path $PSScriptRoot "..\cpp\app\$target"
                            [System.IO.File]::WriteAllText($dest, $tc.args.CodeContent, [System.Text.Encoding]::UTF8)
                            Write-Host "Restored $target from write_to_file at line $i (Size: $($tc.args.CodeContent.Length) chars)"
                            $restored = $true
                            break
                        }
                    }
                }
            } catch {
                # continue
            }
            if ($restored) { break }
        }
    }

    if (-not $restored) {
        Write-Host "write_to_file not found for $target, searching in view_file..."
        # If not write_to_file, check view_file outputs
        for ($i = $lines.Count - 1; $i -ge 0; $i--) {
            $line = $lines[$i]
            if ($line.Contains($target) -and $line.Contains("The following code has been modified to include a line number")) {
                try {
                    $json = $line | ConvertFrom-Json
                    if ($json.content -and $json.content.Contains("The following code has been modified to include a line number")) {
                        # Extract lines by stripping line numbers
                        $content = $json.content
                        $startIdx = $content.IndexOf("1: ")
                        if ($startIdx -ge 0) {
                            $codePart = $content.Substring($startIdx)
                            $endNote = $codePart.IndexOf("The above content")
                            if ($endNote -ge 0) {
                                $codePart = $codePart.Substring(0, $endNote)
                            }
                            $cleanLines = @()
                            foreach ($cl in ($codePart -split "`r?`n")) {
                                if ($cl -match '^\d+:\s?(.*)$') {
                                    $cleanLines += $Matches[1]
                                }
                            }
                            $code = $cleanLines -join "`r`n"
                            $dest = Join-Path $PSScriptRoot "..\cpp\app\$target"
                            [System.IO.File]::WriteAllText($dest, $code, [System.Text.Encoding]::UTF8)
                            Write-Host "Restored $target from view_file at line $i (Size: $($code.Length) chars)"
                            $restored = $true
                            break
                        }
                    }
                } catch {
                    # continue
                }
                if ($restored) { break }
            }
        }
    }
}

