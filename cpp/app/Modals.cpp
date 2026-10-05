#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <commdlg.h>
#include "Modals.h"
#include "Theme.h"
#include "CurlParser.h"
#include "OpenApiParser.h"
#include "CodeGen.h"
#include <vector>
#include <string>
#include <sstream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

namespace native_app {

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], size);
    return result;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size, NULL, NULL);
    return result;
}

static void CenterWindow(HWND hwnd, HWND hParent) {
    RECT rcParent = { 0 }, rcDlg = { 0 };
    if (hParent && IsWindow(hParent)) {
        GetWindowRect(hParent, &rcParent);
    } else {
        rcParent.right = GetSystemMetrics(SM_CXSCREEN);
        rcParent.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    GetWindowRect(hwnd, &rcDlg);
    int dlgWidth = rcDlg.right - rcDlg.left;
    int dlgHeight = rcDlg.bottom - rcDlg.top;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgWidth) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgHeight) / 2;
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

static void RunModalLoop(HWND hwnd, HWND hParent) {
    EnableWindow(hParent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
}

static void SetClipboardText(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        memcpy(GlobalLock(hMem), text.c_str(), bytes);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

// =========================================================================
// 1. Environments Editor Dialog
// =========================================================================
struct EnvDialogContext {
    StoreManager* store = nullptr;
    std::function<void()> onUpdate;
    EnvironmentStore envStore;
    int selectedEnvIndex = -1;
    HWND hEnvList = NULL;
    HWND hVarList = NULL;
    HWND hEnvNameEdit = NULL;
    HWND hKeyEdit = NULL;
    HWND hValEdit = NULL;
    HWND hSecretCheck = NULL;
    HFONT hFont = NULL;
    HBRUSH hBgBrush = NULL;
};

static LRESULT CALLBACK EnvDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    EnvDialogContext* ctx = reinterpret_cast<EnvDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<EnvDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const auto& colors = Theme::Get();
        ctx->hFont = Theme::GetMainFont();
        ctx->hBgBrush = CreateSolidBrush(colors.bgSecondary);

        ctx->envStore = ctx->store->GetEnvironments();
        if (ctx->envStore.environments.empty()) {
            Environment defaultEnv;
            defaultEnv.id = "env_dev";
            defaultEnv.name = "Development";
            ctx->envStore.environments.push_back(defaultEnv);
            ctx->envStore.activeId = defaultEnv.id;
        }
        ctx->selectedEnvIndex = 0;

        // Title
        HWND hTitle = CreateWindowExW(0, L"STATIC", L"Environments Manager", WS_CHILD | WS_VISIBLE,
            20, 15, 300, 24, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        // Left: Environments list
        HWND hLblEnv = CreateWindowExW(0, L"STATIC", L"Environments", WS_CHILD | WS_VISIBLE,
            20, 50, 180, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hLblEnv, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hEnvList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
            20, 75, 180, 320, hwnd, (HMENU)1001, NULL, NULL);
        SendMessageW(ctx->hEnvList, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnNewEnv = CreateWindowExW(0, L"BUTTON", L"+ New", WS_CHILD | WS_VISIBLE,
            20, 405, 85, 28, hwnd, (HMENU)1002, NULL, NULL);
        SendMessageW(hBtnNewEnv, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnDelEnv = CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE,
            115, 405, 85, 28, hwnd, (HMENU)1003, NULL, NULL);
        SendMessageW(hBtnDelEnv, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        // Right: Environment Details
        HWND hLblName = CreateWindowExW(0, L"STATIC", L"Environment Name:", WS_CHILD | WS_VISIBLE,
            220, 50, 150, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hLblName, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hEnvNameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            220, 75, 480, 26, hwnd, (HMENU)1004, NULL, NULL);
        SendMessageW(ctx->hEnvNameEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        // Variables List
        HWND hLblVars = CreateWindowExW(0, L"STATIC", L"Variables (Key - Value):", WS_CHILD | WS_VISIBLE,
            220, 110, 200, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hLblVars, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hVarList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            220, 135, 480, 200, hwnd, (HMENU)1005, NULL, NULL);
        SendMessageW(ctx->hVarList, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);
        ListView_SetExtendedListViewStyle(ctx->hVarList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 160; col.pszText = (LPWSTR)L"Variable Key";
        ListView_InsertColumn(ctx->hVarList, 0, &col);
        col.cx = 240; col.pszText = (LPWSTR)L"Value";
        ListView_InsertColumn(ctx->hVarList, 1, &col);
        col.cx = 70; col.pszText = (LPWSTR)L"Secret";
        ListView_InsertColumn(ctx->hVarList, 2, &col);

        // Variable editor inputs
        ctx->hKeyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            220, 345, 140, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hKeyEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hValEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            370, 345, 180, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hValEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hSecretCheck = CreateWindowExW(0, L"BUTTON", L"Secret", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            560, 345, 65, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hSecretCheck, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnAddVar = CreateWindowExW(0, L"BUTTON", L"Set", WS_CHILD | WS_VISIBLE,
            635, 345, 65, 26, hwnd, (HMENU)1006, NULL, NULL);
        SendMessageW(hBtnAddVar, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnDelVar = CreateWindowExW(0, L"BUTTON", L"Remove Selected Variable", WS_CHILD | WS_VISIBLE,
            220, 380, 200, 26, hwnd, (HMENU)1007, NULL, NULL);
        SendMessageW(hBtnDelVar, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        // Bottom dialog buttons
        HWND hBtnSave = CreateWindowExW(0, L"BUTTON", L"Save & Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            490, 420, 100, 30, hwnd, (HMENU)IDOK, NULL, NULL);
        SendMessageW(hBtnSave, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            600, 420, 100, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        // Populate environments
        for (const auto& env : ctx->envStore.environments) {
            SendMessageW(ctx->hEnvList, LB_ADDSTRING, 0, (LPARAM)Utf8ToWide(env.name).c_str());
        }
        SendMessageW(ctx->hEnvList, LB_SETCURSEL, 0, 0);

        // Load first env
        auto loadEnv = [ctx](int idx) {
            if (idx < 0 || idx >= (int)ctx->envStore.environments.size()) return;
            const auto& env = ctx->envStore.environments[idx];
            SetWindowTextW(ctx->hEnvNameEdit, Utf8ToWide(env.name).c_str());
            ListView_DeleteAllItems(ctx->hVarList);
            int row = 0;
            for (const auto& v : env.variables) {
                LVITEMW item = { 0 };
                item.mask = LVIF_TEXT;
                item.iItem = row;
                std::wstring wKey = Utf8ToWide(v.key);
                item.pszText = (LPWSTR)wKey.c_str();
                ListView_InsertItem(ctx->hVarList, &item);

                std::wstring wVal = Utf8ToWide(v.value);
                ListView_SetItemText(ctx->hVarList, row, 1, (LPWSTR)wVal.c_str());
                ListView_SetItemText(ctx->hVarList, row, 2, (LPWSTR)L"No");
                row++;
            }
        };
        loadEnv(0);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wParam;
        const auto& colors = Theme::Get();
        SetTextColor(hdc, colors.textPrimary);
        SetBkColor(hdc, colors.bgSecondary);
        return (INT_PTR)ctx->hBgBrush;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        if (wmId == 1001 && wmEvent == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(ctx->hEnvList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)ctx->envStore.environments.size()) {
                ctx->selectedEnvIndex = sel;
                const auto& env = ctx->envStore.environments[sel];
                SetWindowTextW(ctx->hEnvNameEdit, Utf8ToWide(env.name).c_str());
                ListView_DeleteAllItems(ctx->hVarList);
                int row = 0;
                for (const auto& v : env.variables) {
                    LVITEMW item = { 0 };
                    item.mask = LVIF_TEXT;
                    item.iItem = row;
                    std::wstring wKey = Utf8ToWide(v.key);
                    item.pszText = (LPWSTR)wKey.c_str();
                    ListView_InsertItem(ctx->hVarList, &item);

                    std::wstring wVal = Utf8ToWide(v.value);
                    ListView_SetItemText(ctx->hVarList, row, 1, (LPWSTR)wVal.c_str());
                    ListView_SetItemText(ctx->hVarList, row, 2, (LPWSTR)L"No");
                    row++;
                }
            }
        }
        else if (wmId == 1002) { // New Env
            Environment newEnv;
            newEnv.id = "env_" + std::to_string(GetTickCount64());
            newEnv.name = "New Environment";
            ctx->envStore.environments.push_back(newEnv);
            int idx = (int)SendMessageW(ctx->hEnvList, LB_ADDSTRING, 0, (LPARAM)L"New Environment");
            SendMessageW(ctx->hEnvList, LB_SETCURSEL, idx, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1001, LBN_SELCHANGE), 0);
        }
        else if (wmId == 1003) { // Delete Env
            if (ctx->envStore.environments.size() <= 1) {
                MessageBoxW(hwnd, L"Cannot delete the only environment.", L"Info", MB_ICONINFORMATION);
                return 0;
            }
            int sel = ctx->selectedEnvIndex;
            if (sel >= 0 && sel < (int)ctx->envStore.environments.size()) {
                ctx->envStore.environments.erase(ctx->envStore.environments.begin() + sel);
                SendMessageW(ctx->hEnvList, LB_DELETESTRING, sel, 0);
                int nextSel = (sel >= (int)ctx->envStore.environments.size()) ? (int)ctx->envStore.environments.size() - 1 : sel;
                SendMessageW(ctx->hEnvList, LB_SETCURSEL, nextSel, 0);
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1001, LBN_SELCHANGE), 0);
            }
        }
        else if (wmId == 1006) { // Add/Update Variable
            if (ctx->selectedEnvIndex < 0 || ctx->selectedEnvIndex >= (int)ctx->envStore.environments.size()) return 0;
            wchar_t kBuf[256] = { 0 }, vBuf[1024] = { 0 };
            GetWindowTextW(ctx->hKeyEdit, kBuf, 255);
            GetWindowTextW(ctx->hValEdit, vBuf, 1023);

            std::string key = WideToUtf8(kBuf);
            std::string val = WideToUtf8(vBuf);
            if (key.empty()) return 0;

            auto& env = ctx->envStore.environments[ctx->selectedEnvIndex];
            bool found = false;
            for (auto& v : env.variables) {
                if (v.key == key) {
                    v.value = val;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Variable v;
                v.key = key;
                v.value = val;
                v.enabled = true;
                env.variables.push_back(v);
            }
            // Refresh list
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1001, LBN_SELCHANGE), 0);
            SetWindowTextW(ctx->hKeyEdit, L"");
            SetWindowTextW(ctx->hValEdit, L"");
            SendMessageW(ctx->hSecretCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        }
        else if (wmId == 1007) { // Remove variable
            if (ctx->selectedEnvIndex < 0 || ctx->selectedEnvIndex >= (int)ctx->envStore.environments.size()) return 0;
            int sel = ListView_GetNextItem(ctx->hVarList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)ctx->envStore.environments[ctx->selectedEnvIndex].variables.size()) {
                ctx->envStore.environments[ctx->selectedEnvIndex].variables.erase(
                    ctx->envStore.environments[ctx->selectedEnvIndex].variables.begin() + sel);
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1001, LBN_SELCHANGE), 0);
            }
        }
        else if (wmId == IDOK) { // Save & Close
            if (ctx->selectedEnvIndex >= 0 && ctx->selectedEnvIndex < (int)ctx->envStore.environments.size()) {
                wchar_t nameBuf[256] = { 0 };
                GetWindowTextW(ctx->hEnvNameEdit, nameBuf, 255);
                std::string name = WideToUtf8(nameBuf);
                if (!name.empty()) ctx->envStore.environments[ctx->selectedEnvIndex].name = name;
            }
            ctx->store->SaveEnvironments(ctx->envStore);
            if (ctx->onUpdate) ctx->onUpdate();
            DestroyWindow(hwnd);
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowEnvironmentsDialog(HWND hParent, StoreManager* store, std::function<void()> onUpdate) {
    EnvDialogContext ctx;
    ctx.store = store;
    ctx.onUpdate = onUpdate;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = EnvDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DataForge_EnvDialog";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Manage Environments - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 740, 500, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// =========================================================================
// 2. Globals Editor Dialog
// =========================================================================
struct GlobalsDialogContext {
    StoreManager* store = nullptr;
    std::function<void()> onUpdate;
    std::vector<Variable> vars;
    HWND hVarList = NULL;
    HWND hKeyEdit = NULL;
    HWND hValEdit = NULL;
    HWND hSecretCheck = NULL;
    HFONT hFont = NULL;
    HBRUSH hBgBrush = NULL;
};

static LRESULT CALLBACK GlobalsDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    GlobalsDialogContext* ctx = reinterpret_cast<GlobalsDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<GlobalsDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const auto& colors = Theme::Get();
        ctx->hFont = Theme::GetMainFont();
        ctx->hBgBrush = CreateSolidBrush(colors.bgSecondary);
        ctx->vars = ctx->store->GetGlobals();

        HWND hTitle = CreateWindowExW(0, L"STATIC", L"Global Variables (Accessible across all environments)", WS_CHILD | WS_VISIBLE,
            20, 15, 500, 24, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        ctx->hVarList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            20, 50, 540, 240, hwnd, (HMENU)2001, NULL, NULL);
        SendMessageW(ctx->hVarList, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);
        ListView_SetExtendedListViewStyle(ctx->hVarList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 180; col.pszText = (LPWSTR)L"Variable Key";
        ListView_InsertColumn(ctx->hVarList, 0, &col);
        col.cx = 260; col.pszText = (LPWSTR)L"Current Value";
        ListView_InsertColumn(ctx->hVarList, 1, &col);
        col.cx = 80; col.pszText = (LPWSTR)L"Secret";
        ListView_InsertColumn(ctx->hVarList, 2, &col);

        // Edit fields
        ctx->hKeyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 305, 170, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hKeyEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hValEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            200, 305, 200, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hValEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hSecretCheck = CreateWindowExW(0, L"BUTTON", L"Secret", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            410, 305, 70, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hSecretCheck, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnAdd = CreateWindowExW(0, L"BUTTON", L"Set", WS_CHILD | WS_VISIBLE,
            490, 305, 70, 26, hwnd, (HMENU)2002, NULL, NULL);
        SendMessageW(hBtnAdd, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnDel = CreateWindowExW(0, L"BUTTON", L"Remove Selected", WS_CHILD | WS_VISIBLE,
            20, 340, 150, 26, hwnd, (HMENU)2003, NULL, NULL);
        SendMessageW(hBtnDel, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnSave = CreateWindowExW(0, L"BUTTON", L"Save & Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            340, 380, 105, 30, hwnd, (HMENU)IDOK, NULL, NULL);
        SendMessageW(hBtnSave, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            455, 380, 105, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        auto refresh = [ctx]() {
            ListView_DeleteAllItems(ctx->hVarList);
            int row = 0;
            for (const auto& v : ctx->vars) {
                LVITEMW item = { 0 };
                item.mask = LVIF_TEXT;
                item.iItem = row;
                std::wstring wKey = Utf8ToWide(v.key);
                item.pszText = (LPWSTR)wKey.c_str();
                ListView_InsertItem(ctx->hVarList, &item);

                std::wstring wVal = Utf8ToWide(v.value);
                ListView_SetItemText(ctx->hVarList, row, 1, (LPWSTR)wVal.c_str());
                ListView_SetItemText(ctx->hVarList, row, 2, (LPWSTR)L"No");
                row++;
            }
        };
        refresh();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wParam;
        const auto& colors = Theme::Get();
        SetTextColor(hdc, colors.textPrimary);
        SetBkColor(hdc, colors.bgSecondary);
        return (INT_PTR)ctx->hBgBrush;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == 2002) { // Add/Set
            wchar_t kBuf[256] = { 0 }, vBuf[1024] = { 0 };
            GetWindowTextW(ctx->hKeyEdit, kBuf, 255);
            GetWindowTextW(ctx->hValEdit, vBuf, 1023);
            std::string key = WideToUtf8(kBuf);
            std::string val = WideToUtf8(vBuf);
            if (key.empty()) return 0;

            bool found = false;
            for (auto& v : ctx->vars) {
                if (v.key == key) {
                    v.value = val;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Variable v;
                v.key = key;
                v.value = val;
                v.enabled = true;
                ctx->vars.push_back(v);
            }
            // Refresh
            ListView_DeleteAllItems(ctx->hVarList);
            int row = 0;
            for (const auto& v : ctx->vars) {
                LVITEMW item = { 0 };
                item.mask = LVIF_TEXT;
                item.iItem = row;
                std::wstring wKey = Utf8ToWide(v.key);
                item.pszText = (LPWSTR)wKey.c_str();
                ListView_InsertItem(ctx->hVarList, &item);

                std::wstring wVal = Utf8ToWide(v.value);
                ListView_SetItemText(ctx->hVarList, row, 1, (LPWSTR)wVal.c_str());
                ListView_SetItemText(ctx->hVarList, row, 2, (LPWSTR)L"No");
                row++;
            }
            SetWindowTextW(ctx->hKeyEdit, L"");
            SetWindowTextW(ctx->hValEdit, L"");
            SendMessageW(ctx->hSecretCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        }
        else if (wmId == 2003) { // Remove
            int sel = ListView_GetNextItem(ctx->hVarList, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < (int)ctx->vars.size()) {
                ctx->vars.erase(ctx->vars.begin() + sel);
                ListView_DeleteItem(ctx->hVarList, sel);
            }
        }
        else if (wmId == IDOK) { // Save
            ctx->store->SaveGlobals(ctx->vars);
            if (ctx->onUpdate) ctx->onUpdate();
            DestroyWindow(hwnd);
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowGlobalsDialog(HWND hParent, StoreManager* store, std::function<void()> onUpdate) {
    GlobalsDialogContext ctx;
    ctx.store = store;
    ctx.onUpdate = onUpdate;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = GlobalsDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DataForge_GlobalsDialog";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Global Variables - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 595, 460, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// =========================================================================
// 3. cURL Import Dialog
// =========================================================================
struct CurlImportContext {
    std::function<void(const ApiRequest&)> onImport;
    HWND hEdit = NULL;
    HFONT hFont = NULL;
    HBRUSH hBgBrush = NULL;
};

static LRESULT CALLBACK CurlDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CurlImportContext* ctx = reinterpret_cast<CurlImportContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<CurlImportContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const auto& colors = Theme::Get();
        ctx->hFont = Theme::GetCodeFont();
        ctx->hBgBrush = CreateSolidBrush(colors.bgSecondary);

        HWND hTitle = CreateWindowExW(0, L"STATIC", L"Paste cURL command below to import as an API request:", WS_CHILD | WS_VISIBLE,
            20, 15, 540, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        ctx->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            L"curl -X POST https://api.example.com/v1/auth/login \\\n"
            L"  -H \"Content-Type: application/json\" \\\n"
            L"  -d '{\"username\": \"admin\", \"password\": \"secret\"}'",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            20, 45, 560, 220, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnImport = CreateWindowExW(0, L"BUTTON", L"Import Request", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            350, 280, 120, 30, hwnd, (HMENU)IDOK, NULL, NULL);
        SendMessageW(hBtnImport, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            480, 280, 100, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wParam;
        const auto& colors = Theme::Get();
        SetTextColor(hdc, colors.textPrimary);
        SetBkColor(hdc, colors.bgSecondary);
        return (INT_PTR)ctx->hBgBrush;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDOK) {
            int len = GetWindowTextLengthW(ctx->hEdit);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(ctx->hEdit, buf.data(), len + 1);
                std::string curlCmd = WideToUtf8(buf.data());

                auto reqOpt = CurlParser::Parse(curlCmd);
                if (reqOpt.has_value()) {
                    if (ctx->onImport) ctx->onImport(reqOpt.value());
                    DestroyWindow(hwnd);
                } else {
                    MessageBoxW(hwnd, L"Could not parse cURL command. Please ensure it starts with 'curl' and contains a valid URL.",
                        L"Import Error", MB_ICONERROR);
                }
            }
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowCurlImportDialog(HWND hParent, std::function<void(const ApiRequest&)> onImport) {
    CurlImportContext ctx;
    ctx.onImport = onImport;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = CurlDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DataForge_CurlDialog";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Import from cURL - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 615, 360, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// =========================================================================
// 4. OpenAPI Import Dialog
// =========================================================================
struct OpenApiDialogContext {
    StoreManager* store = nullptr;
    std::function<void()> onImport;
    HWND hEdit = NULL;
    HFONT hFont = NULL;
    HBRUSH hBgBrush = NULL;
};

static LRESULT CALLBACK OpenApiDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    OpenApiDialogContext* ctx = reinterpret_cast<OpenApiDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<OpenApiDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const auto& colors = Theme::Get();
        ctx->hFont = Theme::GetCodeFont();
        ctx->hBgBrush = CreateSolidBrush(colors.bgSecondary);

        HWND hTitle = CreateWindowExW(0, L"STATIC", L"Paste OpenAPI 3.0 / Swagger 2.0 JSON or load a file:", WS_CHILD | WS_VISIBLE,
            20, 15, 540, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnFile = CreateWindowExW(0, L"BUTTON", L"Browse File...", WS_CHILD | WS_VISIBLE,
            460, 10, 120, 26, hwnd, (HMENU)3001, NULL, NULL);
        SendMessageW(hBtnFile, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        ctx->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            20, 45, 560, 240, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        HWND hBtnImport = CreateWindowExW(0, L"BUTTON", L"Import Collections", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            330, 300, 140, 30, hwnd, (HMENU)IDOK, NULL, NULL);
        SendMessageW(hBtnImport, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            480, 300, 100, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wParam;
        const auto& colors = Theme::Get();
        SetTextColor(hdc, colors.textPrimary);
        SetBkColor(hdc, colors.bgSecondary);
        return (INT_PTR)ctx->hBgBrush;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == 3001) { // Browse file
            wchar_t szFile[MAX_PATH] = { 0 };
            OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"OpenAPI / JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                HANDLE hF = CreateFileW(szFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                if (hF != INVALID_HANDLE_VALUE) {
                    DWORD dwSize = GetFileSize(hF, NULL);
                    if (dwSize > 0 && dwSize < 10 * 1024 * 1024) {
                        std::vector<char> buf(dwSize);
                        DWORD dwRead = 0;
                        ReadFile(hF, buf.data(), dwSize, &dwRead, NULL);
                        std::string jsonStr(buf.data(), dwRead);
                        SetWindowTextW(ctx->hEdit, Utf8ToWide(jsonStr).c_str());
                    }
                    CloseHandle(hF);
                }
            }
        }
        else if (wmId == IDOK) {
            int len = GetWindowTextLengthW(ctx->hEdit);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(ctx->hEdit, buf.data(), len + 1);
                std::string spec = WideToUtf8(buf.data());

                auto cols = OpenApiParser::Parse(spec);
                if (!cols.empty()) {
                    auto existingCols = ctx->store->GetCollections();
                    for (const auto& c : cols) {
                        existingCols.push_back(c);
                    }
                    ctx->store->SaveCollections(existingCols);
                    if (ctx->onImport) ctx->onImport();
                    MessageBoxW(hwnd, (L"Successfully imported " + std::to_wstring(cols.size()) + L" collection(s)!").c_str(),
                        L"Import Success", MB_OK | MB_ICONINFORMATION);
                    DestroyWindow(hwnd);
                } else {
                    MessageBoxW(hwnd, L"Could not parse OpenAPI specification. Please ensure valid OpenAPI 3.0 or Swagger 2.0 JSON.",
                        L"Import Error", MB_ICONERROR);
                }
            }
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowOpenApiImportDialog(HWND hParent, StoreManager* store, std::function<void()> onImport) {
    OpenApiDialogContext ctx;
    ctx.store = store;
    ctx.onImport = onImport;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = OpenApiDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DataForge_OpenApiDialog";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Import OpenAPI / Swagger - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 615, 380, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// =========================================================================
// 5. Code Generator Dialog
// =========================================================================
struct CodeGenContext {
    ApiRequest req;
    HWND hCodeEdit = NULL;
    HWND hRadioCurl = NULL;
    HWND hRadioPy = NULL;
    HWND hRadioFetch = NULL;
    HWND hRadioAxios = NULL;
    HFONT hFont = NULL;
    HBRUSH hBgBrush = NULL;
};

static void UpdateGeneratedCode(CodeGenContext* ctx) {
    std::string lang = "curl";
    if (SendMessageW(ctx->hRadioPy, BM_GETCHECK, 0, 0) == BST_CHECKED) lang = "python";
    else if (SendMessageW(ctx->hRadioFetch, BM_GETCHECK, 0, 0) == BST_CHECKED) lang = "fetch";
    else if (SendMessageW(ctx->hRadioAxios, BM_GETCHECK, 0, 0) == BST_CHECKED) lang = "axios";

    std::string code = CodeGen::Generate(ctx->req, lang);
    SetWindowTextW(ctx->hCodeEdit, Utf8ToWide(code).c_str());
}

static LRESULT CALLBACK CodeGenDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CodeGenContext* ctx = reinterpret_cast<CodeGenContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<CodeGenContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const auto& colors = Theme::Get();
        ctx->hFont = Theme::GetCodeFont();
        ctx->hBgBrush = CreateSolidBrush(colors.bgSecondary);

        HWND hTitle = CreateWindowExW(0, L"STATIC", L"Generate Client Code Snippet", WS_CHILD | WS_VISIBLE,
            20, 15, 300, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        // Language Radios
        ctx->hRadioCurl = CreateWindowExW(0, L"BUTTON", L"cURL", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            20, 45, 80, 24, hwnd, (HMENU)4001, NULL, NULL);
        SendMessageW(ctx->hRadioCurl, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);
        SendMessageW(ctx->hRadioCurl, BM_SETCHECK, BST_CHECKED, 0);

        ctx->hRadioPy = CreateWindowExW(0, L"BUTTON", L"Python (requests)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            110, 45, 140, 24, hwnd, (HMENU)4002, NULL, NULL);
        SendMessageW(ctx->hRadioPy, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        ctx->hRadioFetch = CreateWindowExW(0, L"BUTTON", L"JavaScript (fetch)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            260, 45, 140, 24, hwnd, (HMENU)4003, NULL, NULL);
        SendMessageW(ctx->hRadioFetch, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        ctx->hRadioAxios = CreateWindowExW(0, L"BUTTON", L"JavaScript (axios)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            410, 45, 140, 24, hwnd, (HMENU)4004, NULL, NULL);
        SendMessageW(ctx->hRadioAxios, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        // Code Edit box
        ctx->hCodeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            20, 80, 640, 300, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hCodeEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        // Copy button
        HWND hBtnCopy = CreateWindowExW(0, L"BUTTON", L"Copy to Clipboard", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            410, 395, 140, 30, hwnd, (HMENU)4005, NULL, NULL);
        SendMessageW(hBtnCopy, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnClose = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
            560, 395, 100, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hBtnClose, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        UpdateGeneratedCode(ctx);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wParam;
        const auto& colors = Theme::Get();
        SetTextColor(hdc, colors.textPrimary);
        SetBkColor(hdc, colors.bgSecondary);
        return (INT_PTR)ctx->hBgBrush;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId >= 4001 && wmId <= 4004) {
            UpdateGeneratedCode(ctx);
        }
        else if (wmId == 4005) { // Copy
            int len = GetWindowTextLengthW(ctx->hCodeEdit);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(ctx->hCodeEdit, buf.data(), len + 1);
                SetClipboardText(hwnd, buf.data());
                MessageBoxW(hwnd, L"Code copied to clipboard!", L"Success", MB_OK | MB_ICONINFORMATION);
            }
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowCodeGenDialog(HWND hParent, const ApiRequest& req) {
    CodeGenContext ctx;
    ctx.req = req;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = CodeGenDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DataForge_CodeGenDialog";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Code Snippet Generator - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 695, 480, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// =========================================================================
// 6. New Database Dialog
// =========================================================================
struct NewDbDialogContext {
    std::function<void(const std::wstring&, int)> onCreate;
    HWND hPathEdit = NULL;
    HWND hRadioBlank = NULL;
    HWND hRadioEcom = NULL;
    HWND hRadioDev = NULL;
    HBRUSH hBgBrush = NULL;
};

static LRESULT CALLBACK NewDbDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    NewDbDialogContext* ctx = reinterpret_cast<NewDbDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<NewDbDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const auto& colors = Theme::Get();
        ctx->hBgBrush = CreateSolidBrush(colors.bgSecondary);

        HWND hTitle = CreateWindowExW(0, L"STATIC", L"Create New SQLite Database", WS_CHILD | WS_VISIBLE,
            20, 15, 300, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hLblType = CreateWindowExW(0, L"STATIC", L"Database Template / Preset:", WS_CHILD | WS_VISIBLE,
            20, 45, 300, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hLblType, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        ctx->hRadioBlank = CreateWindowExW(0, L"BUTTON", L"Blank Database (empty schema)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            20, 70, 450, 24, hwnd, (HMENU)5001, NULL, NULL);
        SendMessageW(ctx->hRadioBlank, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        ctx->hRadioEcom = CreateWindowExW(0, L"BUTTON", L"Sample E-Commerce Store (customers, products, orders, order_items)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            20, 95, 520, 24, hwnd, (HMENU)5002, NULL, NULL);
        SendMessageW(ctx->hRadioEcom, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);
        SendMessageW(ctx->hRadioEcom, BM_SETCHECK, BST_CHECKED, 0);

        ctx->hRadioDev = CreateWindowExW(0, L"BUTTON", L"Sample Dev Studio (developers, projects, tasks, commits)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            20, 120, 520, 24, hwnd, (HMENU)5003, NULL, NULL);
        SendMessageW(ctx->hRadioDev, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        HWND hLblPath = CreateWindowExW(0, L"STATIC", L"Database Destination File (.db / .sqlite):", WS_CHILD | WS_VISIBLE,
            20, 160, 400, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hLblPath, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        wchar_t tempPath[MAX_PATH] = { 0 };
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring defaultDb = std::wstring(tempPath) + L"sample_ecommerce.db";

        ctx->hPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultDb.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 185, 430, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hPathEdit, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        HWND hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
            460, 185, 90, 26, hwnd, (HMENU)5004, NULL, NULL);
        SendMessageW(hBtnBrowse, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        HWND hBtnCreate = CreateWindowExW(0, L"BUTTON", L"Create Database", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            310, 230, 140, 30, hwnd, (HMENU)IDOK, NULL, NULL);
        SendMessageW(hBtnCreate, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            460, 230, 90, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wParam;
        const auto& colors = Theme::Get();
        SetTextColor(hdc, colors.textPrimary);
        SetBkColor(hdc, colors.bgSecondary);
        return (INT_PTR)ctx->hBgBrush;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == 5004) { // Browse
            wchar_t szFile[MAX_PATH] = { 0 };
            OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"SQLite Databases (*.db;*.sqlite)\0*.db;*.sqlite\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                SetWindowTextW(ctx->hPathEdit, szFile);
            }
        }
        else if (wmId == IDOK) {
            wchar_t buf[MAX_PATH] = { 0 };
            GetWindowTextW(ctx->hPathEdit, buf, MAX_PATH - 1);
            std::wstring path = buf;
            if (path.empty()) {
                MessageBoxW(hwnd, L"Please enter or select a valid file path.", L"Error", MB_ICONERROR);
                return 0;
            }
            int preset = 0;
            if (SendMessageW(ctx->hRadioEcom, BM_GETCHECK, 0, 0) == BST_CHECKED) preset = 1;
            else if (SendMessageW(ctx->hRadioDev, BM_GETCHECK, 0, 0) == BST_CHECKED) preset = 2;

            if (ctx->onCreate) ctx->onCreate(path, preset);
            DestroyWindow(hwnd);
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowNewDatabaseDialog(HWND hParent, std::function<void(const std::wstring&, int)> onCreate) {
    NewDbDialogContext ctx;
    ctx.onCreate = onCreate;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = NewDbDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DataForge_NewDbDialog";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"New Database Wizard - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 580, 310, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// =========================================================================
// 7. Documentation Dialog
// =========================================================================
struct DocsDialogContext {
    HWND hEdit = NULL;
    HFONT hFont = NULL;
    HBRUSH hBgBrush = NULL;
};

static LRESULT CALLBACK DocsDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    DocsDialogContext* ctx = reinterpret_cast<DocsDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<DocsDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const auto& colors = Theme::Get();
        ctx->hFont = Theme::GetMainFont();
        ctx->hBgBrush = CreateSolidBrush(colors.bgSecondary);

        HWND hTitle = CreateWindowExW(0, L"STATIC", L"DataForge Studio - Help & Quick Reference", WS_CHILD | WS_VISIBLE,
            20, 15, 450, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        std::wstring docs =
            L"DATAFORGE STUDIO NATIVE EDITION\r\n"
            L"=====================================================\r\n\r\n"
            L"1. API CLIENT\r\n"
            L"-----------------------------------------------------\r\n"
            L"• HTTP Methods: GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS.\r\n"
            L"• Variable Interpolation: Use {{variable_name}} in URLs, Headers, and Body.\r\n"
            L"  Variables are resolved in order: Active Environment -> Global Variables.\r\n"
            L"• URL Params: Path parameters like :id or query parameters are supported.\r\n"
            L"• Postman-Style Assertions: Write test assertions in the 'Tests' tab:\r\n"
            L"    pm.test(\"Status is 200\", function() {\r\n"
            L"        pm.response.to.have.status(200);\r\n"
            L"    });\r\n"
            L"    pm.test(\"Latency < 500ms\", function() {\r\n"
            L"        pm.expect(pm.response.responseTime).to.be.below(500);\r\n"
            L"    });\r\n"
            L"• Import: Import collections from OpenAPI 3.0/Swagger JSON or raw cURL commands.\r\n"
            L"• Code Generation: Export any request to cURL, Python (requests), JS (fetch), or JS (axios).\r\n\r\n"
            L"2. DATABASE STUDIO (SQLITE)\r\n"
            L"-----------------------------------------------------\r\n"
            L"• Multi-database management with live schema tree and table metrics.\r\n"
            L"• Multi-statement SQL editor with syntax coloring, results grid, and EXPLAIN query plan.\r\n"
            L"• Paginated data grid with live filter search, column sorting, and inline row editing.\r\n"
            L"• Schema Inspector: Table columns, primary keys, foreign keys, and indexes.\r\n"
            L"• Visual ER Diagram: Automatic node graph layout with relational edges.\r\n"
            L"• Native Export: Export query results or tables to CSV, JSON, or SQL INSERT dumps.\r\n\r\n"
            L"3. SHORTCUTS & HYGIENE\r\n"
            L"-----------------------------------------------------\r\n"
            L"• Ctrl+Enter: Send HTTP Request or Execute SQL Query.\r\n"
            L"• Drag & Drop: Drag .db, .sqlite, .json, or .sql files onto the window to open.\r\n"
            L"• Security: Sensitive environment variables marked as 'Secret' are masked in the UI.\r\n"
            L"• Completely native C++17 runtime with zero external server dependencies.";

        ctx->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", docs.c_str(),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            20, 45, 660, 360, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hEdit, WM_SETFONT, (WPARAM)Theme::GetCodeFont(), TRUE);

        HWND hBtnClose = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            580, 415, 100, 30, hwnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessageW(hBtnClose, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG: {
        HDC hdc = (HDC)wParam;
        const auto& colors = Theme::Get();
        SetTextColor(hdc, colors.textPrimary);
        SetBkColor(hdc, colors.bgSecondary);
        return (INT_PTR)ctx->hBgBrush;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDCANCEL || wmId == IDOK) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DESTROY: {
        if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowDocsDialog(HWND hParent) {
    DocsDialogContext ctx;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = DocsDlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DataForge_DocsDialog";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Documentation & Shortcuts - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 715, 495, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

} // namespace native_app
