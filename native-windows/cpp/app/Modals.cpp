#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include "Modals.h"
#include "Theme.h"
#include "CurlParser.h"
#include "OpenApiParser.h"
#include "CodeGen.h"
#include "CodeEditor.h"
#include "DbEngine.h"
#include "NativeMockServer.h"
#include "NativeDialogs.h"
#include <atomic>
#include <thread>
#include <memory>
#include <cmath>
#include <cctype>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <exception>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

namespace native_app {
static std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window); std::wstring text(length + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1); text.resize(length); return text;
}

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

static RECT MonitorWorkArea(HWND window) {
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return monitor.rcWork;
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0); return work;
}
static void CenterWindow(HWND window, HWND parent) {
    RECT owner{}, bounds{}; GetWindowRect(window, &bounds);
    const RECT work = MonitorWorkArea(parent ? parent : window);
    if (!parent || !GetWindowRect(parent, &owner)) owner = work;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const int x = std::max<int>(work.left, std::min<int>(work.right - width, owner.left + (owner.right - owner.left - width) / 2));
    const int y = std::max<int>(work.top, std::min<int>(work.bottom - height, owner.top + (owner.bottom - owner.top - height) / 2));
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}
struct ModalChildPlacement {
    HWND window = nullptr;
    RECT bounds{};
    int layoutHeight = 0;
};
struct ModalViewport {
    HWND window = nullptr;
    std::vector<ModalChildPlacement> children;
    int contentWidth = 0, contentHeight = 0, offsetX = 0, offsetY = 0;
    bool updating = false;
};
static void UpdateModalViewport(ModalViewport& viewport) {
    if (viewport.updating || !IsWindow(viewport.window)) return;
    viewport.updating = true;
    RECT client{};
    for (int pass = 0; pass < 3; ++pass) {
        GetClientRect(viewport.window, &client);
        ShowScrollBar(viewport.window, SB_HORZ, viewport.contentWidth > client.right);
        ShowScrollBar(viewport.window, SB_VERT, viewport.contentHeight > client.bottom);
    }
    GetClientRect(viewport.window, &client);
    viewport.offsetX = std::max<int>(0, std::min<int>(viewport.offsetX, std::max<int>(0, viewport.contentWidth - client.right)));
    viewport.offsetY = std::max<int>(0, std::min<int>(viewport.offsetY, std::max<int>(0, viewport.contentHeight - client.bottom)));
    SCROLLINFO scroll{}; scroll.cbSize = sizeof(scroll); scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMax = std::max<int>(0, viewport.contentWidth - 1); scroll.nPage = client.right; scroll.nPos = viewport.offsetX;
    SetScrollInfo(viewport.window, SB_HORZ, &scroll, TRUE);
    scroll.nMax = std::max<int>(0, viewport.contentHeight - 1); scroll.nPage = client.bottom; scroll.nPos = viewport.offsetY;
    SetScrollInfo(viewport.window, SB_VERT, &scroll, TRUE);
    for (const auto& child : viewport.children) if (IsWindow(child.window)) {
        SetWindowPos(child.window, nullptr, child.bounds.left - viewport.offsetX, child.bounds.top - viewport.offsetY,
            child.bounds.right - child.bounds.left, child.layoutHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    viewport.updating = false;
    InvalidateRect(viewport.window, nullptr, FALSE);
}
static void RevealModalFocus(ModalViewport& viewport, HWND focused) {
    if (!focused || !IsChild(viewport.window, focused)) return;
    RECT bounds{}, client{}; GetWindowRect(focused, &bounds); GetClientRect(viewport.window, &client);
    MapWindowPoints(nullptr, viewport.window, reinterpret_cast<POINT*>(&bounds), 2);
    const int margin = Theme::Scale(8);
    int x = viewport.offsetX, y = viewport.offsetY;
    if (bounds.left < margin) x += bounds.left - margin;
    else if (bounds.right > client.right - margin) x += bounds.right - client.right + margin;
    if (bounds.top < margin || bounds.bottom - bounds.top > client.bottom - margin * 2) y += bounds.top - margin;
    else if (bounds.bottom > client.bottom - margin) y += bounds.bottom - client.bottom + margin;
    if (x != viewport.offsetX || y != viewport.offsetY) {
        viewport.offsetX = x; viewport.offsetY = y; UpdateModalViewport(viewport);
    }
}
static LRESULT CALLBACK ModalThemeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR reference) {
    auto* viewport = reinterpret_cast<ModalViewport*>(reference);
    if (message == WM_ERASEBKGND) {
        RECT rectangle{}; GetClientRect(window, &rectangle); FillRect(reinterpret_cast<HDC>(wParam), &rectangle, Theme::GetBgSecondaryBrush()); return 1;
    }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
        HDC dc = reinterpret_cast<HDC>(wParam); SetTextColor(dc, Theme::Get().textPrimary); SetBkColor(dc, Theme::Get().bgSecondary);
        return reinterpret_cast<LRESULT>(Theme::GetBgSecondaryBrush());
    }
    if (viewport && message == WM_SIZE) UpdateModalViewport(*viewport);
    if (viewport && (message == WM_HSCROLL || message == WM_VSCROLL) && !lParam) {
        const int bar = message == WM_HSCROLL ? SB_HORZ : SB_VERT;
        SCROLLINFO scroll{}; scroll.cbSize = sizeof(scroll); scroll.fMask = SIF_ALL; GetScrollInfo(window, bar, &scroll);
        int position = scroll.nPos;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: position -= Theme::Scale(24); break;
        case SB_LINEDOWN: position += Theme::Scale(24); break;
        case SB_PAGEUP: position -= scroll.nPage; break;
        case SB_PAGEDOWN: position += scroll.nPage; break;
        case SB_THUMBTRACK: position = scroll.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = scroll.nMax; break;
        default: return 0;
        }
        if (bar == SB_HORZ) viewport->offsetX = position; else viewport->offsetY = position;
        UpdateModalViewport(*viewport); return 0;
    }
    if (viewport && message == WM_MOUSEWHEEL) {
        const int change = static_cast<short>(HIWORD(wParam)) * Theme::Scale(72) / WHEEL_DELTA;
        if (LOWORD(wParam) & MK_SHIFT) viewport->offsetX -= change; else viewport->offsetY -= change;
        UpdateModalViewport(*viewport); return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ModalThemeProc, 87);
    return DefSubclassProc(window, message, wParam, lParam);
}
static void PrepareModalWindow(HWND window, HWND parent, ModalViewport& viewport, const RECT* constrainedArea = nullptr) {
    viewport.window = window;
    RECT bounds{}, client{}; GetWindowRect(window, &bounds); GetClientRect(window, &client);
    viewport.contentWidth = Theme::Scale(client.right); viewport.contentHeight = Theme::Scale(client.bottom);
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT original{}; GetWindowRect(child, &original); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&original), 2);
        RECT scaled{ Theme::Scale(original.left), Theme::Scale(original.top), Theme::Scale(original.right), Theme::Scale(original.bottom) };
        int layoutHeight = scaled.bottom - scaled.top;
        wchar_t name[64]{}; GetClassNameW(child, name, 64);
        if (lstrcmpiW(name, L"COMBOBOX") == 0) {
            RECT expanded{};
            if (SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&expanded))) layoutHeight = Theme::Scale(expanded.bottom - expanded.top);
        }
        viewport.children.push_back({child, scaled, layoutHeight});
        viewport.contentWidth = std::max<int>(viewport.contentWidth, scaled.right + Theme::Scale(14));
        viewport.contentHeight = std::max<int>(viewport.contentHeight, scaled.bottom + Theme::Scale(14));
        SetWindowPos(child, nullptr, scaled.left, scaled.top, scaled.right - scaled.left, layoutHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        if (lstrcmpiW(name, L"STATIC") != 0) SetWindowLongPtrW(child, GWL_STYLE, GetWindowLongPtrW(child, GWL_STYLE) | WS_TABSTOP);
        if (lstrcmpiW(name, WC_LISTVIEWW) == 0) for (int column = 0; column < Header_GetItemCount(ListView_GetHeader(child)); ++column)
            ListView_SetColumnWidth(child, column, Theme::Scale(ListView_GetColumnWidth(child, column)));
    }
    const RECT work = constrainedArea ? *constrainedArea : MonitorWorkArea(parent ? parent : window);
    const int margin = Theme::Scale(8);
    const int width = std::max<int>(1, std::min<int>(Theme::Scale(bounds.right - bounds.left), static_cast<int>(work.right - work.left) - margin * 2));
    const int height = std::max<int>(1, std::min<int>(Theme::Scale(bounds.bottom - bounds.top), static_cast<int>(work.bottom - work.top) - margin * 2));
    SetWindowPos(window, nullptr, work.left + margin, work.top + margin, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowSubclass(window, ModalThemeProc, 87, reinterpret_cast<DWORD_PTR>(&viewport));
    Theme::ApplyToWindow(window); UpdateModalViewport(viewport);
    if (!constrainedArea) CenterWindow(window, parent);
}
static void RunModalLoop(HWND window, HWND parent) {
    if (!window) return;
    ModalViewport viewport; PrepareModalWindow(window, parent, viewport);
    const bool enableParent = parent && IsWindowEnabled(parent);
    if (enableParent) EnableWindow(parent, FALSE);
    ShowWindow(window, SW_SHOW); UpdateWindow(window);
    if (HWND first = GetNextDlgTabItem(window, GetWindow(window, GW_CHILD), FALSE)) { SetFocus(first); RevealModalFocus(viewport, first); }
    const HWND rootOwner = parent ? GetAncestor(parent, GA_ROOTOWNER) : nullptr;
    bool closeOwner = false;
    MSG message{};
    while (IsWindow(window)) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) { if (!received) PostQuitMessage(static_cast<int>(message.wParam)); DestroyWindow(window); break; }
        if (rootOwner && message.hwnd == rootOwner && message.message == WM_CLOSE) { closeOwner = true; SendMessageW(window, WM_CLOSE, 0, 0); continue; }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (IsWindow(window)) RevealModalFocus(viewport, GetFocus());
    }
    if (enableParent && IsWindow(parent)) { EnableWindow(parent, TRUE); SetActiveWindow(parent); }
    if (closeOwner && IsWindow(rootOwner)) PostMessageW(rootOwner, WM_CLOSE, 0, 0);
}
static bool SetClipboardText(HWND window, const std::wstring& text) {
    if (!OpenClipboard(window)) return false;
    bool copied = false;
    if (!EmptyClipboard()) { CloseClipboard(); return false; }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* destination = GlobalLock(memory)) {
            memcpy(destination, text.c_str(), bytes); GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory)) copied = true; else GlobalFree(memory);
        } else GlobalFree(memory);
    }
    CloseClipboard(); return copied;
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
    HWND hEnabledCheck = NULL;
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
        for (int index = 0; index < static_cast<int>(ctx->envStore.environments.size()); ++index)
            if (ctx->envStore.environments[index].id == ctx->envStore.activeId) ctx->selectedEnvIndex = index;

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
        col.cx = 70; col.pszText = (LPWSTR)L"Enabled";
        ListView_InsertColumn(ctx->hVarList, 2, &col);

        // Variable editor inputs
        ctx->hKeyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            220, 345, 140, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hKeyEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hValEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            370, 345, 175, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hValEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hEnabledCheck = CreateWindowExW(0, L"BUTTON", L"Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            550, 345, 80, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hEnabledCheck, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);
        SendMessageW(ctx->hEnabledCheck, BM_SETCHECK, BST_CHECKED, 0);

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
        SendMessageW(ctx->hEnvList, LB_SETCURSEL, ctx->selectedEnvIndex, 0);

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
                ListView_SetItemText(ctx->hVarList, row, 2, const_cast<LPWSTR>(v.enabled ? L"Yes" : L"No"));
                row++;
            }
        };
        loadEnv(ctx->selectedEnvIndex);
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
        try {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);

        if (wmId == 1004 && wmEvent == EN_CHANGE && ctx->selectedEnvIndex >= 0 && ctx->selectedEnvIndex < static_cast<int>(ctx->envStore.environments.size())) {
            const std::wstring name = WindowText(ctx->hEnvNameEdit);
            if (!name.empty()) {
                ctx->envStore.environments[ctx->selectedEnvIndex].name = WideToUtf8(name);
                SendMessageW(ctx->hEnvList, LB_DELETESTRING, ctx->selectedEnvIndex, 0);
                SendMessageW(ctx->hEnvList, LB_INSERTSTRING, ctx->selectedEnvIndex, reinterpret_cast<LPARAM>(name.c_str()));
                SendMessageW(ctx->hEnvList, LB_SETCURSEL, ctx->selectedEnvIndex, 0);
            }
        } else if (wmId == 1001 && wmEvent == LBN_SELCHANGE) {
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
                    ListView_SetItemText(ctx->hVarList, row, 2, const_cast<LPWSTR>(v.enabled ? L"Yes" : L"No"));
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


            std::string key = WideToUtf8(WindowText(ctx->hKeyEdit));
            std::string val = WideToUtf8(WindowText(ctx->hValEdit));
            if (key.empty()) return 0;

            auto& env = ctx->envStore.environments[ctx->selectedEnvIndex];
            bool found = false;
            for (auto& v : env.variables) {
                if (v.key == key) {
                    v.value = val;
                    v.enabled = SendMessageW(ctx->hEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Variable v;
                v.key = key;
                v.value = val;
                    v.enabled = SendMessageW(ctx->hEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

                env.variables.push_back(v);
            }
            // Refresh list
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1001, LBN_SELCHANGE), 0);
            SetWindowTextW(ctx->hKeyEdit, L"");
            SetWindowTextW(ctx->hValEdit, L"");
            SendMessageW(ctx->hEnabledCheck, BM_SETCHECK, BST_CHECKED, 0);
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
                std::string name = WideToUtf8(WindowText(ctx->hEnvNameEdit));
                if (!name.empty()) ctx->envStore.environments[ctx->selectedEnvIndex].name = name;
            }
            if (ctx->selectedEnvIndex >= 0 && ctx->selectedEnvIndex < static_cast<int>(ctx->envStore.environments.size()))
                ctx->envStore.activeId = ctx->envStore.environments[ctx->selectedEnvIndex].id;
            ctx->store->SaveEnvironments(ctx->envStore);
            if (ctx->onUpdate) ctx->onUpdate();
            DestroyWindow(hwnd);
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, Utf8ToWide(error.what()).c_str(), L"Operation failed", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxW(hwnd, L"The operation failed unexpectedly. Your dialog is still open.", L"Operation failed", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == 1005 && header->code == LVN_ITEMCHANGED && ctx->selectedEnvIndex >= 0 && ctx->selectedEnvIndex < static_cast<int>(ctx->envStore.environments.size())) {
            const auto* selected = reinterpret_cast<NMLISTVIEW*>(lParam); const auto& variables = ctx->envStore.environments[ctx->selectedEnvIndex].variables;
            if ((selected->uNewState & LVIS_SELECTED) && selected->iItem >= 0 && selected->iItem < static_cast<int>(variables.size())) {
                const auto& variable = variables[selected->iItem]; SetWindowTextW(ctx->hKeyEdit, Utf8ToWide(variable.key).c_str());
                SetWindowTextW(ctx->hValEdit, Utf8ToWide(variable.value).c_str());
                SendMessageW(ctx->hEnabledCheck, BM_SETCHECK, variable.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            }
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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Manage Environments - DataForge Studio",
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
    HWND hEnabledCheck = NULL;
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
        col.cx = 80; col.pszText = (LPWSTR)L"Enabled";
        ListView_InsertColumn(ctx->hVarList, 2, &col);

        // Edit fields
        ctx->hKeyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 305, 170, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hKeyEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hValEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            200, 305, 200, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hValEdit, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);

        ctx->hEnabledCheck = CreateWindowExW(0, L"BUTTON", L"Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            410, 305, 80, 26, hwnd, NULL, NULL, NULL);
        SendMessageW(ctx->hEnabledCheck, WM_SETFONT, (WPARAM)ctx->hFont, TRUE);
        SendMessageW(ctx->hEnabledCheck, BM_SETCHECK, BST_CHECKED, 0);

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
                ListView_SetItemText(ctx->hVarList, row, 2, const_cast<LPWSTR>(v.enabled ? L"Yes" : L"No"));
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
        try {
        int wmId = LOWORD(wParam);
        if (wmId == 2002) { // Add/Set

            std::string key = WideToUtf8(WindowText(ctx->hKeyEdit));
            std::string val = WideToUtf8(WindowText(ctx->hValEdit));
            if (key.empty()) return 0;

            bool found = false;
            for (auto& v : ctx->vars) {
                if (v.key == key) {
                    v.value = val;
                    v.enabled = SendMessageW(ctx->hEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    found = true;
                    break;
                }
            }
            if (!found) {
                Variable v;
                v.key = key;
                v.value = val;
                    v.enabled = SendMessageW(ctx->hEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

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
                ListView_SetItemText(ctx->hVarList, row, 2, const_cast<LPWSTR>(v.enabled ? L"Yes" : L"No"));
                row++;
            }
            SetWindowTextW(ctx->hKeyEdit, L"");
            SetWindowTextW(ctx->hValEdit, L"");
            SendMessageW(ctx->hEnabledCheck, BM_SETCHECK, BST_CHECKED, 0);
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
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, Utf8ToWide(error.what()).c_str(), L"Operation failed", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxW(hwnd, L"The operation failed unexpectedly. Your dialog is still open.", L"Operation failed", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == 2001 && header->code == LVN_ITEMCHANGED) {
            const auto* selected = reinterpret_cast<NMLISTVIEW*>(lParam); const auto& variables = ctx->vars;
            if ((selected->uNewState & LVIS_SELECTED) && selected->iItem >= 0 && selected->iItem < static_cast<int>(variables.size())) {
                const auto& variable = variables[selected->iItem]; SetWindowTextW(ctx->hKeyEdit, Utf8ToWide(variable.key).c_str());
                SetWindowTextW(ctx->hValEdit, Utf8ToWide(variable.value).c_str());
                SendMessageW(ctx->hEnabledCheck, BM_SETCHECK, variable.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            }
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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Global Variables - DataForge Studio",
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

        ctx->hEdit = CodeEditor::Create(hwnd, 6101, EditorLanguage::Shell);
        MoveWindow(ctx->hEdit, 20, 45, 560, 220, TRUE);
        CodeEditor::SetText(ctx->hEdit, L"curl -X GET https://api.example.com/v1/items");
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
        try {
        int wmId = LOWORD(wParam);
        if (wmId == IDOK) {
            int len = GetWindowTextLengthW(ctx->hEdit);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(ctx->hEdit, buf.data(), len + 1);
                std::string curlCmd = WideToUtf8(buf.data());

                auto request = CurlParser::Parse(curlCmd);
                if (!request.url.empty()) {
                    if (ctx->onImport) ctx->onImport(request);
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
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, Utf8ToWide(error.what()).c_str(), L"Operation failed", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxW(hwnd, L"The operation failed unexpectedly. Your dialog is still open.", L"Operation failed", MB_OK | MB_ICONERROR);
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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Import from cURL - DataForge Studio",
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

        HWND hTitle = CreateWindowExW(0, L"STATIC", L"Paste OpenAPI 3.x / Swagger 2.0 JSON or YAML, or load a file:", WS_CHILD | WS_VISIBLE,
            20, 15, 540, 20, hwnd, NULL, NULL, NULL);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)Theme::GetBoldFont(), TRUE);

        HWND hBtnFile = CreateWindowExW(0, L"BUTTON", L"Browse File...", WS_CHILD | WS_VISIBLE,
            460, 10, 120, 26, hwnd, (HMENU)3001, NULL, NULL);
        SendMessageW(hBtnFile, WM_SETFONT, (WPARAM)Theme::GetMainFont(), TRUE);

        ctx->hEdit = CodeEditor::Create(hwnd, 6102, EditorLanguage::Json);
        MoveWindow(ctx->hEdit, 20, 45, 560, 240, TRUE);
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
        try {
        int wmId = LOWORD(wParam);
        if (wmId == 3001) { // Browse file
            wchar_t szFile[32768] = { 0 };
            OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"OpenAPI specifications (*.json;*.yaml;*.yml)\0*.json;*.yaml;*.yml\0All Files (*.*)\0*.*\0\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = static_cast<DWORD>(std::size(szFile));
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                HANDLE hF = CreateFileW(szFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                if (hF != INVALID_HANDLE_VALUE) {
                    DWORD dwSize = GetFileSize(hF, NULL);
                    if (dwSize > 0 && dwSize < 10 * 1024 * 1024) {
                        std::vector<char> buf(dwSize);
                        DWORD dwRead = 0;
                        if (!ReadFile(hF, buf.data(), dwSize, &dwRead, NULL) || dwRead != dwSize) {
                            CloseHandle(hF); MessageBoxW(hwnd, L"The file could not be read completely. Check permissions and try again.", L"Import failed", MB_OK | MB_ICONERROR); return 0;
                        }
                        std::string jsonStr(buf.data(), dwRead);
                        CodeEditor::SetLanguage(ctx->hEdit, jsonStr.find_first_not_of(" \t\r\n")!=std::string::npos && jsonStr[jsonStr.find_first_not_of(" \t\r\n")]=='{' ? EditorLanguage::Json : EditorLanguage::PlainText);
                        CodeEditor::SetText(ctx->hEdit, Utf8ToWide(jsonStr));
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

                auto collection = OpenApiParser::Parse(spec);
                if (!collection.requests.empty() || !collection.folders.empty()) {
                    auto existingCols = ctx->store->GetCollections();
                    existingCols.push_back(collection);
                    ctx->store->SaveCollections(existingCols);
                    if (ctx->onImport) ctx->onImport();
                    MessageBoxW(hwnd, L"OpenAPI collection imported successfully.",
                        L"Import Success", MB_OK | MB_ICONINFORMATION);
                    DestroyWindow(hwnd);
                } else {
                    MessageBoxW(hwnd, L"Could not parse OpenAPI specification. Please ensure valid OpenAPI 3.x or Swagger 2.0 JSON or YAML.",
                        L"Import Error", MB_ICONERROR);
                }
            }
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, Utf8ToWide(error.what()).c_str(), L"Operation failed", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxW(hwnd, L"The operation failed unexpectedly. Your dialog is still open.", L"Operation failed", MB_OK | MB_ICONERROR);
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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Import OpenAPI / Swagger - DataForge Studio",
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
    CodeEditor::SetLanguage(ctx->hCodeEdit, lang == "curl" ? EditorLanguage::Shell : EditorLanguage::PlainText);
    CodeEditor::SetText(ctx->hCodeEdit, Utf8ToWide(code));
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
        ctx->hCodeEdit = CodeEditor::Create(hwnd, 6103, EditorLanguage::Shell, true);
        MoveWindow(ctx->hCodeEdit, 20, 80, 640, 300, TRUE);
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
        try {
        int wmId = LOWORD(wParam);
        if (wmId >= 4001 && wmId <= 4004) {
            UpdateGeneratedCode(ctx);
        }
        else if (wmId == 4005) { // Copy
            int len = GetWindowTextLengthW(ctx->hCodeEdit);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(ctx->hCodeEdit, buf.data(), len + 1);
                if (SetClipboardText(hwnd, buf.data())) MessageBoxW(hwnd, L"Code copied to clipboard.", L"Copied", MB_OK | MB_ICONINFORMATION);
                else MessageBoxW(hwnd, L"The clipboard is unavailable. Try again after closing the application using it.", L"Copy failed", MB_OK | MB_ICONERROR);
            }
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, Utf8ToWide(error.what()).c_str(), L"Operation failed", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxW(hwnd, L"The operation failed unexpectedly. Your dialog is still open.", L"Operation failed", MB_OK | MB_ICONERROR);
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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Code Snippet Generator - DataForge Studio",
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

        wchar_t documents[MAX_PATH]{};
        std::wstring defaultDb;
        if (SUCCEEDED(SHGetFolderPathW(hwnd, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents)))
            defaultDb = (std::filesystem::path(documents) / L"DataForge-database.db").wstring();

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
        try {
        int wmId = LOWORD(wParam);
        if (wmId == 5004) { // Browse
            wchar_t szFile[32768] = { 0 };
            OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"SQLite Databases (*.db;*.sqlite)\0*.db;*.sqlite\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = static_cast<DWORD>(std::size(szFile));
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) {
                SetWindowTextW(ctx->hPathEdit, szFile);
            }
        }
        else if (wmId == IDOK) {

            std::wstring path = WindowText(ctx->hPathEdit);
            if (path.empty()) {
                MessageBoxW(hwnd, L"Please enter or select a valid file path.", L"Error", MB_ICONERROR);
                return 0;
            }
            if (std::filesystem::exists(path)) { MessageBoxW(hwnd, L"Choose a new destination. To attach an existing SQLite database, close this dialog and drop the file onto the main window.", L"File already exists", MB_OK | MB_ICONWARNING); return 0; }
            int preset = 0;
            if (SendMessageW(ctx->hRadioEcom, BM_GETCHECK, 0, 0) == BST_CHECKED) preset = 1;
            else if (SendMessageW(ctx->hRadioDev, BM_GETCHECK, 0, 0) == BST_CHECKED) preset = 2;

            if (ctx->onCreate) ctx->onCreate(path, preset);
            DestroyWindow(hwnd);
        }
        else if (wmId == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, Utf8ToWide(error.what()).c_str(), L"Operation failed", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxW(hwnd, L"The operation failed unexpectedly. Your dialog is still open.", L"Operation failed", MB_OK | MB_ICONERROR);
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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, L"New Database Wizard - DataForge Studio",
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
            L"DATAFORGE STUDIO - NATIVE WINDOWS EDITION\r\n\r\n"
            L"API CLIENT\r\n"
            L"Independent requests, environments, history, assertions and collection running.\r\n"
            L"Pre-request and post-response scripts use a bounded native JavaScript engine.\r\n"
            L"Import cURL, native/Postman collections, or OpenAPI/Swagger JSON and YAML.\r\n"
            L"Generated snippets are text exports.\r\n\r\n"
            L"DATABASE STUDIO\r\n"
            L"SQL syntax colors, schema-aware completion, selected-query execution and EXPLAIN.\r\n"
            L"Independent query tabs, protected workspace/history and multiple result sets.\r\n"
            L"Queries run in owned background workers with cancellation.\r\n"
            L"Profiles support SQLite, DuckDB, MongoDB, Redis and installed ODBC drivers.\r\n"
            L"Remote engines require their database service and, for ODBC, a native driver.\r\n"
            L"Search and sort paginated data. Add, edit or delete rows using native dialogs.\r\n"
            L"Untouched insert fields use defaults; updates save only changed fields.\r\n"
            L"The row editor preserves NULL, text, numbers, Boolean, binary and JSON values.\r\n"
            L"Table and result exports preserve available database types.\r\n"
            L"Schema columns, defaults, keys, indexes and DDL; draggable ER cards and saved layout.\r\n"
            L"Connection tools can start an optional loopback mock REST API over SQL connections.\r\n"
            L"SQLite, DuckDB and native ODBC providers use their row CRUD permissions.\r\n\r\n"
            L"KEYBOARD\r\n"
            L"Ctrl+Enter: execute the HTTP request or selected SQL/editor contents.\r\n"
            L"Ctrl+Space: completion; arrows/Enter: choose; Esc: close suggestions.\r\n"
            L"Tab/Shift+Tab: move between controls. Ctrl+1/Ctrl+2: switch workbenches.\r\n"
            L"Ctrl+T/Ctrl+W: open/close a document; Ctrl+Tab/Shift+Ctrl+Tab: switch documents.\r\n"
            L"Ctrl++/Ctrl+-: resize the interface; Ctrl+0: reset its size.\r\n\r\n"
            L"The application runs natively without Python, React, Node or a browser.\r\n"
            L"Enabled variable toggles control interpolation. Values remain visible when edited.\r\n";
        ctx->hEdit = CodeEditor::Create(hwnd, 6104, EditorLanguage::PlainText, true);
        MoveWindow(ctx->hEdit, 20, 45, 660, 360, TRUE);
        CodeEditor::SetText(ctx->hEdit, docs);
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
        try {
        int wmId = LOWORD(wParam);
        if (wmId == IDCANCEL || wmId == IDOK) {
            DestroyWindow(hwnd);
        }
        } catch (const std::exception& error) {
            MessageBoxW(hwnd, Utf8ToWide(error.what()).c_str(), L"Operation failed", MB_OK | MB_ICONERROR);
        } catch (...) {
            MessageBoxW(hwnd, L"The operation failed unexpectedly. Your dialog is still open.", L"Operation failed", MB_OK | MB_ICONERROR);
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

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Documentation & Shortcuts - DataForge Studio",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        0, 0, 715, 495, hParent, NULL, wc.hInstance, &ctx);

    CenterWindow(hwnd, hParent);
    RunModalLoop(hwnd, hParent);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

struct ConnectionWork {
    std::atomic_bool canceled{false},done{false};
    DatabaseInfo database;std::string message;bool success=false,saving=false,connect=false;
};
struct ConnectionDialogContext {
    DbEngine* engine=nullptr;ConnectionProfile profile;std::vector<DatabaseDriverInfo> drivers;
    std::function<void(const DatabaseInfo&,bool)> onSaved;
    std::map<std::string,HWND> fields;std::vector<std::pair<HWND,int>> pages;
    HWND driver=nullptr,status=nullptr,hint=nullptr;
    HWND readOnly=nullptr,create=nullptr,persist=nullptr,session=nullptr;
    HWND test=nullptr,save=nullptr,connect=nullptr,cancel=nullptr;
    int page=0;bool closing=false;std::shared_ptr<ConnectionWork> work;std::thread worker;
    ~ConnectionDialogContext(){if(work)work->canceled.store(true);if(worker.joinable())worker.join();}
};
static void SelectConnectionPage(ConnectionDialogContext* context,int page) {
    context->page=page;
    for(const auto& child:context->pages)ShowWindow(child.first,child.second==page?SW_SHOW:SW_HIDE);
    for(int index=0;index<3;++index){HWND button=GetDlgItem(GetParent(context->driver),7280+index);RemovePropW(button,L"DataForge.Active");if(index==page)SetPropW(button,L"DataForge.Active",reinterpret_cast<HANDLE>(1));InvalidateRect(button,nullptr,FALSE);}
}
static void EnableConnectionFields(ConnectionDialogContext* context,bool available) {
    for(const auto& field:context->fields)EnableWindow(field.second,available);EnableWindow(context->driver,available);
    for(HWND control:{context->readOnly,context->create,context->persist,context->session,context->save,context->connect})EnableWindow(control,available);
    SetWindowTextW(context->test,available?L"Test connection":L"Cancel operation");
}
static ConnectionProfile ReadConnectionFields(ConnectionDialogContext* context) {
    ConnectionProfile profile=context->profile;
    const int driver=static_cast<int>(SendMessageW(context->driver,CB_GETCURSEL,0,0));if(driver<0||driver>=static_cast<int>(context->drivers.size()))throw std::runtime_error("Select a database engine.");profile.type=context->drivers[driver].id;
    const auto text=[&](const char* field){return WideToUtf8(WindowText(context->fields.at(field)));};
    profile.name=text("name");if(profile.name.empty())throw std::runtime_error("Enter a connection name.");
    profile.host=text("host");profile.database=text("database");profile.schema=text("schema");profile.path=text("path");profile.username=text("username");profile.password=text("password");profile.url=text("url");
    profile.passwordEnv=text("passwordEnv");profile.urlEnv=text("urlEnv");profile.odbcEnv=text("odbcEnv");profile.odbcConnectionString=text("odbcConnectionString");profile.driver=text("driver");profile.sslMode=text("sslMode");profile.sslCa=text("sslCa");
    const auto integer=[&](const char* field,int minimum,int maximum){const auto value=text(field);if(value.empty())return minimum;size_t parsed=0;int result=0;try{result=std::stoi(value,&parsed);}catch(...){throw std::runtime_error(std::string("Enter a valid number for ")+field+".");}if(parsed!=value.size()||result<minimum||result>maximum)throw std::runtime_error(std::string(field)+" is outside its supported range.");return result;};
    profile.port=integer("port",0,65535);profile.timeoutSec=integer("timeoutSec",1,60);
    profile.optionsJson=text("optionsJson");if(profile.optionsJson.empty())profile.optionsJson="{}";const auto options=nlohmann::json::parse(profile.optionsJson);if(!options.is_object())throw std::runtime_error("Driver options must be a JSON object.");
    profile.readOnly=SendMessageW(context->readOnly,BM_GETCHECK,0,0)==BST_CHECKED;profile.create=SendMessageW(context->create,BM_GETCHECK,0,0)==BST_CHECKED;
    profile.sessionCredentials=SendMessageW(context->session,BM_GETCHECK,0,0)==BST_CHECKED;profile.persistCredentials=!profile.sessionCredentials&&SendMessageW(context->persist,BM_GETCHECK,0,0)==BST_CHECKED;
    return profile;
}
static LRESULT CALLBACK ConnectionDialogProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* context=reinterpret_cast<ConnectionDialogContext*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_CREATE){context=static_cast<ConnectionDialogContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));
        int nextId=7202;
        const auto field=[&](const char* key,const wchar_t* label,const std::string& value,int x,int y,int page,DWORD style=0){
            HWND title=CreateWindowExW(0,L"STATIC",label,WS_CHILD|WS_VISIBLE,x,y,330,25,window,nullptr,nullptr,nullptr);
            const bool driverField=std::string(key)=="driver";
            HWND edit=CreateWindowExW(0,driverField?L"COMBOBOX":L"EDIT",Utf8ToWide(value).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|(driverField?CBS_DROPDOWN|WS_VSCROLL:ES_AUTOHSCROLL|style),x,y+28,330,driverField?250:30,window,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(nextId++)),nullptr,nullptr);
            SendMessageW(edit,EM_SETLIMITTEXT,32768,0);context->fields[key]=edit;if(page>=0){context->pages.push_back({title,page});context->pages.push_back({edit,page});}return edit;};
        field("name",L"Connection name",context->profile.name,20,20,-1);
        CreateWindowExW(0,L"STATIC",L"Database engine",WS_CHILD|WS_VISIBLE,370,20,330,25,window,nullptr,nullptr,nullptr);
        context->driver=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,370,48,330,250,window,reinterpret_cast<HMENU>(7201),nullptr,nullptr);
        int selected=-1;for(size_t index=0;index<context->drivers.size();++index){const auto& driver=context->drivers[index];const auto label=Utf8ToWide(driver.name+(driver.available?"":" (driver required)"));SendMessageW(context->driver,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));if(driver.id==context->profile.type)selected=static_cast<int>(index);}SendMessageW(context->driver,CB_SETCURSEL,selected,0);
        const wchar_t* pages[]={L"Connection",L"Security",L"Advanced"};for(int page=0;page<3;++page)CreateWindowExW(0,L"BUTTON",pages[page],WS_CHILD|WS_VISIBLE|WS_TABSTOP,20+page*230,96,220,34,window,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(7280+page)),nullptr,nullptr);
        field("host",L"Host",context->profile.host,20,154,0);field("port",L"Port (0 uses engine default)",context->profile.port?std::to_string(context->profile.port):"",370,154,0,ES_NUMBER);
        field("database",L"Database / project / catalog",context->profile.database,20,226,0);field("schema",L"Schema / dataset",context->profile.schema,370,226,0);
        field("username",L"Username",context->profile.username,20,298,0);field("password",L"Password",context->profile.password,370,298,0,ES_PASSWORD);
        field("path",L"Database file path (file engines)",context->profile.path,20,370,0);field("url",L"Connection URL (when supported)",context->profile.url,370,370,0);
        field("passwordEnv",L"Password environment variable",context->profile.passwordEnv,20,154,1);field("urlEnv",L"URL environment variable",context->profile.urlEnv,370,154,1);
        field("sslMode",L"TLS mode (for example verify-full)",context->profile.sslMode,20,226,1);field("sslCa",L"TLS CA certificate path",context->profile.sslCa,370,226,1);
        context->readOnly=CreateWindowExW(0,L"BUTTON",L"Read-only connection",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,20,314,330,32,window,nullptr,nullptr,nullptr);
        context->persist=CreateWindowExW(0,L"BUTTON",L"Store credentials for this Windows account",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,20,358,680,32,window,nullptr,nullptr,nullptr);
        context->session=CreateWindowExW(0,L"BUTTON",L"Keep credentials for this session only",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,20,402,680,32,window,nullptr,nullptr,nullptr);
        for(HWND control:{context->readOnly,context->persist,context->session})context->pages.push_back({control,1});
        SendMessageW(context->readOnly,BM_SETCHECK,context->profile.readOnly?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(context->persist,BM_SETCHECK,context->profile.persistCredentials?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(context->session,BM_SETCHECK,context->profile.sessionCredentials?BST_CHECKED:BST_UNCHECKED,0);
        field("odbcConnectionString",L"ODBC connection string",context->profile.odbcConnectionString,20,154,2,ES_PASSWORD);field("driver",L"ODBC driver name",context->profile.driver,370,154,2);
        field("odbcEnv",L"ODBC string environment variable",context->profile.odbcEnv,20,226,2);field("timeoutSec",L"Connection timeout (1-60 seconds)",std::to_string(context->profile.timeoutSec),370,226,2,ES_NUMBER);
        HWND optionsLabel=CreateWindowExW(0,L"STATIC",L"Driver options (JSON object; unsupported settings report an error)",WS_CHILD|WS_VISIBLE,20,302,680,25,window,nullptr,nullptr,nullptr);
        HWND options=CodeEditor::Create(window,7285,EditorLanguage::Json);SetWindowPos(options,nullptr,20,330,680,126,SWP_NOZORDER);CodeEditor::SetText(options,Utf8ToWide(context->profile.optionsJson));context->fields["optionsJson"]=options;context->pages.push_back({optionsLabel,2});context->pages.push_back({options,2});
        context->create=CreateWindowExW(0,L"BUTTON",L"Create a new empty file if it does not exist (file engines)",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,20,454,680,32,window,nullptr,nullptr,nullptr);SendMessageW(context->create,BM_SETCHECK,context->profile.create?BST_CHECKED:BST_UNCHECKED,0);context->pages.push_back({context->create,0});
        context->hint=CreateWindowExW(0,L"STATIC",L"Select an engine to see its installed driver status.",WS_CHILD|WS_VISIBLE,20,500,680,74,window,nullptr,nullptr,nullptr);
        context->status=CreateWindowExW(0,L"STATIC",L"Test the connection, or save its settings for later.",WS_CHILD|WS_VISIBLE,20,580,680,60,window,nullptr,nullptr,nullptr);
        context->test=CreateWindowExW(0,L"BUTTON",L"Test connection",WS_CHILD|WS_VISIBLE|WS_TABSTOP,20,652,155,34,window,reinterpret_cast<HMENU>(7291),nullptr,nullptr);
        context->save=CreateWindowExW(0,L"BUTTON",L"Save",WS_CHILD|WS_VISIBLE|WS_TABSTOP,187,652,95,34,window,reinterpret_cast<HMENU>(7292),nullptr,nullptr);
        context->connect=CreateWindowExW(0,L"BUTTON",L"Save and connect",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,294,652,190,34,window,reinterpret_cast<HMENU>(7293),nullptr,nullptr);
        context->cancel=CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,590,652,110,34,window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
        if(selected>=0){const auto& driver=context->drivers[selected];SetWindowTextW(context->hint,Utf8ToWide(driver.available?(driver.name+" driver is available."):(driver.name+": "+driver.installHint)).c_str());}
        if(selected>=0){HWND driverName=context->fields.at("driver");for(const auto& name:context->drivers[selected].installedDrivers)SendMessageW(driverName,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Utf8ToWide(name).c_str()));SetWindowTextW(driverName,Utf8ToWide(context->profile.driver).c_str());SendMessageW(driverName,CB_LIMITTEXT,32768,0);}
        SelectConnectionPage(context,0);return 0;
    }
    if(!context)return DefWindowProcW(window,message,wParam,lParam);
    try{
        if(message==WM_COMMAND){const int id=LOWORD(wParam);
            if(id>=7280&&id<=7282){SelectConnectionPage(context,id-7280);return 0;}
            if(id==7201&&HIWORD(wParam)==CBN_SELCHANGE&&!context->work){const int selected=static_cast<int>(SendMessageW(context->driver,CB_GETCURSEL,0,0));if(selected>=0&&selected<static_cast<int>(context->drivers.size())){const auto& driver=context->drivers[selected];HWND driverName=context->fields.at("driver");SendMessageW(driverName,CB_RESETCONTENT,0,0);for(const auto& name:driver.installedDrivers)SendMessageW(driverName,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Utf8ToWide(name).c_str()));SetWindowTextW(driverName,driver.installedDrivers.empty()?L"":Utf8ToWide(driver.installedDrivers.front()).c_str());SetWindowTextW(context->fields["port"],std::to_wstring(driver.defaultPort).c_str());SetWindowTextW(context->hint,Utf8ToWide(driver.available?(driver.name+" driver is available."):(driver.name+": "+driver.installHint)).c_str());}return 0;}
            if(id==IDCANCEL){SendMessageW(window,WM_CLOSE,0,0);return 0;}
            if(id==7291&&context->work){context->work->canceled.store(true);SetWindowTextW(context->status,L"Cancellation requested. Waiting for the driver operation to finish...");return 0;}
            if((id==7291||id==7292||id==7293||id==IDOK)&&!context->work){const ConnectionProfile profile=ReadConnectionFields(context);auto work=std::make_shared<ConnectionWork>();work->saving=id!=7291;work->connect=id==7293||id==IDOK;
                context->work=work;EnableConnectionFields(context,false);SetWindowTextW(context->status,work->saving?L"Saving connection...":L"Testing connection...");DbEngine* engine=context->engine;
                try{context->worker=std::thread([work,profile,engine](){try{if(work->saving)work->database=engine->SaveConnection(profile,work->connect,&work->canceled);else work->message=engine->TestConnection(profile,&work->canceled);work->success=true;}catch(const std::exception& error){work->message=error.what();}catch(...){work->message="Connection operation failed unexpectedly.";}work->done.store(true,std::memory_order_release);});SetTimer(window,91,50,nullptr);}catch(...){context->work.reset();EnableConnectionFields(context,true);throw;}return 0;
            }
        }
        if(message==WM_TIMER&&wParam==91&&context->work&&context->work->done.load(std::memory_order_acquire)){auto completed=context->work;if(context->worker.joinable())context->worker.join();context->work.reset();KillTimer(window,91);EnableConnectionFields(context,true);
            if(completed->success&&completed->saving){if(context->onSaved)context->onSaved(completed->database,completed->connect);DestroyWindow(window);return 0;}
            SetWindowTextW(context->status,Utf8ToWide(completed->success?completed->message:("Connection failed: "+completed->message)).c_str());if(context->closing)DestroyWindow(window);return 0;
        }
        if(message==WM_CLOSE){if(context->work){context->closing=true;context->work->canceled.store(true);SetWindowTextW(context->status,L"Canceling connection operation before closing...");}else DestroyWindow(window);return 0;}
        if(message==WM_DESTROY){KillTimer(window,91);if(context->work)context->work->canceled.store(true);return 0;}
    }catch(const std::exception& error){SetWindowTextW(context->status,Utf8ToWide(error.what()).c_str());if(!context->closing)MessageBoxW(window,Utf8ToWide(error.what()).c_str(),L"Connection settings",MB_OK|MB_ICONERROR);return 0;}
    return DefWindowProcW(window,message,wParam,lParam);
}
void ShowConnectionDialog(HWND parent,DbEngine* engine,const ConnectionProfile& profile,const std::function<void(const DatabaseInfo&,bool)>& onSaved) {
    ConnectionDialogContext context;context.engine=engine;context.profile=profile;context.drivers=engine->ListDrivers();context.onSaved=onSaved;
    WNDCLASSEXW type{};type.cbSize=sizeof(type);type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=ConnectionDialogProc;type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=L"DataForgeConnectionProfile";RegisterClassExW(&type);
    HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,type.lpszClassName,profile.id.empty()?L"New database connection":L"Edit database connection",WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,0,0,736,735,parent,nullptr,type.hInstance,&context);
    RunModalLoop(window,parent);UnregisterClassW(type.lpszClassName,type.hInstance);
}
static std::string EncodeMockUrl(const std::string& text) {
    static constexpr char hex[]="0123456789ABCDEF";std::string encoded;for(unsigned char byte:text){if(std::isalnum(byte)||byte=='-'||byte=='_'||byte=='.'||byte=='~')encoded+=static_cast<char>(byte);else{encoded+='%';encoded+=hex[byte>>4];encoded+=hex[byte&15];}}return encoded;
}
struct MockDialogContext { NativeMockServer* server=nullptr;std::string database,table;HWND port=nullptr,url=nullptr,status=nullptr; };
static void RefreshMockDialog(MockDialogContext* context) {
    const std::string url="http://127.0.0.1:"+std::to_string(context->server->IsRunning()?context->server->Port():static_cast<unsigned short>(8001))+"/api/mock/data/"+EncodeMockUrl(context->table.empty()?"table":context->table)+"?db="+EncodeMockUrl(context->database);
    SetWindowTextW(context->url,Utf8ToWide(url).c_str());SetWindowTextW(context->status,context->server->IsRunning()?L"Local REST server is running. POST, PUT, PATCH and DELETE update database rows.":L"Local REST server is stopped. Choose a port and start it to use the endpoint.");
}
static LRESULT CALLBACK MockDialogProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* context=reinterpret_cast<MockDialogContext*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_CREATE){context=static_cast<MockDialogContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));
        CreateWindowExW(0,L"STATIC",L"Local mock REST API",WS_CHILD|WS_VISIBLE,20,18,620,28,window,nullptr,nullptr,nullptr);
        CreateWindowExW(0,L"STATIC",L"Port (0 chooses a free port)",WS_CHILD|WS_VISIBLE,20,64,260,28,window,nullptr,nullptr,nullptr);
        context->port=CreateWindowExW(0,L"EDIT",context->server->IsRunning()?std::to_wstring(context->server->Port()).c_str():L"8001",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_NUMBER,290,62,95,30,window,reinterpret_cast<HMENU>(7401),nullptr,nullptr);SendMessageW(context->port,EM_SETLIMITTEXT,5,0);
        CreateWindowExW(0,L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|WS_TABSTOP,400,62,100,32,window,reinterpret_cast<HMENU>(7402),nullptr,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|WS_TABSTOP,515,62,100,32,window,reinterpret_cast<HMENU>(7403),nullptr,nullptr);
        context->status=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,20,108,620,55,window,nullptr,nullptr,nullptr);
        context->url=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|ES_READONLY,20,174,620,34,window,reinterpret_cast<HMENU>(7404),nullptr,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Copy endpoint URL",WS_CHILD|WS_VISIBLE|WS_TABSTOP,20,220,200,34,window,reinterpret_cast<HMENU>(7405),nullptr,nullptr);
        HWND help=CodeEditor::Create(window,7406,EditorLanguage::PlainText,true);SetWindowPos(help,nullptr,20,270,620,230,SWP_NOZORDER);
        CodeEditor::SetText(help,L"GET /api/mock/data/<table>?db=<connection-id>&page=1&limit=25\r\nGET /api/mock/data/<table>/<id>?db=<connection-id>\r\nPOST /api/mock/data/<table>?db=<connection-id>\r\nPUT /api/mock/data/<table>/<id>?db=<connection-id>\r\nPATCH /api/mock/data/<table>/<id>?db=<connection-id>\r\nDELETE /api/mock/data/<table>/<id>?db=<connection-id>\r\n\r\nSQL connections: SQLite, DuckDB or an installed ODBC provider.\r\nAccount permissions and read-only profile settings apply.\r\nPOST/PUT/PATCH use a JSON object. NULL uses null. Binary values use\r\n{\"$binary\":\"00ff\",\"$encoding\":\"hex\"}. Record endpoints require one primary-key column.\r\nUse the API Client to send requests to this local endpoint.");
        CreateWindowExW(0,L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,530,516,110,34,window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);RefreshMockDialog(context);return 0;
    }
    if(!context)return DefWindowProcW(window,message,wParam,lParam);
    try{if(message==WM_COMMAND){const int id=LOWORD(wParam);if(id==7402){const auto value=WindowText(context->port);if(value.empty())throw std::runtime_error("Enter a port between 0 and 65535.");const int port=_wtoi(value.c_str());if(port<0||port>65535)throw std::runtime_error("Enter a port between 0 and 65535.");const auto actual=context->server->Start(static_cast<unsigned short>(port));SetWindowTextW(context->port,std::to_wstring(actual).c_str());RefreshMockDialog(context);}
            else if(id==7403){context->server->Stop();RefreshMockDialog(context);}else if(id==7405){if(!SetClipboardText(window,WindowText(context->url)))throw std::runtime_error("Clipboard is currently unavailable.");}else if(id==IDCANCEL||id==IDOK)DestroyWindow(window);return 0;}}
    catch(const std::exception& error){MessageBoxW(window,Utf8ToWide(error.what()).c_str(),L"Local REST server",MB_OK|MB_ICONERROR);}
    return DefWindowProcW(window,message,wParam,lParam);
}
void ShowMockServerDialog(HWND parent,NativeMockServer* server,const std::string& databaseId,const std::string& table) {
    MockDialogContext context;context.server=server;context.database=databaseId;context.table=table;
    WNDCLASSEXW type{};type.cbSize=sizeof(type);type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=MockDialogProc;type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=L"DataForgeNativeMockPanel";RegisterClassExW(&type);
    HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,type.lpszClassName,L"Local mock REST API",WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,0,0,680,605,parent,nullptr,type.hInstance,&context);RunModalLoop(window,parent);UnregisterClassW(type.lpszClassName,type.hInstance);
}
static const wchar_t* ValueTypeName(DbValueType type) {
    static const wchar_t* names[] = {L"NULL", L"Text", L"Integer", L"Real", L"Boolean", L"Binary (hex)", L"JSON"};
    const auto index = static_cast<size_t>(type); return index < std::size(names) ? names[index] : L"Text";
}
static std::string BytesToHex(const std::string& bytes) {
    static constexpr char digits[]="0123456789abcdef"; std::string result; result.reserve(bytes.size()*2);
    for(unsigned char byte:bytes){result+=digits[byte>>4];result+=digits[byte&15];} return result;
}
static std::string HexToBytes(std::string text) {
    text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char character){return std::isspace(character)!=0;}),text.end());
    if(text.rfind("0x",0)==0||text.rfind("0X",0)==0)text.erase(0,2);
    if(text.size()%2)throw std::runtime_error("Binary data needs pairs of hexadecimal digits; for example 00ff.");
    const auto digit=[](char value){if(value>='0'&&value<='9')return value-'0';if(value>='a'&&value<='f')return value-'a'+10;if(value>='A'&&value<='F')return value-'A'+10;throw std::runtime_error("Binary data contains a character outside 0-9 and a-f.");};
    std::string bytes;bytes.reserve(text.size()/2);for(size_t index=0;index<text.size();index+=2)bytes+=static_cast<char>((digit(text[index])<<4)|digit(text[index+1]));return bytes;
}
struct RowEditorContext {
    const TableDataResult* table=nullptr;
    TypedRow original,changes;
    HWND list=nullptr,value=nullptr,type=nullptr;
    int selected=-1;
    bool insert=false,loading=false,dirty=false,accepted=false,valueTooLarge=false;
    std::vector<std::string> readOnlyColumns;
};
static bool ReadOnlyRowColumn(const RowEditorContext& context, const std::string& column) {
    return std::find(context.readOnlyColumns.begin(), context.readOnlyColumns.end(), column) != context.readOnlyColumns.end();
}
static DbValueType SuggestedValueType(const RowEditorContext& context,const std::string& column) {
    const auto changed=context.changes.find(column),original=context.original.find(column);
    if(changed!=context.changes.end())return changed->second.type;if(original!=context.original.end())return original->second.type;
    for(const auto& metadata:context.table->columnMetadata)if(metadata.name==column){std::string type=metadata.type;std::transform(type.begin(),type.end(),type.begin(),[](unsigned char character){return static_cast<char>(std::toupper(character));});
        if(type.find("INT")!=std::string::npos)return DbValueType::Integer;if(type.find("BOOL")!=std::string::npos)return DbValueType::Boolean;
        if(type.find("REAL")!=std::string::npos||type.find("FLOAT")!=std::string::npos||type.find("DOUBLE")!=std::string::npos)return DbValueType::Real;
        if(type.find("BLOB")!=std::string::npos||type.find("BINARY")!=std::string::npos)return DbValueType::Blob;if(type.find("JSON")!=std::string::npos)return DbValueType::Json;
    }
    return DbValueType::Text;
}
static void RefreshRowValue(RowEditorContext* context,int index) {
    if(index<0||index>=static_cast<int>(context->table->columns.size()))return;const auto& column=context->table->columns[index];
    const auto changed=context->changes.find(column),original=context->original.find(column);const DbValue* value=changed!=context->changes.end()?&changed->second:original!=context->original.end()?&original->second:nullptr;
    const std::wstring text=!value?L"[default]":value->type==DbValueType::Null?L"[NULL]":Utf8ToWide(value->type==DbValueType::Blob?BytesToHex(value->text):value->text);
    const std::wstring type=value?ValueTypeName(value->type):L"Default";ListView_SetItemText(context->list,index,2,const_cast<LPWSTR>(type.c_str()));ListView_SetItemText(context->list,index,3,const_cast<LPWSTR>(text.c_str()));
}
static void CommitRowValue(RowEditorContext* context) {
    if(context->valueTooLarge||!context->dirty||context->selected<0||context->selected>=static_cast<int>(context->table->columns.size()))return;
    const auto& column=context->table->columns[context->selected];DbValue value;value.type=static_cast<DbValueType>(SendMessageW(context->type,CB_GETCURSEL,0,0));
    if (ReadOnlyRowColumn(*context, column)) { context->dirty = false; return; }
    value.text=WideToUtf8(CodeEditor::GetText(context->value));size_t parsed=0;
    switch(value.type){
    case DbValueType::Null:value.text.clear();break;
    case DbValueType::Blob:value.text=HexToBytes(value.text);break;
    case DbValueType::Integer:{const auto number=std::stoll(value.text,&parsed);if(parsed!=value.text.size())throw std::runtime_error("Enter a whole signed64-bit integer, or select another value type.");value.text=std::to_string(number);break;}
    case DbValueType::Real:{const auto number=std::stod(value.text,&parsed);if(parsed!=value.text.size()||!std::isfinite(number))throw std::runtime_error("Enter a finite real number.");break;}
    case DbValueType::Boolean:{std::string lower=value.text;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char character){return static_cast<char>(std::tolower(character));});if(lower!="true"&&lower!="false"&&lower!="1"&&lower!="0")throw std::runtime_error("Boolean values must be true, false, 1, or 0.");value.text=(lower=="true"||lower=="1")?"true":"false";break;}
    case DbValueType::Json:{const auto parsedJson=nlohmann::json::parse(value.text);value.text=parsedJson.dump();break;}
    default:break;
    }
    const auto original=context->original.find(column);
    if(!context->insert&&original!=context->original.end()&&original->second.type==value.type&&original->second.text==value.text)context->changes.erase(column);else context->changes[column]=std::move(value);
    context->dirty=false;RefreshRowValue(context,context->selected);
}
static void LoadRowColumn(RowEditorContext* context,int index) {
    if(index<0||index>=static_cast<int>(context->table->columns.size()))return;context->selected=index;const auto& column=context->table->columns[index];
    const auto changed=context->changes.find(column),original=context->original.find(column);const DbValue* value=changed!=context->changes.end()?&changed->second:original!=context->original.end()?&original->second:nullptr;
    const DbValueType type=SuggestedValueType(*context,column);context->loading=true;
    context->valueTooLarge=value && (type==DbValueType::Blob ? value->text.size()>8*1024*1024 : value->text.size()>16*1024*1024);
    SetWindowTextW(context->value,context->valueTooLarge?L"This value exceeds the editor size limit and is preserved unchanged. Use SQL or a native file export to work with it.":value?Utf8ToWide(type==DbValueType::Blob?BytesToHex(value->text):value->text).c_str():L"");
    const bool readOnly = ReadOnlyRowColumn(*context, column);
    SendMessageW(context->type,CB_SETCURSEL,static_cast<int>(type),0);EnableWindow(context->value,type!=DbValueType::Null&&!context->valueTooLarge&&!readOnly);EnableWindow(context->type,!context->valueTooLarge&&!readOnly);
    EnableWindow(GetDlgItem(GetParent(context->value), 7103), !readOnly);
    context->loading=false;context->dirty=false;
}
static LRESULT CALLBACK RowEditorProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* context=reinterpret_cast<RowEditorContext*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_CREATE){context=static_cast<RowEditorContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));
        CreateWindowExW(0,L"STATIC",context->insert?L"Choose a value type. Untouched fields retain database defaults.":L"Only changed fields are saved. NULL, text and binary values remain distinct.",WS_CHILD|WS_VISIBLE,18,14,680,28,window,nullptr,nullptr,nullptr);
        context->list=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,18,48,680,245,window,reinterpret_cast<HMENU>(7101),nullptr,nullptr);
        ListView_SetExtendedListViewStyle(context->list,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        const wchar_t* labels[]={L"Column",L"Database type",L"Value type",L"Value"};const int widths[]={180,120,100,265};
        for(int index=0;index<4;++index){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.cx=widths[index];column.pszText=const_cast<LPWSTR>(labels[index]);ListView_InsertColumn(context->list,index,&column);}
        CreateWindowExW(0,L"STATIC",L"Value type",WS_CHILD|WS_VISIBLE,18,309,115,26,window,nullptr,nullptr,nullptr);
        context->type=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,138,303,240,220,window,reinterpret_cast<HMENU>(7104),nullptr,nullptr);
        for(int type=0;type<=static_cast<int>(DbValueType::Json);++type)SendMessageW(context->type,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(ValueTypeName(static_cast<DbValueType>(type))));
        CreateWindowExW(0,L"STATIC",L"Binary uses hexadecimal pairs.",WS_CHILD|WS_VISIBLE,390,309,308,26,window,nullptr,nullptr,nullptr);
        context->value=CodeEditor::Create(window,7102,EditorLanguage::PlainText);SetWindowPos(context->value,nullptr,18,343,680,66,SWP_NOZORDER);
        CreateWindowExW(0,L"BUTTON",context->insert?L"Use default":L"Reset field",WS_CHILD|WS_VISIBLE|WS_TABSTOP,18,425,130,34,window,reinterpret_cast<HMENU>(7103),nullptr,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Save Row",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,466,425,112,34,window,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,586,425,112,34,window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
        for(int index=0;index<static_cast<int>(context->table->columns.size());++index){const auto& name=context->table->columns[index];const auto label=Utf8ToWide(name);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=index;item.pszText=const_cast<LPWSTR>(label.c_str());ListView_InsertItem(context->list,&item);
            std::wstring type=L"TEXT";for(const auto& metadata:context->table->columnMetadata)if(metadata.name==name)type=Utf8ToWide(metadata.type)+(metadata.notnull?L" (required)":L"");ListView_SetItemText(context->list,index,1,const_cast<LPWSTR>(type.c_str()));RefreshRowValue(context,index);}
        if(!context->table->columns.empty()){LoadRowColumn(context,0);ListView_SetItemState(context->list,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);}return 0;
    }
    if(!context)return DefWindowProcW(window,message,wParam,lParam);
    try{
        if(message==WM_NOTIFY){const auto* notification=reinterpret_cast<NMHDR*>(lParam);if(notification->idFrom==7101&&notification->code==LVN_ITEMCHANGED&&!context->loading){const auto* selection=reinterpret_cast<NMLISTVIEW*>(lParam);
            if((selection->uNewState&LVIS_SELECTED)&&!(selection->uOldState&LVIS_SELECTED)&&selection->iItem>=0&&selection->iItem<static_cast<int>(context->table->columns.size())){CommitRowValue(context);LoadRowColumn(context,selection->iItem);}}
            return 0;}
        if(message==WM_COMMAND){const int id=LOWORD(wParam);
            if(id==7102&&HIWORD(wParam)==EN_CHANGE&&!context->loading)context->dirty=true;
            else if(id==7104&&HIWORD(wParam)==CBN_SELCHANGE&&!context->loading){context->dirty=true;EnableWindow(context->value,SendMessageW(context->type,CB_GETCURSEL,0,0)!=static_cast<int>(DbValueType::Null));}
            else if(id==7103&&context->selected>=0){context->changes.erase(context->table->columns[context->selected]);LoadRowColumn(context,context->selected);RefreshRowValue(context,context->selected);}
            else if(id==IDOK){CommitRowValue(context);context->accepted=true;DestroyWindow(window);}else if(id==IDCANCEL)DestroyWindow(window);return 0;}
    }catch(const std::exception& error){context->loading=true;ListView_SetItemState(context->list,-1,0,LVIS_SELECTED);if(context->selected>=0)ListView_SetItemState(context->list,context->selected,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);context->loading=false;MessageBoxW(window,Utf8ToWide(error.what()).c_str(),L"Row value could not be saved",MB_OK|MB_ICONERROR);SetFocus(context->value);}
    return DefWindowProcW(window,message,wParam,lParam);
}
bool ShowTypedRowEditorDialog(HWND parent,const TableDataResult& table,const TypedRow& original,bool insert,TypedRow& changes,const std::vector<std::string>& readOnlyColumns) {
    RowEditorContext context;context.table=&table;context.original=original;context.insert=insert;context.readOnlyColumns=readOnlyColumns;
    WNDCLASSEXW type{};type.cbSize=sizeof(type);type.lpfnWndProc=RowEditorProc;type.hInstance=GetModuleHandleW(nullptr);type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=L"DataForgeNativeRowEditor";RegisterClassExW(&type);
    HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,type.lpszClassName,insert?L"Add row - DataForge Studio":L"Edit row - DataForge Studio",WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,0,0,736,515,parent,nullptr,type.hInstance,&context);
    RunModalLoop(window,parent);UnregisterClassW(type.lpszClassName,type.hInstance);if(context.accepted)changes=std::move(context.changes);return context.accepted;
}
bool ShowRowEditorDialog(HWND parent,const TableDataResult& table,const std::map<std::string,std::string>& original,bool insert,std::map<std::string,std::string>& changes) {
    TypedRow typedOriginal,typedChanges;for(const auto& field:original)typedOriginal[field.first]={DbValueType::Text,field.second};
    if(!ShowTypedRowEditorDialog(parent,table,typedOriginal,insert,typedChanges))return false;
    for(const auto& field:typedChanges)changes[field.first]=field.second.type==DbValueType::Null?"NULL":field.second.text;return true;
}
bool RunConnectionModalSelfTests(DbEngine* engine,std::wstring& failure) {
    bool passed=true;const auto check=[&](bool condition,const wchar_t* label){if(!condition){failure+=std::wstring(label)+L"; ";passed=false;}};
    const int oldScale=Theme::GetTextScale();const UINT oldDpi=static_cast<UINT>(MulDiv(Theme::Scale(96),100,oldScale));
    struct RestoreTheme{UINT dpi;int scale;~RestoreTheme(){Theme::SetDpi(dpi);Theme::SetTextScale(scale);}}restore{oldDpi,oldScale};Theme::SetDpi(144);Theme::SetTextScale(150);
    ConnectionDialogContext context;context.engine=engine;context.drivers=engine->ListDrivers();context.profile.name="Native modal profile";context.profile.path=(std::filesystem::u8path(engine->WorkspaceDirectory())/"modal-profile.db").u8string();context.profile.create=true;
    bool saved=false;DatabaseInfo savedInfo;context.onSaved=[&](const DatabaseInfo& info,bool connected){saved=true;savedInfo=info;check(!connected,L"Save settings does not request an immediate connection");};
    WNDCLASSEXW type{};type.cbSize=sizeof(type);type.lpfnWndProc=ConnectionDialogProc;type.hInstance=GetModuleHandleW(nullptr);type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=L"DataForgeNativeConnectionAcceptance";RegisterClassExW(&type);
    HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,type.lpszClassName,L"Native connection acceptance",WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,0,0,736,735,nullptr,nullptr,type.hInstance,&context);
    if(!window){failure+=L"Native connection acceptance window; ";UnregisterClassW(type.lpszClassName,type.hInstance);return false;}
    const auto pump=[&](){const ULONGLONG deadline=GetTickCount64()+10000;while(context.work&&GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT){PostQuitMessage(static_cast<int>(message.wParam));context.work->canceled.store(true);return false;}TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);}if(context.work){context.work->canceled.store(true);return false;}return true;};
    try{
        ModalViewport viewport;const RECT work{0,0,1280,720};PrepareModalWindow(window,nullptr,viewport,&work);
        RECT bounds{};GetWindowRect(window,&bounds);check(bounds.left>=0&&bounds.top>=0&&bounds.right<=1280&&bounds.bottom<=720,L"connection modal remains within enlarged monitor viewport");
        SelectConnectionPage(&context,1);check(IsWindowVisible(context.fields.at("passwordEnv"))==FALSE && (GetWindowLongPtrW(context.fields.at("passwordEnv"),GWL_STYLE)&WS_VISIBLE)!=0 && !(GetWindowLongPtrW(context.fields.at("host"),GWL_STYLE)&WS_VISIBLE),L"connection security page exposes its fields and hides basic fields");
        SelectConnectionPage(&context,2);check((GetWindowLongPtrW(context.fields.at("odbcConnectionString"),GWL_STYLE)&WS_VISIBLE)!=0,L"connection advanced page exposes driver configuration");
        HWND save=GetDlgItem(window,7292);RevealModalFocus(viewport,save);RECT action{},client{};GetWindowRect(save,&action);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&action),2);GetClientRect(window,&client);
        check(action.left>=0&&action.top>=0&&action.right<=client.right&&action.bottom<=client.bottom,L"connection keyboard focus reveals Save below initial viewport");
        SelectConnectionPage(&context,0);const auto profile=ReadConnectionFields(&context);check(profile.name==context.profile.name&&profile.path==context.profile.path&&profile.type=="sqlite"&&profile.create,L"native connection fields roundtrip provider profile settings");
        SendMessageW(window,WM_COMMAND,7291,0);check(context.work!=nullptr,L"connection Test schedules an owned background job");check(pump(),L"connection test finishes and joins its worker");
        check(!std::filesystem::exists(std::filesystem::u8path(context.profile.path)),L"connection Test does not create an absent database file");
        SendMessageW(window,WM_COMMAND,7292,0);check(pump()&&saved&&!savedInfo.id.empty(),L"native Save persists the completed connection profile");
        check(!context.worker.joinable(),L"connection completion joins the thread before the modal closes");
    }catch(const std::exception& error){failure+=L"Connection modal acceptance: "+Utf8ToWide(error.what())+L"; ";passed=false;}
    if(IsWindow(window))DestroyWindow(window);UnregisterClassW(type.lpszClassName,type.hInstance);
    try{
        NativeMockServer server(engine);MockDialogContext mock;mock.server=&server;mock.database=engine->ListDatabases().front().id;mock.table="ui_rows";
        WNDCLASSEXW mockType{};mockType.cbSize=sizeof(mockType);mockType.lpfnWndProc=MockDialogProc;mockType.hInstance=type.hInstance;mockType.hCursor=type.hCursor;mockType.lpszClassName=L"DataForgeNativeMockAcceptance";RegisterClassExW(&mockType);
        HWND mockWindow=CreateWindowExW(0,mockType.lpszClassName,L"Native mock acceptance",WS_POPUP|WS_CAPTION,0,0,690,600,nullptr,nullptr,mockType.hInstance,&mock);
        check(mockWindow!=nullptr,L"native mock server panel creates real controls");
        if(mockWindow){SetWindowTextW(mock.port,L"0");SendMessageW(mockWindow,WM_COMMAND,7402,0);check(server.IsRunning()&&server.Port()!=0&&WindowText(mock.url).find(std::to_wstring(server.Port()))!=std::wstring::npos,L"mock panel starts a real loopback endpoint and reports its assigned port");SendMessageW(mockWindow,WM_COMMAND,7403,0);check(!server.IsRunning(),L"mock panel Stop joins the native listener");DestroyWindow(mockWindow);}
        UnregisterClassW(mockType.lpszClassName,mockType.hInstance);
    }catch(const std::exception& error){failure+=L"Mock panel acceptance: "+Utf8ToWide(error.what())+L"; ";passed=false;}
    return passed;
}
bool RunModalSelfTests(std::wstring& failure) {
    const int oldScale = Theme::GetTextScale();
    const UINT oldDpi = static_cast<UINT>(MulDiv(Theme::Scale(96), 100, oldScale));
    struct RestoreTheme { UINT dpi; int scale; ~RestoreTheme() { Theme::SetDpi(dpi); Theme::SetTextScale(scale); } } restore{oldDpi, oldScale};
    Theme::SetDpi(144); Theme::SetTextScale(150);
    bool passed = true;
    const auto check = [&](bool condition, const wchar_t* label) { if (!condition) { failure += std::wstring(label) + L"; "; passed = false; } };
    TableDataResult table; table.columns = {"id", "description"};
    RowEditorContext context; context.table = &table; context.insert = true;
    WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.lpfnWndProc = RowEditorProc; type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = L"DataForgeNativeModalAcceptance";
    RegisterClassExW(&type);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, type.lpszClassName, L"Native modal acceptance",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, 736, 515, nullptr, nullptr, type.hInstance, &context);
    if (!window) { failure += L"Native modal acceptance window; "; return false; }
    {
        ModalViewport viewport; const RECT work{0, 0, 1280, 720}; PrepareModalWindow(window, nullptr, viewport, &work);
        RECT bounds{}; GetWindowRect(window, &bounds);
        check(bounds.left >= work.left && bounds.top >= work.top && bounds.right <= work.right && bounds.bottom <= work.bottom,
            L"144 DPI 150% modal stays within monitor work area");
        check(viewport.contentWidth > bounds.right - bounds.left && viewport.contentHeight > bounds.bottom - bounds.top,
            L"enlarged modal retains full content through native scrolling");
        HWND save = GetDlgItem(window, IDOK); RevealModalFocus(viewport, save);
        RECT action{}, client{}; GetWindowRect(save, &action); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&action), 2); GetClientRect(window, &client);
        check(action.left >= 0 && action.top >= 0 && action.right <= client.right && action.bottom <= client.bottom,
            L"keyboard focus reveals a save action below the initial viewport");
        check(GetNextDlgTabItem(window, GetDlgItem(window, 7103), FALSE) == save,
            L"row modal preserves native Tab order through the scroll viewport");
        context.insert=false;context.original["id"]={DbValueType::Null,""};context.original["description"]={DbValueType::Blob,std::string("\0\xff",2)};
        context.changes.clear();LoadRowColumn(&context,0);CommitRowValue(&context);
        check(context.changes.empty() && !IsWindowEnabled(context.value),L"untouched NULL remains NULL and disables value editing");
        LoadRowColumn(&context,1);context.dirty=true;CommitRowValue(&context);
        check(context.changes.empty(),L"binary edit roundtrip preserves zero and non-UTF8 bytes");
        LoadRowColumn(&context,0);SendMessageW(context.type,CB_SETCURSEL,static_cast<int>(DbValueType::Text),0);SetWindowTextW(context.value,L"NULL");context.dirty=true;CommitRowValue(&context);
        check(context.changes.at("id").type==DbValueType::Text && context.changes.at("id").text=="NULL",L"literal NULL text remains distinct from SQL NULL");
        SendMessageW(context.type,CB_SETCURSEL,static_cast<int>(DbValueType::Integer),0);SetWindowTextW(context.value,L"12x");context.dirty=true;
        bool invalidRejected=false;try{CommitRowValue(&context);}catch(const std::exception&){invalidRejected=true;}check(invalidRejected && context.changes.at("id").type==DbValueType::Text,L"invalid numeric edit preserves the previously staged value");
        context.dirty=false;context.changes.clear();context.original.clear();context.insert=true;LoadRowColumn(&context,0);CommitRowValue(&context);
        check(context.changes.empty(),L"untouched inserted fields retain database defaults");
        context.insert = false; context.original["description"] = {DbValueType::Text, "preserved"}; context.readOnlyColumns = {"description"};
        LoadRowColumn(&context, 1); SetWindowTextW(context.value, L"replacement"); context.dirty = true; CommitRowValue(&context);
        check(context.changes.empty() && !IsWindowEnabled(context.value) && !IsWindowEnabled(context.type)
            && !IsWindowEnabled(GetDlgItem(GetParent(context.value), 7103)), L"provider-derived and immutable row fields remain unchanged and disabled");
        context.readOnlyColumns.clear(); context.original["description"] = {DbValueType::Text, "first paragraph\nsecond paragraph"};
        LoadRowColumn(&context, 1); context.dirty = true; CommitRowValue(&context);
        check(context.changes.empty(), L"multiline row text roundtrip preserves LF paragraph bytes");
        CodeEditor::SetText(context.value, L"first paragraph\nupdated paragraph"); context.dirty = true; CommitRowValue(&context);
        check(context.changes.at("description").text == "first paragraph\nupdated paragraph",
            L"multiline row edits use normalized native editor text");
        check(HexToBytes("0x00 ff")==std::string("\0\xff",2),L"binary hexadecimal accepts prefix and whitespace");
        DestroyWindow(window);
    }
    UnregisterClassW(type.lpszClassName, type.hInstance);
    return passed;
}
} // namespace native_app
