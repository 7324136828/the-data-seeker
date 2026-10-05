---
name: native-mfc-productionization
description: >-
  Build, migrate, or productionize a Windows-native C++ desktop application
  using MSVC and Microsoft Foundation Classes (MFC), or plain Win32 when
  explicitly requested. Preserve original-project behavior and resources,
  organize native code under cpp/, deliver a buildable Visual Studio solution
  or an existing CMake-based equivalent, implement responsive native controls,
  background processing, isolated temporary workspaces, persistent history
  and native file export where applicable, automate setup/build/test/run/package
  with Windows scripts, and enforce credential hygiene. Use for native C++,
  MFC, Win32, Visual Studio, and legacy C++ modernization requests, not for
  React/Python applications or browser-wrapped desktop interfaces.
---

# Native C++ / MFC Project Productionization Standard

## Purpose and scope

Transform the implementation in `original-project/`, or an existing C++ project identified by the user, into a maintainable, secure, usable **native Windows desktop application**. When starting a new project, implement the user's requirements directly rather than inventing an original implementation.

Default to **MFC for the interface, MSVC for compilation, Unicode, C++17 or a verified newer standard, and an x64 target**. Use plain Win32 only when requested or when preserving an existing Win32 application is the agreed scope. Preserve a required Win32/x86 target when a legacy dependency prevents x64 migration, and explain the constraint.

The deliverable must contain real C++ implementations, Windows resources, build configuration, automation, tests, and documentation. An empty dialog, screenshots, pseudocode, or a project that merely launches an old script is not a completed native conversion.

MFC is a Windows desktop framework. Do not claim that the MFC interface builds or runs natively on macOS or Linux. A separately isolated core may support other platforms if that support is actually implemented and tested. [R1]

### Native-only default

Do not introduce React, Vite, Node.js, npm, Python, Flask, FastAPI, Electron, Chromium, WebView2, C++/CLI, or a .NET application runtime as substitutes for the requested native application. Do not launch a local web server or require a browser for ordinary operation. PowerShell used for developer automation is not an application-runtime dependency.

Use native C/C++ libraries for necessary parsing, storage, compression, rendering, or networking. Declare third-party dependencies and licenses. A native DLL or a justified native worker executable is permitted; native does not mean “one executable with no dependencies.” Keep external services only when they belong to the user's actual requirements.

### Applying the original conversion workflow

This skill retains the source standard's six pillars and its preservation, ingestion, progress, history, temporary-workspace, ZIP-export, automation, and secrets requirements, translated into desktop equivalents.

For a PDF or document converter, implement the complete **Convert → Progress → History → Save ZIP** workflow, including file selection, drag-and-drop, and supported clipboard ingestion. For another type of application, preserve the corresponding domain workflow. Do not add a PDF parser, conversion ledger, or ZIP output to an application that does not need one. Record such source-specific requirements as not applicable with a reason instead of silently dropping them.

The native-specific build, threading, deployment, and recovery rules below are adaptations and extensions, not claims that the source skill already contained those rules.

## Required execution procedure

1. **Inspect before changing.** Inventory source, project files, resources, dependencies, entry points, existing scripts, tests, supported inputs, outputs, and target platforms. Read the applicable repository instructions. Never infer unavailable source behavior from filenames alone.
2. **Establish the baseline.** Run existing tests or the original application when the environment supports it. Record observed behavior, reference outputs, and anything that could not be inspected or executed.
3. **Choose the implementation profile.** Record MFC versus explicitly requested Win32, application type, architecture, build system, toolset, SDK, runtime linkage, and applicable document-processing features in `docs/architecture.md`.
4. **Create a parity plan.** Map each original feature to its native implementation and an acceptance test. Put gaps in `docs/feature-parity.md`. Distinguish preserved behavior from intentional changes.
5. **Implement vertical slices.** First produce a launching native application, then connect a real operation end-to-end, then complete the remaining interface, background work, persistence, export, and recovery.
6. **Build and test repeatedly.** Compile after meaningful changes. Resolve compiler, resource compiler, and linker errors rather than accumulating an untested scaffold.
7. **Verify and report.** Record actual commands, configurations, exit codes, passed tests, skipped tests, and known limitations in `docs/verification.md`. Never report a Windows/MSVC build as successful without running it successfully.

If the environment lacks Windows, MSVC, or MFC, still produce the requested source and build configuration as far as possible. Identify the unavailable prerequisite and leave native-build verification explicitly pending. Do not invent build logs or substitute a Linux build of the core for an MFC application test.

---

## Pillar 1: Architecture & Project Topology

### 1. Standard directory layout

Keep new native application source and Windows resources under `cpp/`. Keep the original code and resources in their existing locations or in `original-project/`; do not move an established repository merely to satisfy the example layout.

