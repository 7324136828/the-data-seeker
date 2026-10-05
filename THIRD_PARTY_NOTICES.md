# Third-party notices

The Windows application is native C++17/Win32 x64. It does not bundle Python, Node.js, React, a browser, or a web server. The project license is supplied separately in LICENSE (Apache License 2.0).

| Dependency | Pinned source/version | License | Integration and deployment |
|---|---|---|---|
| JSON for Modern C++ by Niels Lohmann | Vendored cpp/third_party/nlohmann/json.hpp, version 3.12.0; https://github.com/nlohmann/json | MIT with embedded MIT/Apache-2.0/CC0-1.0 components; full attribution below | Header-only, compiled with the same MSVC settings as owned code; no runtime DLL. |
| Microsoft Windows SDK APIs: WinHTTP, Windows SQLite, RichEdit, common controls, shell, GDI, COM and DPAPI | Windows SDK 10.0.26100.0 build default | Microsoft Windows/SDK terms; Windows SQLite incorporates public-domain SQLite | Linked against SDK import libraries; corresponding system DLLs are supplied by supported Windows 10/11 x64, not copied from the developer machine. |
| Microsoft Visual C++ runtime | MSVC v145, tools directory 14.51.36231, compiler 14.51.36256.0 and redistributable 14.51.36247.0 in the verified build | Microsoft Visual Studio/Visual C++ Redistributable license terms | /MD Release; official locally installed vc_redist.x64.exe is included in the Release package. Installation is explicit and displays Microsoft's terms. Debug runtimes are excluded. |

Native ZIP creation uses the project's own stored-entry ZIP implementation; it introduces no third-party compression library. No MFC library is linked by this existing Win32 implementation. PowerShell and .NET are used only for developer setup and packaging scripts, not for normal application operation.

## JSON for Modern C++ — MIT license

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


## Embedded components in the JSON amalgamation

The vendored file retains these source attribution notices:

- Hedley, Evan Nemerson, 2016-2021: MIT attribution for the amalgamation, with the original Hedley macros declared CC0-1.0 (https://creativecommons.org/publicdomain/zero/1.0/legalcode).
- Björn Hoehrmann, 2008 and 2009: UTF-8 decoder, MIT.
- Florian Loitsch, 2009: floating-point conversion code, MIT.
- The fast_float authors, 2021: parsing/conversion code, MIT (and Apache-2.0 where marked in the amalgamation).
- The Abseil Authors, 2018: template utilities marked MIT AND Apache-2.0. Apache License 2.0 is supplied in the package's LICENSE file.

Vendored header SHA256: `ecfb9626b0aa4cb5ae2222279ce39298e6abf094dae28a60284d33fcd1b03981`. This identifies the actual supplied source snapshot; setup does not download a mutable upstream copy.
