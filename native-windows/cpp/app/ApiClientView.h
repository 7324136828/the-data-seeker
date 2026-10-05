#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include "Types.h"
#include "HttpEngine.h"
#include "StoreManager.h"
#include "Theme.h"
#include "ScriptRuntime.h"

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
    void OpenRequest(const ApiRequest& req);
    void NewDocument();
    void CloseDocument(bool confirm=true);
    void CycleDocument(bool backwards=false);
    size_t DocumentCount() const {return documents_.size();}
    ApiRequest GetCurrentRequest();
    bool IsBusy() const { return isExecuting_; }
    void RequestCancel();
    void Execute() { SendRequestAsync(); }
    void SaveCurrentRequest();
    void ApplyTheme();
    void ImportCollectionFile();
    bool ProcessAuxMessage(MSG& message){return hRunner_&&(message.hwnd==hRunner_||IsChild(hRunner_,message.hwnd))&&IsDialogMessageW(hRunner_,&message);}

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
    HWND hTreeFilter_=nullptr;
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
    HWND hRequestTabs_[9]{};
    HWND hResponseTabs_[4]{};
    HWND hBtnSaveResponse_ = nullptr;
    HWND hSidebarTabs_[2]{};
    HWND hBtnEditConfig_ = nullptr;
    HWND hEditVariables_ = nullptr;
    HWND hEditOptions_ = nullptr;
    HWND hEditPreScript_ = nullptr;
    HWND hEditPostScript_ = nullptr;
    HWND hEditScriptConsole_ = nullptr;
    HWND hDocumentSelector_ = nullptr;
    HWND hBtnNewDocument_ = nullptr;
    HWND hBtnCloseDocument_ = nullptr;

    int requestTab_ = 0; // 0=Params, 1=Headers, 2=Auth, 3=Body, 4=Tests
    bool compactSections_ = false;
    bool compactResponses_=false;
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
    HWND hComboAuthLocation_ = nullptr;
    HWND hComboBodyType_ = nullptr;
    HWND hEditBody_ = nullptr;
    HWND hBtnAttachFile_ = nullptr;
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
    struct RequestDocument {
        std::string identity;
        ApiRequest request;
        ApiResponse response;
        std::wstring bodyDraft,variablesDraft,optionsDraft,preDraft,postDraft;
        std::vector<Variable> scopes;
        AuthConfig inheritedAuth;
        std::string collectionId,folderId;
        std::vector<ScriptEntry> preScripts,postScripts;
        nlohmann::json runtimeVariables=nlohmann::json::object();
        bool initialized=false;
    };
    std::vector<RequestDocument> documents_;
    size_t activeDocument_=0;
    bool activatingDocument_=false;
    void CaptureDocument();
    void LoadFieldsPreservingDrafts(const ApiRequest& request);
    void ActivateDocument(size_t index);
    void RefreshDocumentSelector();
    std::vector<std::wstring> CompletionKeys(const ApiRequest& request,const std::vector<Variable>& scopes,bool useDraft) const;
    void RefreshEditorCompletions();
    bool isExecuting_ = false;
    struct BoundRequest {
        ApiRequest request;
        std::vector<Variable> scopes;
        AuthConfig inheritedAuth;
        std::string collectionId,folderId;
        std::vector<ScriptEntry> preScripts,postScripts;
    };
    std::vector<BoundRequest> treeRequests_;
    std::vector<Variable> selectedScopes_;
    AuthConfig selectedAuth_;
    std::string selectedCollectionId_,selectedFolderId_;
    std::vector<ScriptEntry> selectedPreScripts_,selectedPostScripts_;
    struct TreeNode {std::string id,collectionId,folderId;bool folder=false;};
    std::vector<TreeNode> treeNodes_;
    void TreeMenu();
    void EditTreeNode(UINT command,LPARAM nodeData);
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
        std::string documentIdentity;
        struct ScopeChanges {nlohmann::json changes,local;std::string collectionId,folderId,environmentId;};
        std::vector<ScopeChanges> scopeChanges;
        struct RunRow {std::string name,method,url,error;int status=0;double latency=0;bool passed=false;std::vector<TestResult> tests;std::vector<ScriptConsoleEntry> console;};
        std::mutex rowsMutex;
        std::vector<RunRow> rows;
    };
    struct WorkerJob {std::shared_ptr<Pending> task;std::thread worker;};
    std::vector<std::unique_ptr<WorkerJob>> jobs_;
    HWND hRunner_=nullptr,hRunnerList_=nullptr,hRunnerStatus_=nullptr;
    std::shared_ptr<Pending> runnerTask_;
    void ShowRunner(const std::shared_ptr<Pending>& task);
    void UpdateRunner();
    static LRESULT CALLBACK RunnerProc(HWND,UINT,WPARAM,LPARAM);
    void LaunchTask(const std::shared_ptr<Pending>& task,std::function<void()> work);
    void UpdateExecutionButtons();
    void PollTask();
    void EditConfiguration();
    void ReplayHistory(size_t index);
    void DisplayResponse(const ApiResponse& response);
    ScriptContext MakeScriptContext(const ApiRequest& request,const std::vector<Variable>& scopes,const std::vector<ScriptEntry>& pre,const std::vector<ScriptEntry>& post) const;
    std::map<std::string, std::string> ResolveVariables(const ApiRequest&, const std::vector<Variable>& scopes);

    void Layout();
    void Draw(HDC hdc);
    void SendRequestAsync();
    void RunCollectionAsync(const std::string& collectionId="",const std::string& folderId="");
    void PopulateCollectionsTree();
    void PopulateHistoryList();
    void UpdateRequestTabsVisibility();
    void UpdateResponseTabsVisibility();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace native_app
