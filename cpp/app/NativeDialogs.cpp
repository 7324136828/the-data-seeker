#include "NativeDialogs.h"
#include "Theme.h"
#include <algorithm>

namespace native_app {
namespace {
struct DialogState {
    HWND window = nullptr;
    HWND editor = nullptr;
    HWND save = nullptr;
    HWND cancel = nullptr;
    bool done = false;
    bool accepted = false;
    std::function<std::wstring(const std::wstring&)> validate;
};
LRESULT CALLBACK EditorDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) { state = reinterpret_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)); state->window = hwnd; }
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_CLOSE) { state->done = true; return 0; }
    if (msg == WM_SIZE && state->editor) {
        const int w = LOWORD(lp), h = HIWORD(lp); const int gap = Theme::Scale(16), row = Theme::Scale(36);
        SetWindowPos(state->editor, nullptr, gap, gap, (std::max)(1, w - 2 * gap), (std::max)(1, h - 3 * gap - row), SWP_NOZORDER);
        SetWindowPos(state->cancel, nullptr, w - gap - Theme::Scale(100), h - gap - row, Theme::Scale(100), row, SWP_NOZORDER);
        SetWindowPos(state->save, nullptr, w - 2 * gap - Theme::Scale(210), h - gap - row, Theme::Scale(110), row, SWP_NOZORDER);
        return 0;
    }
    if (msg == WM_GETMINMAXINFO) { auto* info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {Theme::Scale(500), Theme::Scale(300)}; return 0; }
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
        if (LOWORD(wp) == IDCANCEL) state->done = true;
        if (LOWORD(wp) == IDOK) {
            const auto text = CodeEditor::GetText(state->editor);
            std::wstring error;
            try { if (state->validate) error = state->validate(text); } catch (...) { error = L"The value could not be validated. Check its format and try again."; }
            if (!error.empty()) { MessageBoxW(hwnd, error.c_str(), L"Invalid input", MB_OK | MB_ICONERROR); return 0; }
            state->accepted = true; state->done = true;
        }
        return 0;
    }
    if (msg == WM_PAINT) { PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint); RECT rect{}; GetClientRect(hwnd, &rect); FillRect(dc, &rect, Theme::GetBgSecondaryBrush()); EndPaint(hwnd, &paint); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}
bool EditNativeText(HWND owner, const std::wstring& title, std::wstring& text, EditorLanguage language,
    const std::function<std::wstring(const std::wstring&)>& validate) {
    DialogState state; state.validate = validate;
    WNDCLASSW wc{}; wc.lpfnWndProc = EditorDialogProc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"DataForgeNativeEditorDialog"; RegisterClassW(&wc);
    RECT parent{}; GetWindowRect(owner, &parent); const int width = Theme::Scale(740), height = Theme::Scale(500);
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        parent.left + (parent.right - parent.left - width) / 2, parent.top + (parent.bottom - parent.top - height) / 2, width, height, owner, nullptr, wc.hInstance, &state);
    if (!hwnd) return false;
    state.editor = CodeEditor::Create(hwnd, 1, language);
    state.save = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 32, hwnd, reinterpret_cast<HMENU>(IDOK), wc.hInstance, nullptr);
    state.cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 32, hwnd, reinterpret_cast<HMENU>(IDCANCEL), wc.hInstance, nullptr);
    if (!state.editor || !state.save || !state.cancel) { DestroyWindow(hwnd); return false; }
    CodeEditor::SetText(state.editor, text); ShowWindow(state.editor, SW_SHOW); Theme::ApplyToWindow(hwnd);
    RECT client{}; GetClientRect(hwnd, &client); SendMessageW(hwnd, WM_SIZE, 0, MAKELPARAM(client.right, client.bottom));
    EnableWindow(owner, FALSE); ShowWindow(hwnd, SW_SHOW); SetFocus(state.editor);
    MSG message{};
    while (!state.done) {
        const int result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) { if (result == 0) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && !(SendMessageW(GetFocus(),WM_GETDLGCODE,message.wParam,reinterpret_cast<LPARAM>(&message))&DLGC_WANTMESSAGE)) { state.done = true; continue; }
        if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (state.accepted) text = CodeEditor::GetText(state.editor);
    EnableWindow(owner, TRUE); DestroyWindow(hwnd); SetActiveWindow(owner); return state.accepted;
}
}
