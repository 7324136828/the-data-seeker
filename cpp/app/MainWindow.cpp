#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <json.hpp>
#include <stdexcept>
#include <algorithm>
#include "MainWindow.h"
#include "Modals.h"
#include "OpenApiParser.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace native_app {

static constexpr wchar_t MainWindowClassName[] = L"DataForgeStudioMainWindowClass";

[[maybe_unused]] static std::wstring ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &w[0], sz);
    return w;
}

static std::string ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string s(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &s[0], sz, nullptr, nullptr);
    return s;
}

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (hWnd_ && IsWindow(hWnd_)) {
        DestroyWindow(hWnd_);
    }
}

void MainWindow::InitDataDirectory() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData))) {
        std::wstring base = std::wstring(localAppData) + L"\\DataForgeStudio";
        CoTaskMemFree(localAppData);
        CreateDirectoryW(base.c_str(), NULL);
        CreateDirectoryW((base + L"\\data").c_str(), NULL);
        CreateDirectoryW((base + L"\\databases").c_str(), NULL);
        CreateDirectoryW((base + L"\\history").c_str(), NULL);
        CreateDirectoryW((base + L"\\logs").c_str(), NULL);
        CreateDirectoryW((base + L"\\temp").c_str(), NULL);
        dataDir_ = ToUtf8(base);
    } else throw std::runtime_error("Windows could not locate a durable workspace directory.");

    const auto settingsPath=std::filesystem::u8path(dataDir_)/"settings.json";
    if(std::filesystem::exists(settingsPath)) {
        std::ifstream file(settingsPath,std::ios::binary);auto settings=nlohmann::json::parse(file,nullptr,false);
        if(settings.is_object()) {
            if(settings.contains("textScale")&&settings["textScale"].is_number_integer())Theme::SetTextScale(settings["textScale"].get<int>());
            if(settings.contains("theme")&&settings["theme"].is_string())Theme::SetTheme(settings["theme"]=="light"?AppTheme::Light:AppTheme::Dark);
        }
    }

    storeMgr_ = std::make_unique<StoreManager>(dataDir_ + "/data");
    httpEngine_ = std::make_unique<HttpEngine>();
    dbEngine_ = std::make_unique<DbEngine>(dataDir_ + "/databases");
}

bool MainWindow::Create(HINSTANCE hInstance, int nCmdShow, int width, int height) {
    hInst_ = hInstance;
    width_ = width;
    height_ = height;

    InitDataDirectory();

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = MainWindowClassName;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hbrBackground = Theme::GetBgPrimaryBrush();
    wc.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassExW(&wc);

    hWnd_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        MainWindowClassName,
        L"DataForge Studio - Unified API & Database Workbench",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height,
        NULL, NULL, hInstance, this
    );

    if (!hWnd_) return false;

    // Create child views
    Theme::SetDpi(GetDpiForWindow(hWnd_));
    RECT client{};GetClientRect(hWnd_,&client);width_=client.right;height_=client.bottom;
    headerBar_.Create(hWnd_, 0, 0, width_, HeaderHeight(), 1001);
    headerBar_.SetEnvironments(storeMgr_->GetEnvironments());

    apiClientView_ = std::make_unique<ApiClientView>(storeMgr_.get(), httpEngine_.get());
    if(!apiClientView_->Create(hWnd_, 0, HeaderHeight(), width_, height_ - HeaderHeight(), 1002))return false;

    dbStudioView_ = std::make_unique<DbStudioView>(dbEngine_.get());
    if(!dbStudioView_->Create(hWnd_, 0, HeaderHeight(), width_, height_ - HeaderHeight(), 1003))return false;

    HMENU menu=CreateMenu(),view=CreatePopupMenu(),file=CreatePopupMenu();
    AppendMenuW(file,MF_STRING,5001,L"Save request\tCtrl+S");AppendMenuW(file,MF_STRING,5002,L"Exit");
    AppendMenuW(view,MF_STRING,5010,L"API Client\tCtrl+1");AppendMenuW(view,MF_STRING,5011,L"Database Studio\tCtrl+2");
    AppendMenuW(view,MF_SEPARATOR,0,nullptr);AppendMenuW(view,MF_STRING,5012,L"Toggle theme");
    AppendMenuW(view,MF_STRING,5013,L"Larger interface\tCtrl++");AppendMenuW(view,MF_STRING,5014,L"Smaller interface\tCtrl+-");
    AppendMenuW(view,MF_STRING,5015,L"Reset interface size\tCtrl+0");
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"&File");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(view),L"&View");SetMenu(hWnd_,menu);

    SetupCallbacks();
    ApplyTheme();

    SwitchMode(WorkbenchMode::ApiClient);

    ShowWindow(hWnd_, nCmdShow);
    UpdateWindow(hWnd_);

    return true;
}

