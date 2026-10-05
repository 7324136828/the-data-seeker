# Third-party notices

This Windows x64 application uses C++17/Win32 with compiled native C/C++ dependencies. It does not bundle Python, React, Node.js or a browser. Its optional mock API server runs inside the native executable. The project's Apache License 2.0 is supplied in LICENSE.

| Dependency | Version/source | License and deployment |
|---|---|---|
| JSON for Modern C++, Niels Lohmann | Vendored cpp/third_party/nlohmann/json.hpp 3.12.0; https://github.com/nlohmann/json | MIT; embedded Apache-2.0/CC0-1.0 attribution below. Header-only, compiled into the executable. |
| QuickJS-NG | 0.17.0, commit `6d46d07d04041b40f4f49eaa7fdebe44c314c699`; https://github.com/quickjs-ng/quickjs/tree/v0.17.0 | MIT, supplied in `licenses/quickjs-ng-LICENSE.txt`. Four C translation units are compiled with MSVC into a static library. No command-line interpreter, standard-library filesystem/process bridge or module loader is included. It executes user request scripts inside the native process. |
| yaml-cpp | 0.9.0, commit `56e3bb550c91fd7005566f19c079cb7a503223cf`; https://github.com/jbeder/yaml-cpp/tree/yaml-cpp-0.9.0 | MIT, supplied in `licenses/yaml-cpp-LICENSE.txt`; embedded Dragonbox attribution in `licenses/yaml-cpp-dragonbox-NOTICE.txt`. MSVC static library with `YAML_CPP_STATIC_DEFINE`; native YAML imports. |
| DuckDB | Official Windows amd64 1.5.6 library; https://github.com/duckdb/duckdb/releases/tag/v1.5.6 | MIT, supplied in `licenses/duckdb-LICENSE.txt`. `duckdb.dll` is distributed beside the executable; linked through its matching import library and C header. |
| MongoDB C Driver / libbson | 2.5.5; https://github.com/mongodb/mongo-c-driver/releases/tag/2.5.5 | Apache-2.0 and component notices, supplied in `licenses/mongo-c-driver-COPYING.txt` and `licenses/mongo-c-driver-THIRD_PARTY_NOTICES.txt`. Locally built MSVC x64 Release `/MD`, shared `bson2.dll` and `mongoc2.dll`. Uses Windows TLS/SSPI; SRV enabled, optional Snappy/Zlib/Zstd compression disabled. |
| Microsoft Windows SDK APIs (WinHTTP, Windows SQLite, RichEdit, common controls, GDI, COM, shell, DPAPI) | Windows SDK 10.0.26100.0 default | Microsoft SDK/Windows terms; SQLite incorporates public-domain SQLite. SDK import libraries; system DLLs supplied by supported Windows 10/11, not copied from the developer machine. |
| Microsoft Visual C++ runtime | Verified v145 tools directory 14.51.36231, compiler file 14.51.36256.0, x64 redistributable 14.51.36247.0 | Microsoft Visual C++ Redistributable terms. Release /MD; official locally installed vc_redist.x64.exe is bundled after Microsoft signature verification. Installation is explicit and displays Microsoft's terms; no debug runtimes are shipped. |

Native ZIP writing uses the project's stored-entry ZIP writer; no compression dependency is added. This existing Win32 application does not link MFC. PowerShell/.NET are developer automation dependencies and are not needed when running the packaged application directly.

SQL-family remote connections use Windows ODBC. Their vendor's compatible Windows x64 driver is installed separately and is not redistributed by this package. Redis transport uses the project's native RESP implementation and Windows TLS APIs. The active MongoDB runtime comes from `cpp/third_party/mongodb-msvc`; the alternative `mongodb` development directory is excluded from release packaging.

QuickJS and yaml-cpp source inventories and SHA256 provenance are recorded in their vendored `SOURCE.json` files. The saved official DuckDB 1.5.6 ZIP SHA256 is `44cf59583f9951d2cb09b1bf115a63ecb2d8901e363903029d86c7d8683fe96a`; the MongoDB 2.5.5 source archive SHA256 is `fa255802fe748b98d4464e07223e87a44c88c02c35398d7607b2f02e967395b1`. These downloaded archives are private build inputs and are excluded from the release ZIP. The request scripting compatibility prelude is adapted from the original project's `script_bootstrap.js`.

## MIT license and attributions

Copyright (c) 2013-2026 Niels Lohmann
Copyright (c) 2016-2021 Evan Nemerson
Copyright (c) 2008, 2009 Björn Hoehrmann
Copyright (c) 2009 Florian Loitsch
Copyright (c) 2021 The fast_float authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Embedded JSON components

The supplied amalgamation retains additional attribution to The Abseil Authors (2018, MIT AND Apache-2.0), Evan Nemerson's Hedley macros (CC0-1.0 original declaration), Björn Hoehrmann's UTF-8 decoder (MIT), Florian Loitsch's floating-point conversion (MIT), and the fast_float authors (MIT/Apache-2.0 where marked). Apache License 2.0 is included in LICENSE; CC0-1.0 legal text: https://creativecommons.org/publicdomain/zero/1.0/legalcode.

Actual vendored-header SHA256: ecfb9626b0aa4cb5ae2222279ce39298e6abf094dae28a60284d33fcd1b03981. Setup does not download a mutable upstream source snapshot.
