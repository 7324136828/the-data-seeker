#include "HeaderBar.h"
#include <windowsx.h>

namespace native_app {

static constexpr wchar_t HeaderBarClassName[] = L"DataForgeHeaderBarClass";

HeaderBar::~HeaderBar() {
    if (hWnd_ && IsWindow(hWnd_)) {
        DestroyWindow(hWnd_);
    }
}

bool HeaderBar::Create(HWND hParent, int x, int y, int width, int height, UINT id) {
    hParent_ = hParent;
    width_ = width;
    height_ = height;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = HeaderBarClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT, HeaderBarClassName, L"Workbench navigation",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        x, y, width, height,
        hParent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        GetModuleHandle(nullptr), this
    );

    if (!hWnd_) return false;

    // Create environment dropdown
    hEnvCombo_ = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 150, 200,
        hWnd_, reinterpret_cast<HMENU>(1001),
        GetModuleHandle(nullptr), nullptr
    );
    SendMessage(hEnvCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    const wchar_t* labels[] = {L"API Client", L"Database Studio", L"Environments", L"Globals", L"Import cURL", L"Import OpenAPI", L"Help", L"Light theme"};
    for (int i = 0; i < 8; ++i) hButtons_[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 32, hWnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1101 + i)), GetModuleHandleW(nullptr), nullptr);
    SetPropW(hButtons_[0], L"DataForge.Active", reinterpret_cast<HANDLE>(1));
    Theme::ApplyToWindow(hWnd_);

    Layout();
    return true;
}

void HeaderBar::Resize(int x, int y, int width, int height) {
    width_ = width;
    height_ = height;
    SetWindowPos(hWnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    InvalidateRect(hWnd_, nullptr, TRUE);
}

void HeaderBar::SetMode(WorkbenchMode mode) {
    for (int i = 0; i < 2; ++i) { RemovePropW(hButtons_[i], L"DataForge.Active"); InvalidateRect(hButtons_[i], nullptr, FALSE); }
    SetPropW(hButtons_[mode == WorkbenchMode::ApiClient ? 0 : 1], L"DataForge.Active", reinterpret_cast<HANDLE>(1));
    if (mode_ != mode) {
        mode_ = mode;
        InvalidateRect(hWnd_, nullptr, TRUE);
        if (OnModeChanged) OnModeChanged(mode_);
    }
}

void HeaderBar::SetEnvironments(const EnvironmentStore& envStore) {
    envStore_ = envStore;
    SendMessage(hEnvCombo_, CB_RESETCONTENT, 0, 0);

    int activeIdx = 0;
    SendMessage(hEnvCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No Environment"));

    for (size_t i = 0; i < envStore_.environments.size(); ++i) {
        const auto& env = envStore_.environments[i];
        int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, env.name.data(), static_cast<int>(env.name.size()), nullptr, 0);
        std::wstring wName(sizeNeeded, 0);
        MultiByteToWideChar(CP_UTF8, 0, env.name.data(), static_cast<int>(env.name.size()), &wName[0], sizeNeeded);

        int idx = static_cast<int>(SendMessage(hEnvCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wName.c_str())));
        if (env.id == envStore_.activeId) {
            activeIdx = idx;
        }
    }

    SendMessage(hEnvCombo_, CB_SETCURSEL, activeIdx, 0);
}

std::string HeaderBar::GetSelectedEnvironmentId() const {
    int curSel = static_cast<int>(SendMessage(hEnvCombo_, CB_GETCURSEL, 0, 0));
    if (curSel <= 0 || curSel - 1 >= static_cast<int>(envStore_.environments.size())) {
        return "";
    }
    return envStore_.environments[curSel - 1].id;
}