void MainWindow::SetupCallbacks() {
    // HeaderBar callbacks
    headerBar_.OnModeChanged = [this](WorkbenchMode mode) {
        SwitchMode(mode);
    };

    headerBar_.OnEnvironmentSelected = [this](const std::string& envId) {
        storeMgr_->SetActiveEnvironment(envId);
        if (apiClientView_) apiClientView_->RefreshData();
    };

    headerBar_.OnOpenEnvironments = [this]() {
        ShowEnvironmentsDialog(hWnd_, storeMgr_.get(), [this]() {
            headerBar_.SetEnvironments(storeMgr_->GetEnvironments());
            if (apiClientView_) apiClientView_->RefreshData();
        });
    };

    headerBar_.OnOpenGlobals = [this]() {
        ShowGlobalsDialog(hWnd_, storeMgr_.get(), [this]() {
            if (apiClientView_) apiClientView_->RefreshData();
        });
    };

    headerBar_.OnOpenCurlImport = [this]() {
        ShowCurlImportDialog(hWnd_, [this](const ApiRequest& req) {
            if (apiClientView_) {
                apiClientView_->LoadRequest(req);
                SwitchMode(WorkbenchMode::ApiClient);
                headerBar_.SetMode(WorkbenchMode::ApiClient);
            }
        });
    };

    headerBar_.OnOpenOpenApiImport = [this]() {
        ShowOpenApiImportDialog(hWnd_, storeMgr_.get(), [this]() {
            if (apiClientView_) {
                apiClientView_->RefreshData();
                SwitchMode(WorkbenchMode::ApiClient);
                headerBar_.SetMode(WorkbenchMode::ApiClient);
            }
        });
    };

    headerBar_.OnOpenDocs = [this]() {
        ShowDocsDialog(hWnd_);
    };

    headerBar_.OnThemeChanged = [this](AppTheme theme) {
        Theme::SetTheme(theme);
        ApplyTheme();
        SaveSettings();
    };

    // ApiClientView callbacks
    if (apiClientView_) {
        apiClientView_->OnOpenCodeGen = [this](const ApiRequest& req) {
            ShowCodeGenDialog(hWnd_, req);
        };
    }

    // DbStudioView callbacks
    if (dbStudioView_) {
        dbStudioView_->OnOpenNewDbDialog = [this]() {
            if(dbStudioView_->IsBusy())return;
            ShowNewDatabaseDialog(hWnd_, [this](const std::wstring& dbPath, int samplePreset) {
                std::string pathUtf8 = ToUtf8(dbPath);
                std::string dbName = std::filesystem::u8path(pathUtf8).stem().u8string();
                std::string presetStr = (samplePreset == 1) ? "ecommerce" : (samplePreset == 2 ? "dev_studio" : "blank");
                dbEngine_->CreateDatabase(dbName, presetStr, pathUtf8);

                if (dbStudioView_) dbStudioView_->RefreshData();
                SwitchMode(WorkbenchMode::DatabaseStudio);
                headerBar_.SetMode(WorkbenchMode::DatabaseStudio);
            });
        };
    }
}

void MainWindow::SwitchMode(WorkbenchMode mode) {
    currentMode_ = mode;
    if (mode == WorkbenchMode::ApiClient) {
        if (dbStudioView_) dbStudioView_->Show(false);
        if (apiClientView_) {
            apiClientView_->Show(true);
            apiClientView_->Resize(0, HeaderHeight(), width_, height_ - HeaderHeight());
        }
    } else {
        if (apiClientView_) apiClientView_->Show(false);
        if (dbStudioView_) {
            dbStudioView_->Show(true);
            dbStudioView_->Resize(0, HeaderHeight(), width_, height_ - HeaderHeight());
        }
    }
}

