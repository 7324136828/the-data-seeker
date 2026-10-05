# Migration notes

The original Python/React implementation provided the feature reference. Its `../original-project` directory is absent from the current worktree, so earlier original-source audits are historical. The authoritative native delivery is `native-windows/cpp/DataForgeStudio.sln` and this directory’s scripts; root entry points forward here. Earlier root C++ files are reference material. Isolation preserves the candidate after unexplained root source changes; the user reported no concurrent editor. A later computer shutdown interrupted verification, and acceptance resumes from saved sources with fresh guarded builds.

The existing Win32 interface was retained. Original request scripting now uses the compiled native QuickJS engine, while optional mock serving uses a native loopback worker. Generated Python/JavaScript snippets are output text; the app does not execute them or require their runtimes.

## Identified problems and repairs

| Problem | Native repair |
|---|---|
| Small text, dim labels and inconsistent control backgrounds | Larger logical fonts, interface scale, stronger dark/light colors, themed buttons/combos/lists, focus indicators and system high-contrast fallback |
| Enlarged controls extended outside workbenches/dialogs | Responsive section selectors, DPI-scaled metrics, monitor-bounded modal viewports and focus scrolling |
| No useful native code editing | Rich Edit syntax colors, line numbers, undo-safe formatting, schema/alias/variable/script completion and normal Enter/Tab routing |
| Missing SQL star expansion and quoted script-name suggestions | Ordered provider-quoted column expansion, schema-qualified aliases, scoped names in `pm` string arguments and updates from valid unsaved variable drafts |
| Missing Mongo property suggestions and Redis command colors | Quoted JSON property/operator completion and line-leading native command highlighting |
| Detached HTTP workers and blocking SQL | Owned cancellable jobs with document identities, thread joins and UI-loop completion |
| Scrollbar layout callbacks re-entered recursively | Layout guard and actual-client-size comparison |
| ER relationships tangled at headers with overlapping labels | Column-level orthogonal routes around cards, dependency/cycle-aware Auto arrange, table selection highlighting and hover status; saved manual positions survive refresh |
| One active request/query and lost state | Independent documents, per-request response/console, protected query history/workspace and multiple result selection |
| Missing script/import/provider/mock features | Native QuickJS/`pm`, YAML/Postman/OpenAPI adapters, DuckDB/ODBC/Mongo/Redis providers and SQL-backed mock REST service |
| Incorrect SQL mutation/maintenance handling | Native stepping, rollback, separate autocommit maintenance handling, typed values and cancellation |
| Truncated exports and ambiguous result columns | Complete filtered exports, explicit result limits, unique column labels, checked atomic publication |
| Lost NULL/BLOB/default values | Typed row editor, omitted insert defaults, primary-key/FK checks and large-value preservation |
| Noisy SQLite decimal display | Shortest round-trip formatting for SQLite binary floating-point values, preserving the numeric value |
| Ambiguous ODBC namespaces and fabricated view DDL | Exact catalog/schema discovery, explicit unresolved-namespace errors, available table metadata reconstruction and bound actual view definitions |
| Incorrect snippet auth/forms/methods/JSON literals | Prepared auth/path/query values, enabled-field filtering, binary multipart generation and native syntax/round-trip tests |
| Imported URL queries were duplicated or encoded twice | Merge enabled query overrides, remove disabled entries, preserve unrelated/duplicate URL keys and fragments, and decode cURL values once |
| Plain values and sensitive history | Windows-protected stores, typed script value preservation, launch-scope delta application and location-only redacted history |
| Detach could remove database files | Separate connection removal that preserves files, exact new-file destination and overwrite refusal |
| Query-tab close could retain the closed tab’s database | Restore the surviving document’s database before autosave, tested with two real databases |
| Partial Redis metadata editing could overwrite a value | Preserve the value, reject unsupported field edits and keep derived fields read-only |
| Missing query templates and iteration scope | Provider-specific templates, explicit whole-document execution and typed read-only iteration data retained from the original |
| Placeholder import/drop behavior | Actual native file reads, schema-aware imports and SQL document loading |
| Unreliable script exit/package behavior | Real toolchain probe, owned GUI process wait/exit propagation, source-hash guards, allowlisted portable package and extracted-package self-test |

Mutable data is under `%LOCALAPPDATA%\DataForgeStudio`. Existing original plaintext workspaces are not silently imported or deleted. Native collection imports use the original public shape and fresh identities; private protected stores are an application implementation detail. Keep a backup before manually migrating an existing workspace.

The parity map and verification records separate implementation from actual acceptance. Live remote services, physical mixed-DPI transitions, screen-reader sessions and clean-machine runtime installation have not been inferred from local native tests. HTTP/script/editor/import/grid limits are explicit, and unsupported provider capabilities stay visible.