```text
repository-root/
├── original-project/                # Original implementation and reference assets
├── cpp/
│   ├── NativeApp.sln                 # Default: authoritative MSBuild solution
│   ├── Directory.Build.props        # Shared, configuration-aware build settings
│   ├── app/
│   │   ├── NativeApp.vcxproj
│   │   ├── NativeApp.vcxproj.filters
│   │   ├── App.h / App.cpp           # CWinApp/CWinAppEx application lifetime
│   │   ├── MainDialog.h / .cpp       # Or frame/document/view for an SDI/MDI app
│   │   ├── WorkPage.h / .cpp
│   │   ├── HistoryPage.h / .cpp
│   │   ├── SettingsDialog.h / .cpp   # Only when settings are needed
│   │   └── pch.h / pch.cpp
│   ├── core/
│   │   ├── Core.vcxproj
│   │   ├── include/native/          # Domain types and processing interfaces
│   │   └── src/                     # Testable algorithms; no MFC UI dependency
│   ├── services/
│   │   ├── Services.vcxproj
│   │   ├── JobManager.h / .cpp
│   │   ├── HistoryStore.h / .cpp
│   │   ├── ArchiveService.h / .cpp
│   │   └── OriginalProjectAdapter.h / .cpp
│   ├── platform/
│   │   ├── Platform.vcxproj
│   │   ├── TempWorkspace.h / .cpp
│   │   ├── ProcessRunner.h / .cpp
│   │   └── CredentialStore.h / .cpp
│   ├── resources/
│   │   ├── resource.h
│   │   ├── NativeApp.rc
│   │   ├── NativeApp.rc2            # Only if needed
│   │   ├── NativeApp.manifest
│   │   └── icons/                   # Real, valid resource assets
│   ├── worker/                      # Optional isolated native worker
│   ├── tests/                       # Core, integration, and recovery tests
│   └── third_party/                 # Only intentionally vendored dependencies
├── scripts/
│   ├── common.ps1                   # Tool discovery and checked process execution
│   ├── build.ps1
│   ├── test.ps1
│   └── package.ps1
├── setup.ps1 / setup.bat
├── build.bat / test.bat
├── run.ps1 / run.bat
├── package.bat
├── config.example.json              # Non-sensitive configuration only
├── .vsconfig                        # Verified component requirements, if provided
├── .gitignore
├── README.md
├── THIRD_PARTY_NOTICES.md
├── secrets.md                       # Findings/status only; never secret values
├── docs/
│   ├── architecture.md
│   ├── feature-parity.md
│   ├── migration-notes.md
│   └── verification.md
├── build/                           # Ignored build products and intermediate files
└── dist/                            # Ignored release packages
```

Adapt names and combine small libraries when appropriate. Do not create empty projects or unused infrastructure just to match this tree. For an existing CMake project, keep CMake authoritative and generate Visual Studio projects rather than independently maintaining conflicting build systems.

### 2. Layer boundaries

**UI layer:** Native windows, menus, dialogs, command routing, data exchange, validation, progress display, and user decisions. Keep processing algorithms out of button handlers.

**Core layer:** Deterministic domain logic, typed inputs and outputs, validation, and cancellation/progress contracts. Avoid `CWnd`, dialog resources, or a required message loop in ordinary core tests.

**Services layer:** Job scheduling, persistence, orchestration, packaging, and adapters. Expose local C++ operations instead of HTTP routes.

**Platform layer:** Windows filesystem integration, process/handle management, clipboard bridges, and credential facilities. Avoid circular dependencies; compose concrete services at the application boundary.

Use value types, RAII, explicit ownership, and small interfaces. Handle `HANDLE`, COM interfaces, GDI objects, sockets, and other non-memory resources with appropriate lifetime wrappers. Avoid mutable globals and do not retain UI pointers in long-lived core tasks.

### 3. Preserve and adapt the original implementation

Preserve original computational behavior, file formats, resource identities, command meanings, and error conditions unless the user requests a change. Reuse working C++ logic rather than rewriting it unnecessarily.

Replace hard-coded paths and interactive console prompts with explicit parameters. Remove dependence on the current working directory. Port non-C++ logic into native implementations when native-only operation is required; wrapping a Python script does not complete that port.

For legacy Visual C++ projects, inspect `.dsw`, `.dsp`, older `.vcproj`/`.vcxproj` files, `.rc`/`.rc2` resources, `.lib` dependencies, DLL imports/exports, COM registration, and platform-specific assumptions. Recreate or upgrade build metadata without silently discarding code or assets.

Check pointer truncation, `LONG_PTR`/`UINT_PTR` usage, integer widths, calling conventions, structure packing, ownership across DLL boundaries, character encodings, deprecated APIs, and binary serialization. Do not “fix” x64 errors by casting pointers to 32-bit integers. Rebuild dependencies where feasible or preserve an explicitly documented compatible target.

Distinguish modernizing old source for current MSVC from producing an executable for an old Windows release. Do not promise Windows 98 or Visual Studio 6.0 compatibility merely because the input project originated there.

Preservation is not permission to redistribute live credentials. Handle confirmed secrets through Pillar 6, documenting any redaction rather than silently claiming the baseline remained byte-for-byte unchanged.

### 4. Native project integrity

Provide all referenced source files, headers, resources, and dependency instructions. Resource identifiers must be defined once and match their controls and message handlers. Do not reference nonexistent icons or use a text placeholder as an `.ico` file.

Configure precompiled headers consistently when used. Include the MFC precompiled header first in participating translation units and avoid incompatible Windows-header ordering. Do not apply C++ precompiled-header settings to resource compilation.

Use a GUI subsystem for the application and a separate console target for CLI/tests if needed. Let the MFC application framework own the normal entry point; do not add an unrelated `main()` or custom entry-point override to resolve a linker error.

---

## Pillar 2: Native MFC UI, File Ingestion & History Dashboard

### 1. Application layout and navigation

Choose the simplest native application structure that fits the requirements:

- A resizable `CDialogEx` application with tabs for a focused utility or converter.
- An SDI application using a frame/document/view structure for a document-oriented tool.
- An MDI application only when independent simultaneous documents are needed.

For the conversion profile, provide a **Convert/Work** page and a **History** page. The work page contains input selection, parameters, start/cancel actions, progress, result actions, and a bounded log view. The history page lists persisted records and exposes the appropriate action for each state.

Use MFC/Win32 controls such as buttons, edit controls, list controls, progress bars, tab controls, menus, toolbars, and status bars. Route commands through message maps and update enabled/disabled states consistently. Never report success from a placeholder handler.

### 2. Native file selection, drop, and clipboard paste