void MainWindow::UpdateLayout(int width, int height) {
    width_ = width;
    height_ = height;
    headerBar_.Resize(0, 0, width, HeaderHeight());
    if (currentMode_ == WorkbenchMode::ApiClient && apiClientView_) {
        apiClientView_->Resize(0, HeaderHeight(), width, height - HeaderHeight());
    } else if (currentMode_ == WorkbenchMode::DatabaseStudio && dbStudioView_) {
        dbStudioView_->Resize(0, HeaderHeight(), width, height - HeaderHeight());
    }
}

void MainWindow::ApplyTheme() {
    Theme::ApplyToWindow(hWnd_);
    if(apiClientView_)apiClientView_->ApplyTheme();
    if(dbStudioView_)dbStudioView_->ApplyTheme();
    UpdateLayout(width_,height_);
}
void MainWindow::SaveSettings() {
    const auto path=std::filesystem::u8path(dataDir_)/"settings.json";
    const auto temporary=std::filesystem::u8path(dataDir_)/"settings.pending.json";
    const nlohmann::json settings={{"schemaVersion",1},{"theme",Theme::GetCurrentTheme()==AppTheme::Dark?"dark":"light"},{"textScale",Theme::GetTextScale()}};
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<settings.dump(2);file.flush();
    if(!file)throw std::runtime_error("Interface settings could not be saved. Check disk space and workspace permissions.");file.close();
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Interface settings could not be replaced safely.");
}
bool MainWindow::ProcessMessage(MSG& message) {
    try {
    if(message.message==WM_KEYDOWN && (GetKeyState(VK_CONTROL)&0x8000) && !closing_) {
        switch(message.wParam) {
        case L'1':headerBar_.SetMode(WorkbenchMode::ApiClient);return true;
        case L'2':headerBar_.SetMode(WorkbenchMode::DatabaseStudio);return true;
        case L'S':if(currentMode_==WorkbenchMode::ApiClient)SendMessageW(hWnd_,WM_COMMAND,5001,0);return true;
        case VK_RETURN:if(currentMode_==WorkbenchMode::ApiClient && apiClientView_)apiClientView_->Execute();else if(dbStudioView_)dbStudioView_->Execute();return true;
        case VK_OEM_PLUS:case VK_ADD:SendMessageW(hWnd_,WM_COMMAND,5013,0);return true;
        case VK_OEM_MINUS:case VK_SUBTRACT:SendMessageW(hWnd_,WM_COMMAND,5014,0);return true;
        case L'0':SendMessageW(hWnd_,WM_COMMAND,5015,0);return true;
        }
    }
    return IsDialogMessageW(hWnd_,&message)!=FALSE;
    } catch(const std::exception&) {MessageBoxW(hWnd_,L"Check the request body, variables, options, and workspace permissions before trying again.",L"DataForge Studio",MB_ICONERROR);return true;}
}

