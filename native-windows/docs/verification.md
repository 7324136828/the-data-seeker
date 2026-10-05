# Verification

Audit date: 2026-10-04, America/New_York. DataForge Studio 1.2.1's ER repair and native application were verified on the current Windows development host in Debug x64 and Release x64. The authoritative solution and automation are under `native-windows/`. Work resumed after an unexpected shutdown; older binaries and pre-repair checkpoints are not used as final evidence.

## Actual completed checks

| Check | Observed result |
|---|---|
| Release setup probe (same toolchain) | Exit 0; compiled/linked/executed native SDK, SQLite and STL code, compiled a resource and embedded the manifest |
| Development-script parsing | All 9 scripts parsed successfully under Windows PowerShell 5.1.26100.9596 |
| Final guarded Debug build | Exit 0; all 380 native input hashes unchanged |
| Final Debug acceptance | 30 CLI groups passed, 0 failed; actual GUI self-test exit 0 |
| ER visual review | Dark/light/150% text and full overview/selected captures approved; correct row endpoints, card clearance and visible crossing gaps |
| Final clean guarded Release rebuild | Exit 0; all 380 native input hashes unchanged |
| Final Release acceptance | 30 CLI groups passed, 0 failed; actual GUI self-test exit 0 |
| Debug/Release source comparison | 380 inputs per configuration; 0 differences in relative file, SHA256 or byte length |
| Release version/dependencies | ProductVersion 1.2.1.0; native Windows/Release VC dependencies and the three bundled provider DLLs; no Python, Node, browser-wrapper or Debug runtime DLL imports |
| Current-code 1.2.1 package | Exit 0; guarded incremental Release build, 30 CLI groups and actual GUI acceptance repeated successfully |
| Public staging audit | 16 text files scanned; 0 pattern findings; exact 21-file ZIP allowlist passed |
| Extracted current-code package | The actual extracted `DataForgeStudio.exe --self-test` completed with exit 0 and native editor/database/window/theme PASS |

The final Debug source archive is `build/verification/native-source-Debug-x64-54a8b340d1484198b2b7059c173f81bd.zip`. The matching clean Release archive is `build/verification/native-source-Release-x64-9a4c1f0eb0b1410dbab328c154419eec.zip`. Before/after location/hash/length snapshots are in that ignored directory. Packaging creates additional identical-code guarded checkpoints. Private source archives and diagnostics are excluded from distribution.

The clean builds emitted upstream QuickJS C4701/C4702/C4703 initialization/unreachable warnings, yaml-cpp C4244/C4267 numeric-conversion warnings and MongoDB-header C4324 alignment warnings. A warning-free build is not claimed.

## Behavior exercised

- Native/Postman/OpenAPI JSON and YAML imports, bounded parsing, typed object/list variables, iteration metadata, duplicate/missing identities, cross-collection identity rejection and legacy compatibility. Real cURL/native HTTP round trips cover duplicate/blank/plus queries, fragment boundaries, editable overrides and disabled removal, GET bodies, literal multipart values, binary file reattachment and redirect defaults.
- Generated cURL round trips preserve URL/auth/options/multipart behavior; generated JavaScript is parsed by the compiled native QuickJS library. Python snippets are generated text and structurally checked; Python is not executed or required by the application.
- Native QuickJS request scripts exercise inherited scopes, typed iteration data, assertions, runner propagation, timeout/memory/serialized-data limits and a 512 KiB JavaScript stack budget. Real worker-thread recursion is rejected and an ordinary script succeeds afterward in the same worker. Protected persistence/reload, legacy plaintext compatibility, history redaction, free-form descriptions and request-script source are covered.
- Real SQLite and DuckDB operations cover typed NULL/text/BLOB values, generated keys, CRUD, read-only protection, transaction failure/rollback, cancellation, native configuration and full export. SQLite double display uses the shortest round-trip representation: `349.99` remains readable; high-precision, tiny, denormal, large and maximum doubles retain their exact binary values through typed JSON, export and rebinding, including negative-zero sign.
- The native Redis client talks to a real loopback RESP fixture. It covers framing, command/options validation, cancellation, read-only protection, safe partial edits and complete 250-member list/set/sorted-set/stream exports while grid previews disclose truncation. This is not a live Redis-server deployment test.
- Native ODBC metadata fixtures allow a schema-less driver or an exact configured schema, reject an omitted schema when a named namespace is returned, and exclude wildcard overmatches such as `a_b` versus `axb`. Catalog fixtures require the exact current/configured database and reject unresolved or unrelated catalogs. DDL fixtures preserve quoted identifiers, defaults, PKs, ordered composite FKs and actions, character widths and decimal precision; incomplete/truncated metadata is marked. Major-provider view lookups use bound schema/name parameters, preserve returned definitions and report unavailable definitions without inventing table DDL. These are native guard/metadata tests, not live remote ODBC sessions.
- The native SQL mock REST server is exercised over actual loopback HTTP against SQLite and DuckDB: list/record CRUD, generated IDs, bound record keys, malformed requests, framing and shutdown. Document/key-value databases remain available through their provider-native workbench operations.
- ER layout/routing fixtures cover the six-table/six-FK eCommerce schema, layered dependencies, disconnected cards, a 4,000-node cycle, variable card heights, obstacle avoidance, parallel/self/reversed links, determinism, invalid endpoints, overflow and search budgets. Actual GUI checks cover column-row endpoints, route caching, hover/selection tracing, drag/refresh/persistence, Auto arrange and wheel scrolling at the exact vertical-fit boundary. Cached crossing markers distinguish orthogonal intersections without changing connection endpoints.
- Actual Win32 GUI checks cover SQL/JavaScript/JSON/Shell highlighting, undo, no-wrap line-count/resize behavior, LF readback, SQL wildcard expansion, quoted schema-qualified aliases, dialect-safe column quoting, quoted Mongo JSON/Redis names, and script scope-key completion. Actual Ctrl+Space/Enter checks cover request-local and valid unsaved-draft script candidates. Multiple API/query documents, running-query navigation/close/result ownership, cancellation, multiline row values, themes, contrast and enlarged/narrow layouts are exercised.

