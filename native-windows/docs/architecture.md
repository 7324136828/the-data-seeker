# Native Windows architecture

## Implementation profile

| Choice | Implementation |
|---|---|
| Interface | Existing Win32 implementation retained and repaired |
| Language/target | C++17 and native C libraries, Unicode, x64 |
| Build | `cpp/DataForgeStudio.sln`, MSBuild, VS 2026 v145, SDK 10.0.26100.0 |
| Runtime | /MDd Debug, /MD Release; Microsoft Visual C++ x64 runtime |
| Editors | Windows Rich Edit/TOM, native lexers/completion and line numbers |
| HTTP | WinHTTP; optional native mock listener belongs to the product feature set |
| Databases | Windows SQLite, native DuckDB, ODBC, libmongoc/libbson, RESP/Schannel |
| Request scripts | Statically compiled QuickJS-NG, original bounded `pm` bootstrap |
| Import | Native JSON and statically compiled yaml-cpp, native Postman/OpenAPI adapters |
| Storage | Checked atomic writes, DPAPI-protected configuration/query workspace, redacted HTTP history |
| Platform | Windows 10/11 x64; no non-Windows GUI claim |

The productionization skill defaults to MFC for a new application. This repository already contained Win32 controls and project structure. Retaining that implementation focuses the work on native behavior and avoids another interface-framework migration. Python/React/Node/browser components are source references, with their behavior ported into native modules.

## Layers and ownership

`app/` owns windows, drafts, validation, menus and native dialogs. `core/` owns HTTP, databases/providers, scripts, imports, formatting, snippets and assertions. `services/` owns stores, script deltas and archives. `platform/` owns temporary workspace and process containment. Tests exercise these actual modules.

API workers carry immutable request, document identity, environment and collection/folder snapshots. Completion joins each owned thread on the UI loop, updates the matching document, and merges script deltas into the newest store using launch identities. Independent documents may run concurrently. Sequential runners apply variable changes to their private scope snapshot before the next request, then persist them through the UI completion path.

Database jobs carry database/query/schema/grid snapshots, cancellation and provider contracts. Controls are mutated on the UI thread. Provider-native cancellation interrupts SQL/network work; the optional mock server owns its loopback socket and worker. ODBC browsing requires an explicit schema when discovery reports named namespaces and filters the returned namespace exactly, so driver metadata wildcard matching cannot redirect CRUD or exports to another table. The SQL console remains available without a schema. Close stops new work, requests cancellation and continues pumping messages until owned operations finish. Layout guards prevent scrollbar/WM_SIZE re-entry from exhausting the native stack.

SQL/provider results retain typed NULL/text/integer/real/boolean/BLOB/JSON values and separate result sets. Duplicate result labels are made unambiguous. Capabilities control CRUD/ER/explain/export tools. Profile credentials are session-only, environment-referenced or explicitly persisted with Windows DPAPI. Removing a connection detaches its catalog entry without deleting files.

Exports preserve the selected filtered data or fail visibly at a provider bound. Mongo export is limited to 100,000 documents and Redis export to 10,000 matching keys; narrow the search when those limits are exceeded. Redis grid previews sample large list/set/sorted-set/stream values, while export reads complete values subject to native protocol response bounds.

ODBC table DDL is reconstructed from the driver's available column, default, primary-key and foreign-key metadata; indexes remain a separate schema list. View definitions use bound catalog queries for supported SQL families. Unsupported or inaccessible definitions produce an explicit unavailable comment rather than a fabricated table definition. Complete server-specific constraints and storage options are not inferred from generic ODBC metadata.

## Visual system and editing

Slate surfaces (#0f172a/#1e293b/#334155), dark/light modes and the blue accent family remain. UI/code text uses 16 logical px and secondary text 14. Primary, muted, method/status and active-button text/background pairs target 4.5:1 contrast. High-contrast mode follows system text/highlight colors. Native control painting includes focus indicators and status text.

DPI messages rebuild fonts/layout; interface scale enlarges existing editor text. Compact section selectors retain every editor when space is limited, and native modal viewports scroll while keeping enlarged fonts. Mixed physical monitor and screen-reader acceptance remains separate from these automated code paths.

ER layout groups dependencies into layers, orders related cards to reduce crossings and handles cyclic/disconnected components with variable card heights. Saved manual positions survive refresh; Auto arrange explicitly replaces them. Orthogonal routing connects at the actual column rows and searches around expanded card obstacles, with penalties for crossings and shared segments. Logical routes are cached across scroll/scale changes and recomputed when geometry changes. Search has per-route and total-batch bounds; unresolved connectors are reported instead of being drawn through cards. A bounded cached visual pass adds gaps at strict interior crossings, away from ports and bends; hovered/selected relationships take precedence. Table selection emphasizes incident links, and hovering a line identifies its columns in the status area.

Rich Edit provides Windows Unicode input, text selection and undo. TOM formatting suspends/resumes undo instead of replacing text. Completion uses a native keyboard-accessible list. SQL suggestions use current schema, schema-qualified FROM/JOIN aliases and explicit `*`/`alias.*` expansion with provider identifier quoting. JSON can complete `{{variables}}` inside strings; JavaScript suggests the original `pm` APIs and names in scope-method string arguments. Both request script editors refresh names from valid unsaved variable drafts, while inherited script dialogs receive the selected scope's names. Coloring and completion are limited above 512 KiB; editor text is bounded to 16 MiB characters and oversized/binary edits fail visibly. API body preview is limited to 4 MiB while full response bytes remain available for export. Multipart selection is bounded to 8 MiB per file and 16 MiB of aggregate editor text. Large grid values remain unmodified and read-only in row dialogs.

## Script and import boundaries

QuickJS-NG executes optional request scripts inside the native process, with no filesystem/process/network/module host bridge. Each frame has 64 MiB heap, a tested 512 KiB JS stack budget, one-second interruption, cancellation, 4 MiB serialized input/output, up to 25 inherited scripts and 64 KiB per entry. Scripts with no code bypass payload serialization. Pre-request errors stop dispatch; post-response errors become visible failed tests. The executable requests an 8 MiB native stack reserve, with memory committed on demand; the smaller script budget also leaves space for native worker frames. Worker-thread recursion rejection and recovery are covered by the native tests.

YAML/JSON imports have explicit size/depth/item/reference boundaries. External references are not fetched automatically. Imported collection identities regenerate; preserved editable identities are nonempty and unique. Native/Postman/OpenAPI imports retain auth, variables, nesting, descriptions and script events. Advanced OpenAPI serialization choices may still need request configuration.

## Data, export and deployment

Mutable data lives under Local AppData. DPAPI protects saved request values, connection secrets and query workspace for the current Windows user; redacted history retains safe locations and identities. Exported collections/snippets contain the requested values. Original files are preserved on damaged-store failures, failed exports and detach. Database creation honors the chosen path and refuses overwrite. Scratch cleanup only removes owned directories and validates reparse/path boundaries.

The portable release contains intended binaries, actual native DLL dependencies, public docs/configuration/notices and the official signed VC runtime installer. Source snapshots/build intermediates/private files are excluded. PowerShell automates development and packaging. Ordinary operation is the GUI executable; the mock server starts only through its user control. Application signing and a separate installer are not implemented.

PDF/OCR and conversion ZIP dashboards are not applicable to this API/database product. Corresponding native workflows are request execution/results/history, query/grid/schema/ER tools, the runner, mock server and file exports. Auxiliary ZIP support uses stored entries with validated ownership boundaries.
