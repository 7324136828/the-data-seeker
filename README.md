# DataForge Studio — native Windows application

The C++ application is in [native-windows](native-windows/README.md). Open [DataForgeStudio.sln](native-windows/cpp/DataForgeStudio.sln) in Visual Studio. The UI, HTTP client, database adapters, stores and mock server are native; ordinary use requires no Python, React, Node.js or browser. Optional original request scripts run in the compiled native QuickJS engine.

The original slate style is preserved with larger text, stronger contrast, interface zoom, syntax highlighting, line numbers and completion. The port includes independent request/query documents, collection/folder editing, scripts and assertions, imports/snippets, the runner, SQLite/DuckDB/ODBC/MongoDB/Redis connections, typed grid editing, schema/ER tools and the mock REST API. Background work owns its threads and cancellation state.

Run these commands from the repository root:

```powershell
.\setup.bat -Configuration Release -Platform x64
.\build.bat -Configuration Release -Platform x64
.\test.bat -Configuration Release -Platform x64
.\run.bat -Configuration Release -Platform x64
.\package.bat -Configuration Release -Platform x64 -Version 1.2.0
```

Root entry points forward to the maintained project. The executable is `native-windows/build/bin/x64/Release/DataForgeStudio.exe`; archives are under `native-windows/dist/`. See [usage](native-windows/README.md), [identified issues and parity](native-windows/docs/feature-parity.md), [actual verification and limits](native-windows/docs/verification.md), and [migration notes](native-windows/docs/migration-notes.md).

`original-project/` remains the source reference. The earlier root `cpp/` and `SampleCpp.sln` remain for reference; the authoritative application and scripts are under `native-windows/`.