For each input mode, use the same validation and staging service so behavior is consistent.

**File picker:** Use `CFileDialog` or the Windows common item dialog. Configure filters for supported formats, including PDF when applicable. Validate existence, accessibility, file type, size, and parser acceptance rather than trusting an extension alone.

**Drag-and-drop:** Support filesystem drops through `WM_DROPFILES` or an OLE drop target. Provide visible feedback and reject unsupported payloads. Initialize the necessary OLE facilities when using OLE drag-and-drop. Release resources according to the transfer mechanism.

**Clipboard paste:** Provide an `ID_EDIT_PASTE` command and `Ctrl+V` behavior when the input area has focus. Support `CF_HDROP` for copied filesystem files. Where required, implement virtual-file transfers through file-descriptor/file-contents formats. Support `CF_UNICODETEXT` only when pasted text is a valid domain input. [R3]

There is no assumption that every application places an entire PDF on the clipboard. Copying text from a PDF viewer is not equivalent to copying the PDF file. Clearly explain unsupported clipboard contents without creating a fake input document.

Respect clipboard/COM ownership: do not free borrowed clipboard data; do not call `DragFinish` on a borrowed clipboard handle. For a received `WM_DROPFILES` handle, finish the drop appropriately. For OLE transfers, release acquired storage media. Keep COM apartment and marshaling requirements explicit when work crosses threads.

Do not intercept ordinary paste into unrelated text fields. Stage copies into application-managed workspaces; never move or modify the user's original input as a side effect of ingestion.

### 3. Working and calculation feedback

Show real state transitions, the current stage, elapsed time, a truthful progress indicator, and a cancellation action. Use indeterminate progress when the amount of work is unknown; do not fabricate percentages.

A document-processing sequence might be:

```text
Idle -> Validating -> Staging -> Processing -> Packaging -> Completed
                                |               |
                                +-- Failed -----+
                                +-- Cancelling -> Discarded
```

Separate input-selection state from persisted job state. Distinguish validation errors from execution failures. Disable conflicting actions while preserving window movement, resizing, navigation, cancellation, and access to logs.

Send processing, large input copies, database maintenance, and archive generation off the UI thread. Redact log entries before they reach either the UI or disk. Limit retained log lines and coalesce frequent progress updates.

### 4. Safe UI/worker communication

Keep each window and control owned by its UI thread. Do not call `SetWindowText`, `UpdateData`, `UpdateAllViews`, or manipulate a UI-owned MFC object directly from a processing thread. MFC objects and handle maps have thread-specific constraints. [R2]

Use a thread-safe event queue containing owned values or immutable snapshots. A worker can post a `WM_APP` notification to tell a dispatcher that events are available; the UI drains the queue and updates controls. An event notification is not permission to transfer dangling pointers through `WPARAM` or `LPARAM`.

Define queue bounds, progress coalescing, job/attempt identifiers, shutdown behavior, and notification failure handling. Stop producers and detach notifications before destroying their dispatch window. An `IsWindow` check alone does not establish safe window lifetime.

Use standard C++ worker threads for code that does not call MFC. Where a worker must use MFC, create and manage it through the appropriate MFC threading facilities, such as `AfxBeginThread`, and still avoid cross-thread control access. [R2]

### 5. Historical conversions screen

For the conversion profile, persist and display job ID, input display name, input size, creation time, duration, state, stage, progress if known, and result availability.

Implement these actions with distinct semantics:

| Action | Required behavior |
|---|---|
| Continue / View progress | Reopen monitoring for a job that is actually still running. Do not start another copy. |
| Resume | Continue an interrupted job only from a validated, supported checkpoint. |
| Retry | Create an explicitly identified new attempt or job and preserve the previous failure record. |
| Discard | Request cancellation, wait for execution to stop, clean only managed temporary artifacts, and update history. |
| Save ZIP / Export | Open a native save dialog and save an available completed artifact. |
| Open output folder | Open a validated existing result directory; do not construct an unchecked shell command. |

Refresh from local service events. A modest local timer is acceptable where events are impractical; do not recreate the original HTTP polling architecture without a real requirement.

A persistent record is not proof that its output still exists. Show unavailable or expired artifacts clearly. Do not delete user-exported files when discarding a history entry unless the user explicitly requests that deletion.

### 6. Native visual quality and accessibility

Implement resizing, keyboard navigation, logical tab order, visible focus, accessible control names, clear validation messages, and usable high-contrast behavior. Avoid depending on color alone for status.

Use appropriate system fonts, consistent spacing, and DPI-appropriate assets. Configure a manifest and layout behavior appropriate to the chosen DPI-awareness level. For per-monitor awareness, handle DPI changes and refresh sizes, fonts, and cached assets as necessary; adding a manifest entry alone is not sufficient. [R4]

Test at representative display scales and across mixed-DPI monitors when available. Do not claim dark-mode, screen-reader, or per-monitor support without checking the relevant controls. Optional custom drawing must not erase standard keyboard and accessibility behavior.

---

## Pillar 3: Native Processing Services, Temp Pipeline & ZIP Packaging

### 1. Isolated system temporary workspaces

Give every processing job a unique managed workspace under the operating system's temporary directory. Resolve the location through a supported Windows API or an appropriate native abstraction; do not hard-code a username or `C:\Temp`.

When using `GetTempPath2W`, account for the declared OS baseline and provide runtime fallback to `GetTempPathW` if required. The returned location must still be checked for existence, accessibility, and suitability. [R5]

```text
<system-temp>/Vendor/NativeApp/jobs/<job-id>/
├── inputs/             # Copies of user input under generated safe names
├── work/               # Intermediate files and resumable stage data
├── outputs/            # Completed output candidates
└── archive/            # ZIP under construction, then finalized
```