void MainWindow::HandleFileDrop(HDROP drop) {
    struct DropOwner {HDROP handle;~DropOwner(){DragFinish(handle);}} owner{drop};
    if(closing_)return;
    const UINT count=DragQueryFileW(drop,0xFFFFFFFF,nullptr,0);
    if(count!=1)throw std::runtime_error("Drop one SQLite database, SQL script, or OpenAPI JSON file at a time.");
    const UINT length=DragQueryFileW(drop,0,nullptr,0);
    std::wstring name(static_cast<size_t>(length)+1,L'\0');DragQueryFileW(drop,0,name.data(),length+1);name.resize(length);
    const std::filesystem::path path(name);
    if(!std::filesystem::is_regular_file(path))throw std::runtime_error("The dropped item must be an accessible regular file.");
    const auto extension=path.extension().wstring();
    if(_wcsicmp(extension.c_str(),L".db")==0 || _wcsicmp(extension.c_str(),L".sqlite")==0 || _wcsicmp(extension.c_str(),L".sqlite3")==0) {
        if(dbStudioView_ && dbStudioView_->IsBusy())throw std::runtime_error("Finish or cancel the current database operation before attaching another file.");
        dbEngine_->AttachDatabase(path.stem().u8string(),path.u8string());
        dbStudioView_->RefreshData();headerBar_.SetMode(WorkbenchMode::DatabaseStudio);return;
    }
    if(std::filesystem::file_size(path)>16*1024*1024)throw std::runtime_error("SQL and JSON imports are limited to 16 MiB.");
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("The dropped file could not be opened.");
    std::string content((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    if(file.bad())throw std::runtime_error("The dropped file could not be read completely.");
    if(!content.empty() && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,content.data(),static_cast<int>(content.size()),nullptr,0)==0)throw std::runtime_error("Text imports must be encoded as UTF-8.");
    if(_wcsicmp(extension.c_str(),L".sql")==0) {
        dbStudioView_->LoadSql(content);headerBar_.SetMode(WorkbenchMode::DatabaseStudio);return;
    }
    if(_wcsicmp(extension.c_str(),L".json")==0) {
        auto collection=OpenApiParser::Parse(content);
        if(collection.requests.empty() && collection.folders.empty())throw std::runtime_error("This JSON file has no importable OpenAPI/Swagger requests.");
        auto collections=storeMgr_->GetCollections();collections.push_back(collection);storeMgr_->SaveCollections(collections);
        apiClientView_->RefreshData();headerBar_.SetMode(WorkbenchMode::ApiClient);return;
    }
    throw std::runtime_error("Supported drops: SQLite (.db/.sqlite/.sqlite3), UTF-8 SQL (.sql), and OpenAPI/Swagger JSON (.json).");
}

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    try {
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->hWnd_ = hWnd;
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    case WM_SIZE: {
        if (pThis) {
            pThis->UpdateLayout(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(lParam);info->ptMinTrackSize={Theme::Scale(1040),Theme::Scale(650)};return 0;
    }
    case WM_DPICHANGED: {
        Theme::SetDpi(HIWORD(wParam));auto* rect=reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hWnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
        if(pThis)pThis->ApplyTheme();return 0;
    }
    case WM_COMMAND: {
        if(!pThis || pThis->closing_)return 0;
        switch(LOWORD(wParam)) {
        case 5001:if(pThis->currentMode_==WorkbenchMode::ApiClient && pThis->apiClientView_)pThis->apiClientView_->SaveCurrentRequest();break;
        case 5002:SendMessageW(hWnd,WM_CLOSE,0,0);break;
        case 5010:pThis->headerBar_.SetMode(WorkbenchMode::ApiClient);break;
        case 5011:pThis->headerBar_.SetMode(WorkbenchMode::DatabaseStudio);break;
        case 5012:Theme::SetTheme(Theme::GetCurrentTheme()==AppTheme::Dark?AppTheme::Light:AppTheme::Dark);pThis->ApplyTheme();pThis->SaveSettings();break;
        case 5013:case 5014:case 5015:Theme::SetTextScale(LOWORD(wParam)==5015?100:Theme::GetTextScale()+(LOWORD(wParam)==5013?10:-10));pThis->ApplyTheme();pThis->SaveSettings();break;
        }
        return 0;
    }
    case WM_CLOSE: {
        if(!pThis)return 0;
        if((pThis->apiClientView_ && pThis->apiClientView_->IsBusy()) || (pThis->dbStudioView_ && pThis->dbStudioView_->IsBusy())) {
            pThis->closing_=true;SetPropW(hWnd,L"DataForge.Closing",reinterpret_cast<HANDLE>(1));
            if(pThis->apiClientView_)pThis->apiClientView_->RequestCancel();if(pThis->dbStudioView_)pThis->dbStudioView_->RequestCancel();
            EnableWindow(pThis->headerBar_.GetHwnd(),FALSE);SetWindowTextW(hWnd,L"DataForge Studio - cancelling active work...");SetTimer(hWnd,88,50,nullptr);return 0;
        }
        DestroyWindow(hWnd);return 0;
    }
    case WM_TIMER: {
        if(pThis && wParam==88 && (!pThis->apiClientView_ || !pThis->apiClientView_->IsBusy()) && (!pThis->dbStudioView_ || !pThis->dbStudioView_->IsBusy())){KillTimer(hWnd,88);DestroyWindow(hWnd);}return 0;
    }
    case WM_DROPFILES: {
        if (pThis) {
            pThis->HandleFileDrop(reinterpret_cast<HDROP>(wParam));
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // Double buffered/handled by children
    case WM_DESTROY: {
        PostQuitMessage(0);
        return 0;
    }
    }
    }catch(const std::exception&) {MessageBoxW(hWnd,L"The operation could not complete. Check the input format, disk space, and workspace permissions.",L"DataForge Studio",MB_ICONERROR);return 0;}
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace native_app