void HeaderBar::Layout() {
    auto s = [](int v) { return Theme::Scale(v); };
    compact_=width_<s(900);
    rcLogo_ = {s(16), s(8), s(265), s(48)};
    rcBtnApi_ = {s(280), s(10), s(400), s(44)};
    rcBtnDb_ = {s(410), s(10), s(590), s(44)};
    rcBtnEnv_ = {s(226), s(60), s(380), s(94)};
    rcBtnGlobals_ = {s(390), s(60), s(480), s(94)};
    rcBtnCurl_ = {s(490), s(60), s(622), s(94)};
    rcBtnOpenApi_ = {s(632), s(60), s(802), s(94)};
    rcBtnDocs_ = {width_ - s(230), s(10), width_ - s(160), s(44)};
    rcBtnTheme_ = {width_ - s(150), s(10), width_ - s(16), s(44)};
    if(compact_){
        rcLogo_={s(16),s(4),width_*30/100,s(34)};
        const int start=rcLogo_.right+s(12),end=width_-s(116),available=(std::max)(1,end-start-s(8));
        rcBtnApi_={start,s(4),start+available*42/100,s(28)};
        rcBtnDb_={rcBtnApi_.right+s(8),s(4),end,s(28)};
        rcBtnDocs_={width_-s(106),s(4),width_-s(16),s(28)};
        const int envWidth=(std::min)(s(200),width_/3);
        rcBtnEnv_={s(26)+envWidth,s(40),s(26)+envWidth+s(154),s(64)};
        rcBtnGlobals_={rcBtnEnv_.right+s(10),s(40),rcBtnEnv_.right+s(100),s(64)};
        rcBtnCurl_=rcBtnOpenApi_=rcBtnTheme_={};
    }
    RECT rects[] = {rcBtnApi_, rcBtnDb_, rcBtnEnv_, rcBtnGlobals_, rcBtnCurl_, rcBtnOpenApi_, rcBtnDocs_, rcBtnTheme_};
    for (int i = 0; i < 8; ++i) SetWindowPos(hButtons_[i], nullptr, rects[i].left, rects[i].top, rects[i].right - rects[i].left, rects[i].bottom - rects[i].top, SWP_NOZORDER | SWP_NOACTIVATE);
    for(int i=0;i<8;++i)ShowWindow(hButtons_[i],compact_&&(i==4||i==5||i==7)?SW_HIDE:SW_SHOW);
    SetWindowTextW(hButtons_[1],compact_?L"Database":L"Database Studio");SetWindowTextW(hButtons_[6],compact_?L"More \x25BE":L"Help");
    SetWindowTextW(hButtons_[7], Theme::GetCurrentTheme() == AppTheme::Dark ? L"Light theme" : L"Dark theme");
    SetWindowPos(hEnvCombo_, nullptr, s(16), s(compact_?40:60),compact_?(std::min)(s(200),width_/3):s(200), s(300), SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(hEnvCombo_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), s(compact_?18:26));
}