Create the job directory exclusively with an unpredictable identifier and appropriate user access. Detect collisions rather than reusing someone else's directory. Validate file and directory operations, disk-space failures, and cleanup errors.

Never write user inputs, temporary processing data, history databases, or application-runtime outputs into the source checkout or installation directory by default. Development build products may live under ignored `build/` and `dist/` directories.

Use generated staging filenames and retain the original name only as display metadata. Validate containment and account for reparse points, symlinks, alternate data streams, reserved names, and path traversal. A string-prefix comparison alone is not a sufficient boundary check.

Delete only directories created and owned by the application's workspace manager. Never recursively delete the system temp root, the repository, an arbitrary path read from a database, or the user's source folder. Prevent path-replacement races for the selected threat model; private ownership and handle-aware checks are stronger than an unchecked lexical path.

### 2. Separate scratch space from durable history and results

Store persistent metadata in an application-specific user-data directory, resolved through Windows known-folder facilities, such as a location under Local AppData.

```text
<local-app-data>/Vendor/NativeApp/
├── history/jobs.db
├── settings/config.json             # Non-secret settings only
├── results/<job-id>/output.zip       # Retained results, when retention is enabled
└── logs/                            # Redacted, bounded application diagnostics
```

The path above is a storage policy, not a reason to concatenate untrusted environment values without validation. Protect private data according to its sensitivity and honor retention settings.

For completed conversion history, default to retaining the final archive in managed durable storage until the user deletes it or an explicit retention policy expires it. Finalize and verify the artifact before marking it available. Keep processing intermediates in temp and remove them independently.

If an interrupted job is advertised as resumable across restarts, persist its required checkpoints and inputs durably or verify that retained temporary material still exists before enabling Resume. Never promise recovery from a directory the OS may have removed.

Cleanup must respect active jobs and active exports. An exported user copy is independent of the managed cache. A scoped workspace owner must not delete an artifact while a save operation is still reading it.

### 3. Native processing contract

Replace Python imports and API endpoints with typed C++ calls. Implement the domain processor rather than calling an invented function name and leaving it undefined.

The following is an **interface example**, not a complete application:

```cpp
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace native_app {

struct JobRequest {
    std::filesystem::path stagedInput;
    std::filesystem::path workDirectory;
    std::filesystem::path outputDirectory;
};

struct ProgressUpdate {
    std::string stage;
    std::optional<double> fraction;  // Absent when progress is indeterminate.
    std::string safeMessage;         // Redacted before publication.
};

class CancellationToken {
public:
    explicit CancellationToken(const std::atomic_bool& flag) noexcept
        : flag_(flag) {}

    bool requested() const noexcept {
        return flag_.load(std::memory_order_relaxed);
    }

private:
    const std::atomic_bool& flag_;
};

struct JobResult {
    std::vector<std::filesystem::path> outputFiles;
};

using ProgressSink = std::function<void(const ProgressUpdate&)>;

class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual JobResult Run(const JobRequest& request,
                          const CancellationToken& cancellation,
                          const ProgressSink& progress) = 0;
};

} // namespace native_app
```

The cancellation flag must outlive every token using it. The processor must check it at useful work boundaries. A callback runs on the calling worker unless explicitly dispatched; it must enqueue owned data instead of updating a dialog directly or retaining a reference to a temporary event.

Choose and document one result/error convention. Catch errors at job and UI boundaries, preserve diagnostic context safely, and map cancellation separately from failure. Do not allow an exception to escape a Windows callback or worker entry point.

### 4. Job state and persistence

For the conversion profile, use an explicit transition model such as:

```text
queued -> staging -> running -> packaging -> completed
   |         |          |           |
   +---------+----------+-----------+-> cancelling -> discarded
   +---------+----------+-----------+-> failed

On recovery: unfinished state -> interrupted
interrupted -> resumed work, new retry attempt, or discarded
```

Persist job ID, attempt ID, original display name, input size, relevant input identity/hash, parameters, processor/checkpoint version, status, stage, nullable progress, creation/start/completion timestamps, redacted errors, managed workspace identity, archive location, artifact availability, and cleanup status where applicable.

Use SQLite or a transactional equivalent appropriate to the application. Do not put the database in a temporary workspace. Define schema versioning, migrations, connection ownership, concurrent access, bounded queries, and retention.

Record history changes coherently with filesystem changes. Treat artifact publication as a staged operation with reconciliation on startup; a filesystem rename and a database transaction are not automatically a single transaction.

After an unexpected shutdown, reconcile previously active records with verified worker ownership, checkpoints, and available files. Do not identify a surviving worker by PID alone because PIDs can be reused. Default to an interrupted state when continuation cannot be established safely.

### 5. Local service operations replacing the API

| Source HTTP responsibility | Native service responsibility |
|---|---|
| Start conversion | `SubmitJob(request)` validates and schedules work. |
| List jobs | `ListJobs(filter)` reads persistent history. |
| Read job status | `GetJob(jobId)` returns a snapshot. |
| Stream/poll progress | `SubscribeToJobEvents(...)` delivers owned notifications. |
| Discard job | `RequestDiscard(jobId)` starts the cancellation/cleanup protocol. |
| Download archive | `ExportArchive(jobId, destination)` performs a checked native save. |

These names describe contracts. Implement them or adapt them to existing project interfaces; do not create an HTTP server merely to preserve route names.

### 6. Cancellation and native process isolation

Prefer cooperative cancellation for in-process tasks. Keep their state and resources alive until they exit. Do not use `TerminateThread` to stop work and do not report Discarded while a worker may still write into its directory.

