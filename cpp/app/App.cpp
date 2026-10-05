#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include "MainWindow.h"
#include "CodeEditor.h"
#include <stdexcept>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' \
version='6.0.0.0' \
processorArchitecture='*' \
publicKeyToken='6595b64144ccf1df' \
language='*'\"")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR lpCmdLine, int nCmdShow) {
    // 1. Enable Per-Monitor V2 DPI awareness if supported
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 2. Initialize Common Controls
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_COOL_CLASSES | ICC_BAR_CLASSES |
                 ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES |
                 ICC_LISTVIEW_CLASSES | ICC_USEREX_CLASSES;
    InitCommonControlsEx(&icex);

    // 3. Initialize COM for OLE / Shell / Dialogs
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    if (lpCmdLine && wcsstr(lpCmdLine, L"--self-test")) {
        native_app::Theme::SetTheme(native_app::AppTheme::Dark);
        std::wstring failure;
        const bool contrast = native_app::Theme::VerifyContrast();
        const bool editor = native_app::CodeEditor::RunSelfTests(failure);
        const bool database = native_app::DbStudioView::RunSelfTests(failure);
        if(!contrast)failure+=L"Contrast ratios below 4.5:1; ";
        const std::wstring report = std::wstring(contrast && editor && database ? L"PASS" : L"FAIL") + L": native editor, database view and theme self-tests. " + failure + L"\r\n";
        AttachConsole(ATTACH_PARENT_PROCESS);
        DWORD written = 0; if(!WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), report.data(), static_cast<DWORD>(report.size()), &written, nullptr)) {
            const int count=WideCharToMultiByte(CP_UTF8,0,report.data(),static_cast<int>(report.size()),nullptr,0,nullptr,nullptr);
            std::string utf8(count,'\0');WideCharToMultiByte(CP_UTF8,0,report.data(),static_cast<int>(report.size()),utf8.data(),count,nullptr,nullptr);
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),utf8.data(),static_cast<DWORD>(utf8.size()),&written,nullptr);
        }
        OutputDebugStringW(report.c_str());
        native_app::Theme::Shutdown();
        if(SUCCEEDED(hr))CoUninitialize();
        return contrast && editor && database ? 0 : 1;
    }

    // 4. Create and show main application window
    int exitCode = 0;
    try {
    native_app::MainWindow mainWin;
    if (!mainWin.Create(hInstance, nCmdShow, 1366, 850)) {
        MessageBoxW(NULL, L"Failed to initialize DataForge Studio main window.", L"Fatal Error", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(hr)) CoUninitialize();
        return 1;
    }

    // 5. Message Loop
    MSG msg{};
    int result = 0;
    while ((result = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if(!mainWin.ProcessMessage(msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    exitCode = result < 0 ? 1 : static_cast<int>(msg.wParam);
    } catch(const std::exception&) {
        MessageBoxW(nullptr,L"The workspace could not be opened. Check that the DataForgeStudio folder in Local AppData is writable and contains valid data. Existing files have been preserved.",L"DataForge Studio",MB_ICONERROR);
        exitCode=1;
    }
    native_app::Theme::Shutdown();

    // 6. Cleanup COM
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }

    return exitCode;
}
