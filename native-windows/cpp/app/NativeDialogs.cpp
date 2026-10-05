#include "NativeDialogs.h"
#include "Theme.h"
#include <algorithm>
#include <commdlg.h>
#include <objbase.h>
#include <vector>
#include <stdexcept>
#pragma comment(lib,"comdlg32.lib")

namespace native_app {
std::wstring ChooseNativeFile(HWND owner,const wchar_t* title,const wchar_t* filter,bool save,const wchar_t* suggestedName) {
    std::vector<wchar_t> path(32768);wcscpy_s(path.data(),path.size(),suggestedName);
    OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=owner;dialog.lpstrTitle=title;dialog.lpstrFilter=filter;dialog.lpstrFile=path.data();dialog.nMaxFile=static_cast<DWORD>(path.size());
    dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    const BOOL accepted=save?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog);
    if(!accepted&&CommDlgExtendedError())throw std::runtime_error("Windows could not open the file picker.");
    return accepted?std::wstring(path.data()):std::wstring{};
}
void WriteNativeFileAtomic(const std::wstring& path,const std::string& content) {
    GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Windows could not prepare the export.");wchar_t identity[40]{};StringFromGUID2(id,identity,40);
    const auto temporary=path+L".pending-"+identity;
    struct FileGuard {HANDLE handle=INVALID_HANDLE_VALUE;std::wstring path;bool published=false;~FileGuard(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);if(!published&&!path.empty())DeleteFileW(path.c_str());}} file;
    file.path=temporary;file.handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file.handle==INVALID_HANDLE_VALUE)throw std::runtime_error("The export destination is not writable.");
    size_t position=0;while(position<content.size()){const DWORD count=static_cast<DWORD>((std::min)(content.size()-position,size_t(1024*1024)));DWORD written=0;
        if(!WriteFile(file.handle,content.data()+position,count,&written,nullptr)||written!=count)throw std::runtime_error("The export could not be fully written. The existing file is preserved.");position+=written;}
    if(!FlushFileBuffers(file.handle))throw std::runtime_error("The export could not be flushed. The existing file is preserved.");CloseHandle(file.handle);file.handle=INVALID_HANDLE_VALUE;
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("The export could not replace the destination. The existing file is preserved.");file.published=true;
}
namespace {
struct PickState {HWND combo=nullptr;int selected=-1;bool done=false;};
LRESULT CALLBACK PickProc(HWND window,UINT message,WPARAM wp,LPARAM lp){auto* state=reinterpret_cast<PickState*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){state=static_cast<PickState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}if(!state)return DefWindowProcW(window,message,wp,lp);if(message==WM_CLOSE){state->done=true;return 0;}if(message==WM_COMMAND&&(LOWORD(wp)==IDOK||LOWORD(wp)==IDCANCEL)){if(LOWORD(wp)==IDOK)state->selected=static_cast<int>(SendMessageW(state->combo,CB_GETCURSEL,0,0));state->done=true;return 0;}if(message==WM_PAINT){PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);FillRect(dc,&rect,Theme::GetBgSecondaryBrush());EndPaint(window,&paint);return 0;}return DefWindowProcW(window,message,wp,lp);}
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
    if (msg == WM_GETMINMAXINFO) { auto* info = reinterpret_cast<MINMAXINFO*>(lp);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);info->ptMinTrackSize={(std::min<int>)(Theme::Scale(500),monitor.rcWork.right-monitor.rcWork.left),(std::min<int>)(Theme::Scale(300),monitor.rcWork.bottom-monitor.rcWork.top)};return 0; }
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
int ChooseNativeItem(HWND owner,const std::wstring& title,const std::vector<std::wstring>& choices,int initial){
    if(choices.empty())return -1;PickState state;WNDCLASSW wc{};wc.lpfnWndProc=PickProc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"DataForgeNativeChoice";RegisterClassW(&wc);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor);const int width=(std::min<int>)(Theme::Scale(540),monitor.rcWork.right-monitor.rcWork.left),height=(std::min<int>)(Theme::Scale(180),monitor.rcWork.bottom-monitor.rcWork.top),gap=Theme::Scale(16),row=Theme::Scale(36);
    HWND window=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME,wc.lpszClassName,title.c_str(),WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,owner,nullptr,wc.hInstance,&state);if(!window)return -1;
    RECT rect{};GetClientRect(window,&rect);state.combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,gap,gap,rect.right-2*gap,Theme::Scale(240),window,reinterpret_cast<HMENU>(10),wc.hInstance,nullptr);for(const auto& choice:choices)SendMessageW(state.combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(choice.c_str()));SendMessageW(state.combo,CB_SETCURSEL,(std::max)(0,(std::min)(initial,static_cast<int>(choices.size())-1)),0);
    CreateWindowExW(0,L"BUTTON",L"Save here",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,rect.right-2*gap-Theme::Scale(220),rect.bottom-gap-row,Theme::Scale(110),row,window,reinterpret_cast<HMENU>(IDOK),wc.hInstance,nullptr);CreateWindowExW(0,L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,rect.right-gap-Theme::Scale(100),rect.bottom-gap-row,Theme::Scale(100),row,window,reinterpret_cast<HMENU>(IDCANCEL),wc.hInstance,nullptr);Theme::ApplyToWindow(window);SendMessageW(state.combo,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),row-Theme::Scale(6));const BOOL enabled=IsWindowEnabled(owner);EnableWindow(owner,FALSE);ShowWindow(window,SW_SHOW);SetFocus(state.combo);MSG message{};while(!state.done){const int result=GetMessageW(&message,nullptr,0,0);if(result<=0){if(result==0)PostQuitMessage(static_cast<int>(message.wParam));break;}if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}DestroyWindow(window);EnableWindow(owner,enabled);SetActiveWindow(owner);return state.selected;
}
bool EditNativeText(HWND owner, const std::wstring& title, std::wstring& text, EditorLanguage language,
    const std::function<std::wstring(const std::wstring&)>& validate,const std::vector<std::wstring>& completions) {
    DialogState state; state.validate = validate;
    WNDCLASSW wc{}; wc.lpfnWndProc = EditorDialogProc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"DataForgeNativeEditorDialog"; RegisterClassW(&wc);
    RECT parent{};GetWindowRect(owner,&parent);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor);
    const int width=(std::min<int>)(Theme::Scale(740),monitor.rcWork.right-monitor.rcWork.left),height=(std::min<int>)(Theme::Scale(500),monitor.rcWork.bottom-monitor.rcWork.top);
    const int left=(std::max)(monitor.rcWork.left,(std::min)(parent.left+(parent.right-parent.left-width)/2,monitor.rcWork.right-width));
    const int top=(std::max)(monitor.rcWork.top,(std::min)(parent.top+(parent.bottom-parent.top-height)/2,monitor.rcWork.bottom-height));
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, wc.lpszClassName, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        left,top,width,height,owner,nullptr,wc.hInstance,&state);
    if (!hwnd) return false;
    state.editor = CodeEditor::Create(hwnd, 1, language);
    state.save = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 32, hwnd, reinterpret_cast<HMENU>(IDOK), wc.hInstance, nullptr);
    state.cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 32, hwnd, reinterpret_cast<HMENU>(IDCANCEL), wc.hInstance, nullptr);
    if (!state.editor || !state.save || !state.cancel) { DestroyWindow(hwnd); return false; }
    CodeEditor::SetCompletions(state.editor,completions);CodeEditor::SetText(state.editor, text); ShowWindow(state.editor, SW_SHOW); Theme::ApplyToWindow(hwnd);
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