For untrusted, crash-prone, or non-cancellable native components, use a separate native worker executable when justified. Launch an explicit trusted executable with `CreateProcessW`, an explicit working directory, correctly encoded arguments, and a controlled set of inherited handles. Do not route user data through `cmd.exe /c` or `system()`.

Use an appropriate Windows Job Object when the application must own and contain a process tree. Account for assignment failures and existing job restrictions; establish containment before execution where required. Windows Job Objects support process-group lifetime and termination management. [R6]

Drain redirected output without pipe deadlocks. Define IPC framing, size limits, timeouts, disconnect handling, and credential redaction. Do not send large or sensitive payloads on unchecked command lines.

On discard: request cancellation, wait asynchronously for exit, use a documented worker-process termination fallback only when necessary, close handles, then remove managed scratch data. If cleanup fails, record it as pending or failed; never report a successful purge that did not occur.

### 7. Native ZIP packaging and saving

Use a pinned, declared native compression library for ZIP generation when ZIP output is part of the application. Do not add a Python or PowerShell runtime dependency just to create archives.

Add only intended output files using relative archive names. Exclude source inputs unless required, secrets, private configuration, unrelated logs, temporary files, and the archive itself. Reject unsafe archive paths and links outside the allowed output tree. If importing archives is supported, defend against traversal and excessive expansion as well.

Write to a temporary archive name, close and validate it, then publish the completed artifact. Mark the job Completed only when the required outputs and archive are successfully finalized. Check capacity and library errors; use large-file/archive support when required by the tested limits.

For export, use a native Save dialog. Confirm replacement before overwriting an existing destination. Write a temporary destination-side file and finalize it with the appropriate filesystem operation, handling cross-volume copies and failures honestly. Do not delete an existing user file before a successful replacement is ready.

PDF parsing, rendering, or OCR must use an explicitly selected native implementation with verified capabilities and license suitability. Do not claim that MFC itself provides those engines. Include OCR only when the actual requirements call for it.

---

## Pillar 4: Automated Setup & Native Dependency Management

### 1. Supported toolchain and prerequisites

Default to an installed, supported Visual Studio or Visual Studio Build Tools installation with the required MSVC tools, Windows SDK, and matching MFC headers/libraries. MFC support is an optional installation component; detecting `cl.exe` alone is not enough. [R7]

Detect installations using `vswhere` or another supported discovery mechanism rather than hard-coded edition-specific paths. Microsoft documents `vswhere` discovery of C++ installations and developer-environment setup. [R8]

Select a compatible installation, not simply the numerically newest installation. Honor explicit toolset/SDK/architecture requirements. Accept a user-specified installation path where useful and validate it. Verify the selected compiler, linker, resource compiler, manifest tool, MSBuild, MFC headers, and target-architecture libraries by actual configuration/build probes.

Record exact tool versions and target architecture. Do not invent a toolset version, component ID, or SDK path. A `.vsconfig` file must reflect components verified for the chosen installation family.

Do not install or upgrade large tools silently, accept license terms on the user's behalf, change global execution policy, or request elevation for ordinary application use. Report missing components with actionable installation requirements. Any automatic provisioning must be explicit, approved, and use trusted distributions.

### 2. Build-system policy

For a new MFC project, default to a Visual Studio solution with `.vcxproj` files. Keep project GUIDs, solution configuration mappings, source/resource items, references, and property imports valid.

For an existing CMake project, retain that build system. Use an appropriate installed Visual Studio generator and matching architecture where relying on CMake's MFC project settings. Set shared versus static MFC intentionally; `CMAKE_MFC_FLAG` uses `2` for shared MFC and `1` for static MFC. Do not assume that the variable alone configures arbitrary non-Visual-Studio generators. [R9]

Keep MFC configuration scoped to GUI targets/directories. Configure the MSVC runtime consistently across relevant native targets and dependencies. If CMake generates the solution, do not hand-edit the generated project and lose the change on reconfiguration.

### 3. Compiler, linker, and resource contract

Use these defaults unless an existing compatibility constraint requires a documented alternative:

| Setting | Default requirement |
|---|---|
| Target | x64; Win32/x86 only when required or explicitly supported. |
| Language | C++17 or a verified newer project-wide standard. |
| Character set | Unicode; explicit conversions at UTF-8/UTF-16 boundaries. |
| Configuration | Separate Debug and Release outputs and dependencies. |
| MFC linkage | Shared MFC by default; static linkage only as an intentional alternative. |
| CRT linkage | `/MDd` for Debug and `/MD` for Release with shared MFC. |
| Diagnostics | `/W4` for owned code; resolve warnings rather than globally suppressing them. |
| Conformance | `/permissive-`, `/EHsc`, and `/utf-8` where supported and appropriate. |
| Security | Preserve supported compiler/linker protection settings; document exceptions. |
| Resource build | Compile `.rc` files, include real assets, and embed a coherent manifest. |
| Output | `build/bin/<platform>/<configuration>/` with per-project intermediate folders. |

Do not mix incompatible runtime selections, architectures, debug/release libraries, or allocation ownership. MSVC documents the runtime-library options and requires compatible runtime selections for modules linked together. [R10]

Do not turn on warnings-as-errors globally for unmodified third-party code just to create an unusable baseline. Conversely, do not disable security checks or conformance warnings globally to conceal defects in new code.

For shared MFC, verify the expected `_AFXDLL` and Unicode definitions are present through the chosen project configuration. Keep `_DEBUG` and runtime settings consistent. Avoid indiscriminate preprocessor definitions that break legacy MFC headers.

### 4. Script responsibilities

`setup.ps1`, launched by `setup.bat`, must validate prerequisites, resolve repository paths from its script location, check the selected build configuration, provision approved native dependencies, initialize non-secret local settings without overwriting existing values, and explain the next command.