The GUI runs render `api-dark.bmp`, `api-light.bmp`, `api-150.bmp`, `api-documents-dark.bmp`, `runner-dark.bmp`, `runner-150.bmp`, `sql-dark.bmp`, `sql-150.bmp`, `er-dark.bmp`, `er-light.bmp`, `er-150.bmp`, `er-overview-dark.bmp` and `er-selected-dark.bmp` under `build/bin/x64/<configuration>/ui-artifacts/`. These are real native windows with isolated sample data. Visual review confirmed the slate/light styling, readable syntax/text, bounded controls and enlarged SQL horizontal scrolling. The fresh post-format Debug SQL dark capture visibly showed `349.99` without the long-digit ellipsis. The ER overview visibly showed all six relationships at their correct column rows, clear of cards; selection emphasized the selected product table's three links. Crossing gaps distinguish the orthogonal intersections, and dark/light/enlarged views retain readable fonts and accessible scrollbars. Captures are private review artifacts and are not shipped.

## Reproduction

Run from `native-windows/`:

```powershell
.\setup.bat -Configuration Release -Platform x64
.\build.bat -Configuration Debug -Platform x64
.\test.bat -Configuration Debug -Platform x64 -SkipBuild
.\build.bat -Configuration Release -Platform x64 -Rebuild
.\test.bat -Configuration Release -Platform x64 -SkipBuild
.\package.bat -Configuration Release -Platform x64 -Version 1.2.1
```

Builds cap MSBuild at two project workers and C++ compilation at `/MP2`, reject empty inputs and compare hashes before/after compilation. GUI tests wait for the owned process and check its actual exit code; launching a window is not counted as acceptance.

## Deployment and audit boundaries

The package is `dist/DataForgeStudio-v1.2.1-windows-x64.zip`; its final SHA256 is in the adjacent `.sha256` file. Its 21 public files contain the executable, `duckdb.dll`, `bson2.dll`, `mongoc2.dll`, six dependency licenses/notices, the official VC redistributable, public configuration and documentation. The exact allowlist excludes user settings/credentials/databases, source snapshots, logs, dumps, debug binaries and intermediates. The three provider DLLs must stay beside the executable; remote SQL profiles require separately installed compatible x64 vendor ODBC drivers.

Toolchain: Visual Studio Build Tools 2026 18.9.2; MSBuild 18.9.1+a81b43525; v145; MSVC tools 14.51.36231 (compiler/linker file version 14.51.36256.0); Windows SDK 10.0.26100.0. Debug uses `/MDd`; Release uses `/MD`. The bundled local x64 VC redistributable has ProductVersion 14.51.36247.0 and a Valid Microsoft Corporation signature. No toolchain/runtime was silently installed. The application itself is unsigned.

The frozen native current-files pattern audit examined 439 text files and classified five template/synthetic-fixture locations. The earlier original-source audit examined 63 files and classified five; it is historical evidence because `original-project/` is absent from the current workspace and was not rescanned for 1.2.1. No live credential was established in the checks performed. Exact locations and exclusions are in `secrets.md`. Gitleaks and Trufflehog were unavailable; Git history, old conflicting root-worktree copies and downloaded external reference documentation were not scanned. Public staged text is rescanned with failure on any finding.

Clean-machine runtime installation without Visual Studio, live MongoDB/remote ODBC services, a production Redis deployment, physical mixed-DPI monitor transitions, screen-reader operation and broad manual accessibility review remain unperformed. The extracted-package acceptance ran on the current development host and does not substitute for those checks. No installer, code-signing or universal deployment result is claimed.
