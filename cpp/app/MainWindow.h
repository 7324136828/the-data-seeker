#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <memory>
#include "Types.h"
#include "Theme.h"
#include "HeaderBar.h"
#include "ApiClientView.h"
#include "DbStudioView.h"
#include "StoreManager.h"
#include "HttpEngine.h"
#include "DbEngine.h"

namespace native_app {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, int nCmdShow, int width = 1280, int height = 800);
    HWND GetHwnd() const { return hWnd_; }

    void SwitchMode(WorkbenchMode mode);
    void UpdateLayout(int width, int height);
    void HandleFileDrop(HDROP hDrop);
    void ApplyTheme();
    void SaveSettings();
    bool ProcessMessage(MSG& message);
    int HeaderHeight() const { return Theme::Scale(56); }

private:
    HWND hWnd_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    int width_ = 1280;
    int height_ = 800;
    bool closing_ = false;

    std::string dataDir_;
    std::unique_ptr<StoreManager> storeMgr_;
    std::unique_ptr<HttpEngine> httpEngine_;
    std::unique_ptr<DbEngine> dbEngine_;

    HeaderBar headerBar_;
    std::unique_ptr<ApiClientView> apiClientView_;
    std::unique_ptr<DbStudioView> dbStudioView_;
    WorkbenchMode currentMode_ = WorkbenchMode::ApiClient;

    void InitDataDirectory();
    void SetupCallbacks();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace native_app