`build.bat` delegates to a checked build script. `test.bat` runs the actual test executables or the selected native test runner. `package.bat` creates the release deliverable. `run.bat` launches the selected executable through `run.ps1`.

Each script must accept consistent configuration/platform parameters, work from a path containing spaces and Unicode characters, return a nonzero exit code on failure, and avoid interactive `pause` in automation.

Example user-facing command contract:

```bat
setup.bat -Configuration Release -Platform x64
build.bat -Configuration Release -Platform x64
test.bat -Configuration Release -Platform x64
run.bat -Configuration Release -Platform x64
package.bat -Configuration Release -Platform x64
```

Implement these arguments consistently; do not document switches that do not exist. Keep a direct MSBuild or CMake command in the README for troubleshooting.

A minimal batch dispatch pattern is:

```bat
@echo off
setlocal
powershell.exe -NoLogo -NoProfile -File "%~dp0setup.ps1" %*
exit /b %errorlevel%
```

PowerShell policy requirements must be documented rather than silently weakening machine policy. Support the PowerShell version declared by the project; do not use PowerShell 7-only features while claiming compatibility with Windows PowerShell 5.1.

### 5. Exit-code and environment handling

Use strict error handling and explicitly check native tool exit codes. `$ErrorActionPreference = 'Stop'` is not a substitute for checking a native build tool's result.

The following helper illustrates the required failure behavior:

```powershell
function Invoke-CheckedNativeTool {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Executable,
        [string[]] $Arguments = @()
    )

    & $Executable @Arguments
    $toolExitCode = $LASTEXITCODE
    if ($toolExitCode -ne 0) {
        throw "Native tool failed with exit code $toolExitCode."
    }
}
```

Initialize the compiler environment in the process that actually builds, or invoke correctly discovered build tools with all required configuration. Running `VsDevCmd.bat` in an isolated child process does not automatically update its PowerShell parent's environment.

Do not print the entire environment while diagnosing tool discovery. If importing a developer environment internally, capture it without echoing secret-bearing values. Do not publish unreviewed MSBuild binary logs because build properties and environment data may contain sensitive information.

### 6. Native dependencies and platform honesty

Declare each third-party dependency, version or pinned revision, source, license, target architecture, build options, and runtime-linkage requirements. Use a reproducible native dependency mechanism, such as an intentionally configured vcpkg manifest or reviewed vendored source, only where dependencies are needed.

Actually wire dependency includes, libraries, and runtime DLL deployment into the selected build system. A manifest alone does not prove the application will link. Do not rely on undeclared globally installed libraries or files left behind by an earlier build.

Python virtual environments, `requirements.txt`, npm installs, and web development servers are not part of this native setup. Do not produce misleading `setup.sh` or `run.sh` scripts that claim to build MFC on POSIX systems. Optional core-only scripts must state their restricted scope.

---

## Pillar 5: Native Execution, Process Lifecycle & Deployment

### 1. Unified native launch

A normal launch starts the native GUI executable, not two services. Resolve it from the documented build configuration or installed location, not an arbitrary current directory or the first executable found recursively.

Fail clearly when the executable or a required runtime is missing. An explicit build-if-missing option may invoke the build script; do not silently download toolchains or launch a stale binary after a failed build.

For developer automation, `run.ps1` should wait for the launched process when expected and propagate its exit status. Quote paths safely and use structured argument handling. Start only owned helper processes, and retain the handles required to manage them.

### 2. Graceful shutdown and restart

On close, reject new jobs, request cancellation or offer a documented wait/close decision, persist recoverable state, stop event producers, finish worker cleanup, and release resources. Keep the message loop responsive while waiting; do not block it indefinitely on a join.

Do not detach threads that still reference application services or windows. Do not leave orphaned native workers after a normal shutdown. If work must survive GUI closure, implement that as an explicit worker/service architecture with authenticated local communication, persistent ownership, and a reconnection protocol.

A Windows service is not the default answer to keeping a desktop window responsive. Use one only for an actual independent service-lifecycle requirement.

After an unexpected shutdown, reconcile state, validate checkpoints and inputs, and offer Continue, Resume, Retry, or Discard according to what is genuinely possible. Re-running a job from the beginning must be labeled Retry, not checkpoint recovery.

### 3. Logging and local settings

Store bounded, redacted diagnostics in an appropriate user-data directory. Separate user-facing messages from detailed troubleshooting information. Do not log credentials, clipboard contents, entire documents, or sensitive paths unnecessarily.

Persist non-secret settings with a schema/version and safe defaults. Recover from malformed settings without silently destroying user data. Keep credentials in appropriate platform facilities rather than plaintext JSON or resources.

Handle low disk space, inaccessible folders, missing DLLs, parser failures, canceled dialogs, invalid inputs, and damaged history databases with actionable messages. Avoid presenting the user with a successful status after a failed write.

### 4. Release packaging

Create a repeatable Release package under `dist/` containing the executable, required redistributable native dependencies, public configuration examples, documentation, and applicable third-party notices. Exclude credentials, local databases, user inputs, temporary workspaces, private diagnostics, and unrelated source material.

Shared MFC applications require the appropriate runtime components on the target machine. Use a deployment method permitted for the selected Visual Studio/runtime version, normally the appropriate official Visual C++ Redistributable where applicable. Do not distribute debug MFC runtimes as a production dependency. [R11][R12]

Do not assume a developer machine proves deployment readiness. Test the package on a clean Windows environment without Visual Studio installed. Verify architecture, startup, resources, dependent DLL resolution, a real workflow, and uninstall/data-retention behavior where an installer is supplied.

Do not claim that static linkage automatically removes every dependency or guarantees a universally portable executable. Check actual linked components and licenses. Do not silently claim code signing; sign only through an available, authorized signing mechanism and never package its private key.

