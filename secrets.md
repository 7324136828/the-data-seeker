# Secrets audit

Audit date: 2026-10-04. No secret values, excerpts, reversible encodings, or raw scanner reports are stored here.

The initial conservative current-files scan in `scripts/scan_secrets.ps1` examined 143 text files in the checkout, including the ignored original application's backend/frontend source, native code, project/resource files, scripts, local examples and documentation. The tracked-file inventory contained only LICENSE and .gitignore at the beginning of this task; ignoring a file does not remove it from Git history.

Excluded scope: `.git` contents/history, generated build/dist products, dependency caches (node_modules and virtual environments), reparse points, binary files, files larger than 8 MB, and the downloaded external reference documentation corpus under `original-project/requirement/downloaded_docs/`. That reference corpus contains approximately 10,000 third-party web pages/assets and is not redistributed. Git history was not scanned or rewritten. Gitleaks and Trufflehog were checked and neither was installed; an established scanner was not run. This is a limited pattern audit, not a guarantee that no credentials exist.

| File | Line | Category | Detection and classification | Action | Status |
|---|---:|---|---|---|---|
| original-project/backend/tests/test_api_engine.py | 284 | Authorization literal | Conservative pattern; synthetic literal inside `test_script_credentials_are_not_copied_into_history` | Preserve this credential-redaction test fixture; exclude original-project from the native package | Classified test data; no live credential established |
| cpp/tests/TestRunner.cpp | 396 | Credential assignment | Conservative pattern; synthetic credential fixture for protected collection persistence | Preserve regression fixture; native tests verify plaintext is absent from disk | Classified test data; no live credential established |
| cpp/tests/TestRunner.cpp | 415 | Credential assignment | Conservative pattern; synthetic credential fixture for sanitized history persistence | Preserve regression fixture; native tests verify sensitive request values are removed | Classified test data; no live credential established |

No confirmed live credentials were found in the checks performed. No credential was copied into native source and no credential rotation or history rewrite was performed. Application-created user credentials are outside this repository audit; their native persistence design is documented in the architecture notes.

Release packaging uses an explicit allowlist and performs a new scan of staged text files with failures on unclassified matches. It excludes local databases, history, inputs, logs, credentials, debug binaries, dumps and source snapshots. Update this report after the final native build and release-stage scan; actual final counts and package checks appear in `docs/verification.md`.

To repeat the location-only current-files audit:

```powershell
powershell.exe -NoLogo -NoProfile -File scripts/scan_secrets.ps1 -ReportPath build/secret-audit-locations.json
```

The optional JSON report contains relative paths, line numbers, categories and statuses only, and is kept in the ignored build directory. Findings require classification; this command does not print matched values or automatically redact source.

