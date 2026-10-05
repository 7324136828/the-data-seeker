#include "Theme.h"
#include <algorithm>

namespace native_app {

AppTheme Theme::currentTheme_ = AppTheme::Dark;
ThemeColors Theme::colors_{};
HFONT Theme::hFontMain_ = nullptr;
HFONT Theme::hFontBold_ = nullptr;
HFONT Theme::hFontTitle_ = nullptr;
HFONT Theme::hFontCode_ = nullptr;
HFONT Theme::hFontSmall_ = nullptr;
HBRUSH Theme::hBrBgPrimary_ = nullptr;
HBRUSH Theme::hBrBgSecondary_ = nullptr;
HBRUSH Theme::hBrBgTertiary_ = nullptr;

void Theme::UpdateBrushes() {
    if (hBrBgPrimary_) DeleteObject(hBrBgPrimary_);
    if (hBrBgSecondary_) DeleteObject(hBrBgSecondary_);
    if (hBrBgTertiary_) DeleteObject(hBrBgTertiary_);

    hBrBgPrimary_ = CreateSolidBrush(colors_.bgPrimary);
    hBrBgSecondary_ = CreateSolidBrush(colors_.bgSecondary);
    hBrBgTertiary_ = CreateSolidBrush(colors_.bgTertiary);
}

void Theme::SetTheme(AppTheme theme) {
    currentTheme_ = theme;

    if (theme == AppTheme::Dark) {
        colors_.bgPrimary = RGB(15, 23, 42);       // #0f172a
        colors_.bgSecondary = RGB(30, 41, 59);     // #1e293b
        colors_.bgTertiary = RGB(51, 65, 85);      // #334155
        colors_.bgHover = RGB(39, 53, 73);         // #273549
        colors_.borderColor = RGB(51, 65, 85);     // #334155
        colors_.textPrimary = RGB(248, 250, 252);  // #f8fafc
        colors_.textSecondary = RGB(148, 163, 184);// #94a3b8
        colors_.textMuted = RGB(100, 116, 139);    // #64748b
        colors_.accent = RGB(59, 130, 246);        // #3b82f6
        colors_.accentHover = RGB(37, 99, 235);    // #2563eb
    } else {
        colors_.bgPrimary = RGB(248, 250, 252);    // #f8fafc
        colors_.bgSecondary = RGB(255, 255, 255);  // #ffffff
        colors_.bgTertiary = RGB(241, 245, 249);   // #f1f5f9
        colors_.bgHover = RGB(226, 232, 240);      // #e2e8f0
        colors_.borderColor = RGB(226, 232, 240);  // #e2e8f0
        colors_.textPrimary = RGB(15, 23, 42);     // #0f172a
        colors_.textSecondary = RGB(71, 85, 105);  // #475569
        colors_.textMuted = RGB(148, 163, 184);    // #94a3b8
        colors_.accent = RGB(37, 99, 235);         // #2563eb
        colors_.accentHover = RGB(29, 78, 216);    // #1d4ed8
    }

    colors_.methodGet = RGB(16, 185, 129);     // #10b981
    colors_.methodPost = RGB(245, 158, 11);    // #f59e0b
    colors_.methodPut = RGB(59, 130, 246);     // #3b82f6
    colors_.methodDelete = RGB(239, 68, 68);   // #ef4444
    colors_.methodPatch = RGB(139, 92, 246);   // #8b5cf6
    colors_.methodOther = RGB(156, 163, 175);  // #9ca3af

    colors_.status2xx = RGB(16, 185, 129);
    colors_.status3xx = RGB(59, 130, 246);
    colors_.status4xx = RGB(245, 158, 11);
    colors_.status5xx = RGB(239, 68, 68);

    UpdateBrushes();

    if (!hFontMain_) {
        hFontMain_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        hFontBold_ = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        hFontTitle_ = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        hFontCode_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        hFontSmall_ = CreateFontW(-11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
}

AppTheme Theme::GetCurrentTheme() {
    return currentTheme_;
}

const ThemeColors& Theme::Get() {
    if (!hBrBgPrimary_) SetTheme(AppTheme::Dark);
    return colors_;
}

HFONT Theme::GetMainFont() {
    Get();
    return hFontMain_;
}

HFONT Theme::GetBoldFont() {
    Get();
    return hFontBold_;
}

HFONT Theme::GetTitleFont() {
    Get();
    return hFontTitle_;
}

HFONT Theme::GetCodeFont() {
    Get();
    return hFontCode_;
}

HFONT Theme::GetSmallFont() {
    Get();
    return hFontSmall_;
}

HBRUSH Theme::GetBgPrimaryBrush() {
    Get();
    return hBrBgPrimary_;
}

HBRUSH Theme::GetBgSecondaryBrush() {
    Get();
    return hBrBgSecondary_;
}

HBRUSH Theme::GetBgTertiaryBrush() {
    Get();
    return hBrBgTertiary_;
}

COLORREF Theme::GetMethodColor(const std::string& method) {
    std::string m = method;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
    if (m == "GET") return colors_.methodGet;
    if (m == "POST") return colors_.methodPost;
    if (m == "PUT") return colors_.methodPut;
    if (m == "DELETE") return colors_.methodDelete;
    if (m == "PATCH") return colors_.methodPatch;
    return colors_.methodOther;
}

COLORREF Theme::GetStatusColor(int statusCode) {
    if (statusCode >= 200 && statusCode < 300) return colors_.status2xx;
    if (statusCode >= 300 && statusCode < 400) return colors_.status3xx;
    if (statusCode >= 400 && statusCode < 500) return colors_.status4xx;
    return colors_.status5xx;
}

} // namespace native_app