### 5. Required tests

Implement core behavior tests using known-good inputs/outputs and integration tests for applicable file-processing features. At minimum, cover successful processing, invalid/unsupported inputs, cancellation, failure propagation, concurrent jobs where supported, export, and persistence across restarts.

Test paths with spaces and non-ASCII characters, access denied, low disk space where safely simulated, missing artifacts, oversized inputs, cleanup boundaries, and shutdown during active work. Test that user source files and exported copies survive discard and cleanup.

For resumable jobs, test a real interruption between checkpoints, verification of changed inputs, idempotence, and resumed output correctness. For non-resumable work, verify that Retry does not masquerade as Resume.

For the GUI, verify navigation, enabled states, keyboard behavior, resizing, DPI behavior, error messages, and continued responsiveness during real processing. Record manual tests separately from automated tests.

---

## Pillar 6: Secrets Detection, Redaction & Credential Hygiene

### 1. Repository-wide scan

Inspect source, headers, resources, configuration, project files, property sheets, scripts, CI workflows, documentation, examples, test fixtures, and release-staging files for likely secrets. Review original code without copying secrets into new native files.

Check API keys, access tokens, passwords, OAuth secrets, connection strings, SMTP credentials, signing secrets, private keys/certificates, cloud credentials, authorization headers, session cookies, and secret-bearing environment files.

Use an available established secret scanner plus conservative pattern checks. Verify the installed scanner's command syntax and enable redaction. Do not claim a scanner ran if it was unavailable. Report the actual scope: current files, Git history, generated artifacts, or a subset.

### 2. Never expose detected values

Never print a discovered credential in console output, source excerpts, UI logs, tests, documentation, reports, or `secrets.md`. Use `<REDACTED_SECRET>` as a report placeholder. Do not include reversible encodings or private-key fragments.

Do not paste a raw scanner report into the response. Ensure temporary scan output is redacted and protected before viewing or sharing it. Do not treat encoding or string obfuscation inside a C++ binary as credential protection.

### 3. Conservative remediation

Classify suspected secrets before editing. Do not redact ordinary resource IDs, GUIDs, public URLs, version numbers, content hashes, or deliberately fake examples merely because they match a broad pattern.

For a confirmed hard-coded secret within the authorized edit scope, replace it with a placeholder or a runtime credential lookup without corrupting the file or breaking a required format. Preserve behavior using an explicit missing-credential error or settings flow, not an invented replacement credential.

Never patch an unknown binary blindly. If the finding is outside the authorized edit scope or must remain in an external baseline, report its location safely, exclude it from redistribution, and describe the required remediation. Document any baseline redaction explicitly.

### 4. Native credential storage

Use Windows Credential Manager, an appropriate DPAPI-backed design, a managed secret facility, or explicitly provided process-environment values according to the application's needs. Do not implement custom encryption or embed a decryption key next to an encrypted credential.

Treat environment values as secrets, not as a secure logging surface. Do not propagate credentials to unrelated child processes. Keep real tokens out of resources, command lines, public configuration examples, `.props` files, and generated project metadata.

OS-protected local storage does not make a credential safe from every process running as that user. Document the actual protection scope and avoid absolute security claims.

### 5. Location-only audit trail

Create or update `secrets.md` with the audit scope, tools/checks performed, date, and findings or explicitly stated limitations. Never prepopulate it with invented findings or mark unperformed scans as clean.

Use a table like this, replacing the illustrative row with actual findings:

```markdown
# Secrets Audit

No secret values are stored in this report.

| File | Line | Category | Action | Status |
|---|---:|---|---|---|
| <relative-path> | <known-line-or-dash> | <category> | <remediation> | <status> |
```

Record only repository-relative location, reliable line number if known, general category, remediation, detection method, status, and audit time. Do not invent a line number. Keep ambiguous findings marked for review rather than claiming confirmed exposure.

### 6. Configuration and ignore rules

Ship only non-sensitive examples, such as empty API-key fields or names of environment variables. Preserve the user's existing local configuration instead of overwriting it during setup.

Include targeted ignore rules for generated and sensitive local files. Adapt the following to the repository; do not hide legitimate source fixtures or vendored libraries unnecessarily:

```gitignore
/build/
/dist/
/.local/
.vs/
*.suo
*.user
.env
.env.*
!.env.example
/config.local.json
/credentials/
```

Ignoring a file does not untrack a previously committed file. Check the tracked file list as well as `.gitignore`. Ensure release packaging uses an allowlist or equivalent controlled input set rather than archiving the entire checkout.

### 7. Previously exposed credentials

Removing a credential from the current working tree does not remove it from prior commits or revoke it. Mark exposed live credentials as requiring revocation/rotation and report that action without showing the value.

Inspect history when appropriate and authorized. Do not rewrite shared Git history automatically. Any destructive history rewrite requires an agreed process, and it is not a substitute for rotating an exposed credential.

### 8. Secret-safe CI and diagnostics

Use the CI platform's protected secret mechanism. Never embed literal credentials in workflow files or print the full environment, authorization headers, process command lines, or private build properties.

Keep unredacted crash dumps, binary build logs, diagnostic bundles, and signing inputs out of public artifacts. Limit retention and access. Review the release package and the audit report themselves for accidental disclosure.

### 9. Final secret verification

Re-scan remediated files and generated release inputs. Verify that configuration examples contain placeholders only, local credential files are excluded, CI uses protected inputs, `secrets.md` contains no values, and unresolved rotation requirements are visible.

Say “no findings in the checks performed,” not “guaranteed secret-free.” Distinguish an unavailable scanner from a successful scan with zero findings.

### 10. Safe automation rule

