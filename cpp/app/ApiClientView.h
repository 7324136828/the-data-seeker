#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include "Types.h"
#include "HttpEngine.h"
#include "StoreManager.h"
#include "Theme.h"

namespace native_app {

class ApiClientView {
public:
    ApiClientView(StoreManager* storeMgr, HttpEngine* httpEngine);
    ~ApiClientView();

    bool Create(HWND hParent, int x, int y, int width, int height, UINT id);
    void Resize(int x, int y, int width, int height);
    void Show(bool bShow);
    void RefreshData();

    void LoadRequest(const ApiRequest& req);
    ApiRequest GetCurrentRequest();
    bool IsBusy() const { return isExecuting_; }
    void RequestCancel();
    void Execute() { SendRequestAsync(); }
    void SaveCurrentRequest();
    void ApplyTheme();

    std::function<void(const std::string&)> OnToast;
    std::function<void(const ApiRequest&)> OnOpenCodeGen;

private:
    HWND hWnd_ = nullptr;
    HWND hParent_ = nullptr;
    StoreManager* storeMgr_ = nullptr;
    HttpEngine* httpEngine_ = nullptr;

    int width_ = 0;
    int height_ = 0;

    // Sidebar controls
    int sidebarWidth_ = 280;
    int sidebarTab_ = 0; // 0 = Collections, 1 = History
    RECT rcSidebarTabCol_{};
    RECT rcSidebarTabHist_{};
    HWND hTreeCollections_ = nullptr;
    HWND hListHistory_ = nullptr;
    HWND hBtnAddCol_ = nullptr;
    HWND hBtnAddReq_ = nullptr;
    HWND hBtnRunAll_ = nullptr;
    HWND hBtnClearHist_ = nullptr;

    // Request builder controls
    HWND hComboMethod_ = nullptr;
    HWND hEditUrl_ = nullptr;
    HWND hBtnSend_ = nullptr;
    HWND hBtnCode_ = nullptr;
    HWND hEditName_ = nullptr;
    HWND hBtnSave_ = nullptr;
    HWND hRequestTabs_[7]{};
    HWND hResponseTabs_[3]{};
    HWND hSidebarTabs_[2]{};
    HWND hBtnEditConfig_ = nullptr;
    HWND hEditVariables_ = nullptr;
    HWND hEditOptions_ = nullptr;

    int requestTab_ = 0; // 0=Params, 1=Headers, 2=Auth, 3=Body, 4=Tests
    RECT rcReqTabParams_{};
    RECT rcReqTabHeaders_{};
    RECT rcReqTabAuth_{};
    RECT rcReqTabBody_{};
    RECT rcReqTabTests_{};

    // Sub-tab controls
    HWND hListParams_ = nullptr;
    HWND hListHeaders_ = nullptr;
    HWND hComboAuthType_ = nullptr;
    HWND hEditAuthToken_ = nullptr;
    HWND hEditAuthUser_ = nullptr;
    HWND hEditAuthPass_ = nullptr;
    HWND hEditAuthKey_ = nullptr;
    HWND hEditAuthVal_ = nullptr;
    HWND hComboBodyType_ = nullptr;
    HWND hEditBody_ = nullptr;
    HWND hListTests_ = nullptr;
    HWND hBtnAddTest_ = nullptr;

    // Response viewer controls
    int responseTab_ = 0; // 0=Body, 1=Headers, 2=Test Results
    RECT rcRespTabBody_{};
    RECT rcRespTabHeaders_{};
    RECT rcRespTabTests_{};
    HWND hEditRespBody_ = nullptr;
    HWND hListRespHeaders_ = nullptr;
    HWND hListRespTests_ = nullptr;

    ApiResponse lastResponse_;
    ApiRequest currentRequest_;
    bool isExecuting_ = false;
    struct BoundRequest {
        ApiRequest request;
        std::vector<Variable> scopes;
        AuthConfig inheritedAuth;
    };
    std::vector<BoundRequest> treeRequests_;
    std::vector<Variable> selectedScopes_;
    AuthConfig selectedAuth_;
    struct Pending {
        std::atomic_bool done{false};
        std::atomic_bool cancelled{false};
        std::atomic_int completed{0};
        bool runner = false;
        ApiResponse response;
        std::vector<HistoryEntry> history;
        int total = 0;
        int passed = 0;
        std::string error;
    };
    std::shared_ptr<Pending> pending_;
    std::thread worker_;
    void PollTask();
    void EditConfiguration();
    std::map<std::string, std::string> ResolveVariables(const ApiRequest&, const std::vector<Variable>& scopes);

    void Layout();
    void Draw(HDC hdc);
    void SendRequestAsync();
    void RunCollectionAsync();
    void PopulateCollectionsTree();
    void PopulateHistoryList();
    void UpdateRequestTabsVisibility();
    void UpdateResponseTabsVisibility();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace native_app
