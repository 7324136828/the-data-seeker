#pragma once

#include <windows.h>
#include <string>

namespace native_app {

enum class AppTheme {
    Dark,
    Light
};

struct ThemeColors {
    COLORREF bgPrimary;
    COLORREF bgSecondary;
    COLORREF bgTertiary;
    COLORREF bgHover;
    COLORREF borderColor;
    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF textMuted;
    COLORREF accent;
    COLORREF accentHover;

    COLORREF methodGet;
    COLORREF methodPost;
    COLORREF methodPut;
    COLORREF methodDelete;
    COLORREF methodPatch;
    COLORREF methodOther;

    COLORREF status2xx;
    COLORREF status3xx;
    COLORREF status4xx;
    COLORREF status5xx;
    COLORREF textOnAccent;
};

class Theme {
public:
    static void SetTheme(AppTheme theme);
    static AppTheme GetCurrentTheme();
    static const ThemeColors& Get();

    static HFONT GetMainFont();
    static HFONT GetBoldFont();
    static HFONT GetTitleFont();
    static HFONT GetCodeFont();
    static HFONT GetSmallFont();

    static COLORREF GetMethodColor(const std::string& method);
    static COLORREF GetStatusColor(int statusCode);

    static HBRUSH GetBgPrimaryBrush();
    static HBRUSH GetBgSecondaryBrush();
    static HBRUSH GetBgTertiaryBrush();
    static void SetDpi(UINT dpi);
    static void SetTextScale(int percent);
    static int GetTextScale();
    static int Scale(int logicalPixels);
    static void ApplyToWindow(HWND window);
    static void Shutdown();
    static bool VerifyContrast();

private:
    static AppTheme currentTheme_;
    static ThemeColors colors_;
    static HFONT hFontMain_;
    static HFONT hFontBold_;
    static HFONT hFontTitle_;
    static HFONT hFontCode_;
    static HFONT hFontSmall_;
    static HBRUSH hBrBgPrimary_;
    static HBRUSH hBrBgSecondary_;
    static HBRUSH hBrBgTertiary_;

    static void UpdateBrushes();
    static void UpdateFonts();
};

} // namespace native_app