```text
SCAN -> CLASSIFY -> MASK / REFACTOR -> LOG LOCATION ONLY -> VERIFY
```

Never transform a secret-detection task into a credentials archive.

---

## Comprehensive Native Productionization Checklist

### Phase 1: Source ingestion and native processing

- [ ] Inspect original source, resources, dependencies, build files, and expected behavior.
- [ ] Record the native profile, assumptions, parity plan, and unavailable baseline evidence.
- [ ] Preserve original code/assets or document any authorized redactions and changes.
- [ ] Implement real native core logic and adapters without hidden interpreter dependencies.
- [ ] Create isolated managed temporary workspaces for applicable processing jobs.
- [ ] Keep user inputs and installation/repository folders out of cleanup scope.
- [ ] Implement actual job scheduling, progress, cancellation, errors, and persistence.
- [ ] Implement archive creation and native export where required.
- [ ] Separate durable history/results/checkpoints from disposable scratch space.
- [ ] Implement honest Continue/Resume/Retry/Discard and startup reconciliation.

### Phase 2: Native UI and history

- [ ] Implement a functioning MFC or explicitly requested Win32 application.
- [ ] Supply all source, headers, resource IDs, dialogs, icons, and manifest inputs.
- [ ] Implement work/history navigation or the documented domain equivalent.
- [ ] Support validated file selection, drops, and applicable clipboard payloads.
- [ ] Display truthful progress, redacted logs, and meaningful failures.
- [ ] Keep long-running operations off the UI thread with lifetime-safe notifications.
- [ ] Preserve UI responsiveness, cancellation, keyboard access, and resizing.
- [ ] Test claimed Unicode, accessibility, and DPI behavior.
- [ ] Show missing or expired outputs honestly and protect user-exported files.

### Phase 3: Setup, build, run, and package

- [ ] Discover and verify the selected MSVC, Windows SDK, MFC, and build tools.
- [ ] Provide one authoritative build system with correct Debug/Release mappings.
- [ ] Keep architecture, runtime linkage, MFC definitions, and dependencies consistent.
- [ ] Provide working setup/build/test/run/package scripts with checked exit codes.
- [ ] Avoid machine-global policy changes, silent installs, and fabricated platform support.
- [ ] Launch the real native executable with no required browser or local API server.
- [ ] Verify graceful close, worker ownership, and interruption recovery.
- [ ] Create a reproducible release package with necessary runtimes and notices.
- [ ] Test deployment on a clean supported Windows environment when available.

### Phase 4: Secrets hygiene and final verification

- [ ] Scan the actual authorized scope and record scanner availability/limitations.
- [ ] Remediate confirmed credentials safely and log locations only.
- [ ] Keep real secrets out of configuration examples, resources, logs, and packages.
- [ ] Mark exposed credentials for rotation and review history when appropriate.
- [ ] Re-scan `secrets.md`, remediated source, and release inputs.
- [ ] Run core/integration tests and a real end-to-end native workflow.
- [ ] Record commands, exit codes, test evidence, and untested configurations.
- [ ] Complete the parity matrix; identify remaining work without claiming completion.

## Required completion report

When this skill is used to implement a project, finish with the actual solution/project location, executable/package location if built, exact setup/build/test/run commands, configurations tested, important implemented features, known gaps, and the location/status of the secret audit.

Classify the outcome accurately: **implemented and verified**, **implemented but not verified on Windows**, or **partially implemented with specified gaps**. A clean-looking UI, successful configure step, or generated project file is not proof of a successful native build or complete behavioral parity.

## Provenance and technical references

**Source adaptation:** User-supplied `Pasted markdown.md`, “Project Productionization Standard.” Its six pillars, preservation requirement, document-ingestion modes, progress/history workflow, temporary-job isolation, ZIP packaging, setup/run automation, and credential-audit principles are retained or explicitly mapped above. Windows build/resource requirements, process-lifetime rules, durable-result retention, and checkpoint semantics are native-specific extensions.

The references below support platform-specific details; the defaults and acceptance criteria in this skill are engineering requirements chosen for this conversion. Verify version-sensitive behavior against the toolchain actually selected for each project. URLs are reference documentation, not instructions to install or execute anything automatically.

- **[R1] Microsoft Learn — MFC Desktop Applications.** `https://learn.microsoft.com/en-us/cpp/mfc/mfc-desktop-applications`
- **[R2] Microsoft Learn — Multithreading: MFC Programming Tips.** `https://learn.microsoft.com/en-us/cpp/parallel/multithreading-programming-tips`
- **[R3] Microsoft Learn — Shell Clipboard Formats.** `https://learn.microsoft.com/en-us/windows/win32/shell/clipboard`
- **[R4] Microsoft Learn — High DPI Desktop Application Development on Windows.** `https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows`
- **[R5] Microsoft Learn — GetTempPath2W.** `https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettemppath2w`
- **[R6] Microsoft Learn — Job Objects.** `https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects`
- **[R7] Microsoft Learn — Install C and C++ Support in Visual Studio.** `https://learn.microsoft.com/en-us/cpp/build/vscpp-step-0-installation`
- **[R8] Microsoft — vswhere: Find VC.** `https://github.com/microsoft/vswhere/wiki/Find-VC`
- **[R9] CMake — CMAKE_MFC_FLAG.** `https://cmake.org/cmake/help/latest/variable/CMAKE_MFC_FLAG.html`
- **[R10] Microsoft Learn — /MD, /MT, /LD (Use Runtime Library).** `https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library`
- **[R11] Microsoft Learn — Redistribute Visual C++ Files.** `https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files`
- **[R12] Microsoft Learn — Redistribute the MFC Library.** `https://learn.microsoft.com/en-us/cpp/windows/redistributing-the-mfc-library`
