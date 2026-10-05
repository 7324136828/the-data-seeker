# Secrets audit

Audit date: 2026-10-04. This report contains locations and classifications only; no credential values, source excerpts or reversible encodings.

The current native source scan examined 439 text files under `native-windows/`, including active vendored source/licenses and the 1.2.1 ER implementation, and reported five locations. It returned exit 0 in review mode; all five findings remain variable-reference templates or deliberately synthetic regression fixtures. No live credential was established. The current-code 1.2.1 Release stage scan examined 16 public text files with zero findings; its package/extraction checks are recorded in `docs/verification.md`.

The original application audit below is historical evidence from the earlier migration, when `original-project/` was available: 63 text files, five classified locations, exit 0 in review mode. That directory is absent from the current workspace, so the original-source scan was not repeated for 1.2.1. Its findings are retained as an audit record, not a claim about current original files.

| Repository-relative file | Line | Category | Classification | Action | Status |
|---|---:|---|---|---|---|
| native-windows/cpp/core/src/OpenApiParser.cpp | 117 | Credential assignment | Generated variable-reference template for bearer authentication; confirmed by the core owner | Retain template | Classified placeholder |
| native-windows/cpp/tests/TestRunner.cpp | 458 | Credential assignment | Synthetic protected-collection persistence regression fixture; confirmed by the core owner | Retain test | Classified test fixture |
| native-windows/cpp/tests/TestRunner.cpp | 487 | Credential assignment | Synthetic history-redaction regression fixture; confirmed by the core owner | Retain test | Classified test fixture |
| native-windows/cpp/tests/DbProviderTests.cpp | 89 | Credential assignment | Synthetic protected-profile fixture for DPAPI and session-only credential checks; confirmed by the core owner | Retain test | Classified test fixture |
| native-windows/cpp/tests/DbProviderTests.cpp | 92 | Credential-bearing connection URL | Synthetic profile/diagnostic-scrubbing fixture; confirmed by the core owner | Retain test | Classified test fixture |
| original-project/backend/openapi_parser.py | 117 | Credential assignment | Two variable-reference placeholder defaults | Historical record; excluded from native Release ZIP | Classified placeholder |
| original-project/backend/tests/test_api_engine.py | 193 | Credential assignment | Synthetic fixture inside `test_history_redacts_literal_credentials` | Retain original test; excluded from native Release ZIP | Classified test fixture |
| original-project/backend/tests/test_api_engine.py | 195 | Credential assignment | Synthetic fixture inside `test_history_redacts_literal_credentials` | Retain original test; excluded from native Release ZIP | Classified test fixture |
| original-project/backend/tests/test_api_engine.py | 284 | Authorization literal | Synthetic fixture inside `test_script_credentials_are_not_copied_into_history` | Retain original test; excluded from native Release ZIP | Classified test fixture |
| original-project/backend/tests/test_db_engine.py | 237 | Credential assignment | Synthetic fixture inside `test_connection_profiles_do_not_persist_secrets` | Retain original test; excluded from native Release ZIP | Classified test fixture |

The scan excludes Git contents/history, build/dist products, dependency caches, binaries, reparse points, files larger than 8 MB and directories named `downloaded_docs`. The historical original audit excluded its downloaded external reference corpus; that corpus is not redistributed. The previous root C++ worktree and its conflicting-edit recovery copies are outside this isolated candidate audit. No Git history scan or rewrite was performed.

Gitleaks and Trufflehog were checked and neither was available; an established scanner was not run. Conservative checks cover provider/cloud tokens, private keys, credential assignments, authorization literals and credential-bearing connection URLs. Findings require human classification. There were no confirmed live credentials in the checks performed; this limited pattern audit is not a guarantee that credentials are absent.

Release packaging uses an exact allowlist of the native executable, three provider DLLs, six dependency license/notice files, official signed Microsoft redistributable, public config example and documentation. It excludes user settings, credentials, databases, inputs, logs, source snapshots, debug binaries and dumps. Staged text files are rescanned with failure on any match. Native persistent credential protection uses DPAPI according to the architecture notes, including request scripts and free-form descriptions. No credential rotation or history rewriting was performed.

Repeat the current location-only audit from this isolated project:

```powershell
powershell.exe -NoLogo -NoProfile -File scripts/scan_secrets.ps1 -ReportPath build/candidate-secret-locations.json
```

If the original source is restored, scan it separately with `-Root ../original-project` and a report path under `build/`. Reports remain under the ignored isolated build directory and contain location/category/status only. A synthetic scanner fixture in a managed path containing spaces and Japanese characters verified a nonzero failure and that its generated token never appeared in captured output.