static void DrawRoundedButton(HDC hdc, const RECT& rc, const wchar_t* text, bool isActive, bool isHovered, const ThemeColors& tc, HFONT font) {
    COLORREF bg = isActive ? tc.accent : isHovered ? tc.bgHover : tc.bgSecondary;
    COLORREF fg = isActive ? tc.textOnAccent : isHovered ? tc.textPrimary : tc.textSecondary;
    COLORREF border = isActive ? tc.accentHover : tc.borderColor;

    HBRUSH hBr = CreateSolidBrush(bg);
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(hdc, hBr);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(hPen);
    DeleteObject(hBr);

    SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRc = rc;
    DrawTextW(hdc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void HeaderBar::Draw(HDC hdc) {
    const auto& tc = Theme::Get();

    RECT clientRc{ 0, 0, width_, height_ };
    FillRect(hdc, &clientRc, Theme::GetBgSecondaryBrush());

    // Bottom border
    HPEN hBorderPen = CreatePen(PS_SOLID, 1, tc.borderColor);
    HGDIOBJ oldPen = SelectObject(hdc, hBorderPen);
    MoveToEx(hdc, 0, height_ - 1, nullptr);
    LineTo(hdc, width_, height_ - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(hBorderPen);

    // Draw Logo & Name
    SelectObject(hdc, compact_?Theme::GetBoldFont():Theme::GetTitleFont());
    SetTextColor(hdc, tc.textPrimary);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, L"DataForge Studio", -1, &rcLogo_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Mode Buttons
    DrawRoundedButton(hdc, rcBtnApi_, L"API Client", mode_ == WorkbenchMode::ApiClient, hoveredBtn_ == 1, tc, Theme::GetBoldFont());
    DrawRoundedButton(hdc, rcBtnDb_, L"Database Studio", mode_ == WorkbenchMode::DatabaseStudio, hoveredBtn_ == 2, tc, Theme::GetBoldFont());

    // Right Action Buttons
    DrawRoundedButton(hdc, rcBtnEnv_, L"Environments", false, hoveredBtn_ == 3, tc, Theme::GetMainFont());
    DrawRoundedButton(hdc, rcBtnGlobals_, L"Globals", false, hoveredBtn_ == 4, tc, Theme::GetMainFont());
    if(!compact_){DrawRoundedButton(hdc, rcBtnCurl_, L"Import cURL", false, hoveredBtn_ == 5, tc, Theme::GetMainFont());
    DrawRoundedButton(hdc, rcBtnOpenApi_, L"Import OpenAPI", false, hoveredBtn_ == 6, tc, Theme::GetMainFont());}
    DrawRoundedButton(hdc, rcBtnDocs_,compact_?L"More":L"Help", false, hoveredBtn_ == 7, tc, Theme::GetMainFont());

    // Theme toggle button
    const wchar_t* themeText = (Theme::GetCurrentTheme() == AppTheme::Dark) ? L"\x2600" : L"\x263D"; // Sun or Moon
    if(!compact_)DrawRoundedButton(hdc, rcBtnTheme_, themeText, false, hoveredBtn_ == 8, tc, Theme::GetTitleFont());
}

LRESULT CALLBACK HeaderBar::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HeaderBar* pThis = reinterpret_cast<HeaderBar*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<HeaderBar*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    case WM_PRINTCLIENT: if(pThis)pThis->Draw(reinterpret_cast<HDC>(wParam));return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBm = SelectObject(memDC, memBm);

        if (pThis) pThis->Draw(memDC);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBm);
        DeleteObject(memBm);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!pThis) break;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int prev = pThis->hoveredBtn_;
        pThis->hoveredBtn_ = -1;
        if (PtInRect(&pThis->rcBtnApi_, pt)) pThis->hoveredBtn_ = 1;
        else if (PtInRect(&pThis->rcBtnDb_, pt)) pThis->hoveredBtn_ = 2;
        else if (PtInRect(&pThis->rcBtnEnv_, pt)) pThis->hoveredBtn_ = 3;
        else if (PtInRect(&pThis->rcBtnGlobals_, pt)) pThis->hoveredBtn_ = 4;
        else if (PtInRect(&pThis->rcBtnCurl_, pt)) pThis->hoveredBtn_ = 5;
        else if (PtInRect(&pThis->rcBtnOpenApi_, pt)) pThis->hoveredBtn_ = 6;
        else if (PtInRect(&pThis->rcBtnDocs_, pt)) pThis->hoveredBtn_ = 7;
        else if (PtInRect(&pThis->rcBtnTheme_, pt)) pThis->hoveredBtn_ = 8;

        if (prev != pThis->hoveredBtn_) InvalidateRect(hWnd, nullptr, FALSE);

        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (pThis && pThis->hoveredBtn_ != -1) {
            pThis->hoveredBtn_ = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!pThis) break;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&pThis->rcBtnApi_, pt)) {
            pThis->SetMode(WorkbenchMode::ApiClient);
        } else if (PtInRect(&pThis->rcBtnDb_, pt)) {
            pThis->SetMode(WorkbenchMode::DatabaseStudio);
        } else if (PtInRect(&pThis->rcBtnEnv_, pt)) {
            if (pThis->OnOpenEnvironments) pThis->OnOpenEnvironments();
        } else if (PtInRect(&pThis->rcBtnGlobals_, pt)) {
            if (pThis->OnOpenGlobals) pThis->OnOpenGlobals();
        } else if (PtInRect(&pThis->rcBtnCurl_, pt)) {
            if (pThis->OnOpenCurlImport) pThis->OnOpenCurlImport();
        } else if (PtInRect(&pThis->rcBtnOpenApi_, pt)) {
            if (pThis->OnOpenOpenApiImport) pThis->OnOpenOpenApiImport();
        } else if (PtInRect(&pThis->rcBtnDocs_, pt)) {
            if (pThis->OnOpenDocs) pThis->OnOpenDocs();
        } else if (PtInRect(&pThis->rcBtnTheme_, pt)) {
            AppTheme nextTheme = (Theme::GetCurrentTheme() == AppTheme::Dark) ? AppTheme::Light : AppTheme::Dark;
            Theme::SetTheme(nextTheme);
            if (pThis->OnThemeChanged) pThis->OnThemeChanged(nextTheme);
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_COMMAND: {
        if (pThis && HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case 1101: pThis->SetMode(WorkbenchMode::ApiClient); break;
            case 1102: pThis->SetMode(WorkbenchMode::DatabaseStudio); break;
            case 1103: if (pThis->OnOpenEnvironments) pThis->OnOpenEnvironments(); break;
            case 1104: if (pThis->OnOpenGlobals) pThis->OnOpenGlobals(); break;
            case 1105: if (pThis->OnOpenCurlImport) pThis->OnOpenCurlImport(); break;
            case 1106: if (pThis->OnOpenOpenApiImport) pThis->OnOpenOpenApiImport(); break;
            case 1107:
                if(pThis->compact_){
                    HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1105,L"Import cURL");AppendMenuW(menu,MF_STRING,1106,L"Import OpenAPI");AppendMenuW(menu,MF_STRING,1117,L"Help");AppendMenuW(menu,MF_STRING,1108,Theme::GetCurrentTheme()==AppTheme::Dark?L"Light theme":L"Dark theme");
                    RECT rect{};GetWindowRect(pThis->hButtons_[6],&rect);const UINT selection=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTALIGN,rect.right,rect.bottom,0,hWnd,nullptr);DestroyMenu(menu);if(selection)SendMessageW(hWnd,WM_COMMAND,MAKEWPARAM(selection,BN_CLICKED),0);
                }else if(pThis->OnOpenDocs)pThis->OnOpenDocs();break;
            case 1117: if(pThis->OnOpenDocs)pThis->OnOpenDocs();break;
            case 1108: if (pThis->OnThemeChanged) pThis->OnThemeChanged(Theme::GetCurrentTheme() == AppTheme::Dark ? AppTheme::Light : AppTheme::Dark); pThis->Layout(); break;
            }
        }
        if (LOWORD(wParam) == 1001 && HIWORD(wParam) == CBN_SELCHANGE) {
            if (pThis && pThis->OnEnvironmentSelected) {
                pThis->OnEnvironmentSelected(pThis->GetSelectedEnvironmentId());
            }
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const auto& tc = Theme::Get();
        SetTextColor(hdc, tc.textPrimary);
        SetBkColor(hdc, tc.bgSecondary);
        return reinterpret_cast<LRESULT>(Theme::GetBgSecondaryBrush());
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace native_app
