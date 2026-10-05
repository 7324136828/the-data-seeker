# DataForge Studio — native Windows edition

This project ports the original API and database workbench to C++17 and native Windows controls. Open `cpp/DataForgeStudio.sln`, or use the commands below. The application has no Python, React, Node.js, Electron or browser runtime. Optional request scripts execute inside a bounded, compiled QuickJS engine; the optional mock API runs inside the native application. Developer automation uses PowerShell.

The original slate dark/light surfaces, blue accents, sidebars and workbench layout remain. Default text is 16 logical pixels, secondary labels have stronger contrast, and View / Ctrl++ / Ctrl+- enlarges the interface up to 150%. Rich Edit provides syntax colors, line numbers, Unicode editing, undo and Ctrl+Space completion for SQL, JSON, variables and request scripts. SQL suggestions include connected tables, columns, schema-qualified aliases and quoted `*`/`alias.*` expansion. Script suggestions include variable names inside `pm` calls and update from unsaved variable edits.

## Build, test and run

Install Visual Studio or Build Tools with the desktop C++ components in `.vsconfig`. The verified development toolchain is VS 2026 Build Tools, v145 and Windows SDK 10.0.26100.0. Setup checks existing installations and compiles a native probe.

```powershell
.\setup.bat -Configuration Release -Platform x64
.\build.bat -Configuration Release -Platform x64
.\test.bat -Configuration Release -Platform x64
.\run.bat -Configuration Release -Platform x64
.\package.bat -Configuration Release -Platform x64 -Version 1.2.1
```

Debug supports build/test/run. The executable is `build/bin/x64/Release/DataForgeStudio.exe`; packages are under `dist/`. Install the included Microsoft-signed x64 Visual C++ Redistributable if the machine lacks the runtime. The application itself is unsigned and distributed as a portable archive.

The test script waits for both the console tests and actual GUI self-test process and checks their exit codes. GUI fixtures use disposable data and produce native render images next to the test executable. Build guards record source hashes and keep a matching private source archive. See [verification](docs/verification.md) for the actual acceptance results; a successful compile alone does not validate a feature.

## Working with the app

- Use Ctrl+T, Ctrl+W and Ctrl+Tab / Ctrl+Shift+Tab to create, close and switch request or query documents. Responses belong to their request documents; unfinished drafts remain when switching.
- API Client supports methods, query/path parameters, headers, authentication, JSON/raw/form/file bodies, structured assertions, variables and timeout/TLS/redirect options. Send with Ctrl+Enter and save named requests with Ctrl+S. A new saved request can select its collection or folder.
- Right-click a collection or folder to create/edit/duplicate/delete nodes, edit inherited authentication/variables/scripts, export JSON or run that scope. Search filters the tree. The native runner shows progress, per-request results, assertions and console output, and supports cancellation and export.
- Pre-request and post-response scripts retain the original `pm` APIs. Global → collection → nested folder → environment → iteration data → request-local variables resolve in that order. Script changes use the launch environment and collection identity, including during a sequential run.
- Import cURL from the header; import native collections, Postman v2.1, OpenAPI or Swagger JSON/YAML from File, the tree menu or file drop. The snippet dialog generates cURL, requests, fetch and Axios output as text. Multipart cURL snippets reference the selected attachment filename; save that file alongside the snippet.
- Database Studio supports SQLite, DuckDB, native ODBC SQL connections, MongoDB and Redis. Connection settings show available drivers and provider capabilities. SQL-family connections require the corresponding installed x64 ODBC driver and reachable service; native DuckDB/Mongo libraries ship with the app. Specify the exact schema for ODBC table browsing when the server exposes named schemas; the SQL console can still execute explicit queries without it. Credentials can be supplied for the session, referenced through environment variables, or explicitly persisted with Windows protection.
- Query documents retain SQL/JSON/commands, selected database, history and results. Use schema completion, templates, explain, row limits and multiple result-set selection. The grid supports paging, search, sort and typed row editing, including NULL/BLOB values where the provider supports them. Exports write the complete filtered table.
- Inspect columns, defaults, foreign-key actions, indexes and DDL. ER relationships connect at their column rows and route around cards. Use Auto arrange to reduce crossings, click a table to highlight its relationships, or hover a line to see its columns; dragged card positions remain saved. The optional File → Mock API server exposes connected SQL tables at `http://127.0.0.1:<port>/api/mock/data/<table>?db=<database-id>` with GET/POST and record GET/PUT/PATCH/DELETE routes. Record routes require one primary-key column; writes follow the connection's capabilities and read-only setting.
- Drop a SQLite database to attach it or a UTF-8 `.sql` file to open a query. Ctrl+1 / Ctrl+2 switches workbenches. Tab / Shift+Tab moves between controls; completion accepts Enter/Tab and dismisses with Escape.

Normal data lives under `%LOCALAPPDATA%\DataForgeStudio`. Saved request values, connection credentials and query workspace state use Windows DPAPI for the current user. History omits sensitive request values. Exported collections/snippets deliberately contain the values being exported; store those files accordingly. Removing a connection detaches its catalog entry and preserves the database file. New database creation refuses overwrite; exports replace destinations only after a complete checked write.

Background tasks own their threads and cancellation state. Closing stops new work, requests cancellation and continues processing messages until workers finish. Request scripts have limits on input, heap, execution time, nesting and inherited entries; editors and HTTP transfers also have explicit bounds. See [architecture](docs/architecture.md).

The entire original feature set is implemented in native modules. External-service compatibility, deployment and accessibility claims are limited to the checks actually listed in [verification](docs/verification.md) and [feature parity](docs/feature-parity.md). A clean-machine installation, live remote databases, physical mixed-DPI transitions and screen-reader acceptance require their own checks.

See [migration notes](docs/migration-notes.md), [third-party notices](THIRD_PARTY_NOTICES.md) and the location-only [secret audit](secrets.md).
