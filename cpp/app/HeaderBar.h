#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "Types.h"
#include "Theme.h"

namespace native_app {

enum class WorkbenchMode {
    ApiClient,
    DatabaseStudio
};

class HeaderBar {
public:
    HeaderBar() = default;
    ~HeaderBar();

    bool Create(HWND hParent, int x, int y, int width, int height, UINT id);
    void Resize(int x, int y, int width, int height);

    void SetMode(WorkbenchMode mode);
    WorkbenchMode GetMode() const { return mode_; }

    void SetEnvironments(const EnvironmentStore& envStore);
    std::string GetSelectedEnvironmentId() const;

    // Callbacks
    std::function<void(WorkbenchMode)> OnModeChanged;
    std::function<void(const std::string&)> OnEnvironmentSelected;
    std::function<void()> OnOpenEnvironments;
    std::function<void()> OnOpenGlobals;
    std::function<void()> OnOpenCurlImport;
    std::function<void()> OnOpenOpenApiImport;
    std::function<void()> OnOpenDocs;
    std::function<void(AppTheme)> OnThemeChanged;

    HWND GetHwnd() const { return hWnd_; }

private:
    HWND hWnd_ = nullptr;
    HWND hParent_ = nullptr;
    HWND hEnvCombo_ = nullptr;
    HWND hButtons_[8]{};
    WorkbenchMode mode_ = WorkbenchMode::ApiClient;
    EnvironmentStore envStore_;
    int width_ = 0;
    int height_ = 52;

    RECT rcLogo_{};
    RECT rcBtnApi_{};
    RECT rcBtnDb_{};
    RECT rcBtnEnv_{};
    RECT rcBtnGlobals_{};
    RECT rcBtnCurl_{};
    RECT rcBtnOpenApi_{};
    RECT rcBtnDocs_{};
    RECT rcBtnTheme_{};

    int hoveredBtn_ = -1;

    void Layout();
    void Draw(HDC hdc);
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace native_app
