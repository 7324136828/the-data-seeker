#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <thread>
#include "Types.h"
#include "DbEngine.h"
#include "Theme.h"
#include "CodeEditor.h"

namespace native_app {

class NativeMockServer;

class DbStudioView {
public:
    explicit DbStudioView(DbEngine* dbEngine);
    ~DbStudioView();

    bool Create(HWND hParent, int x, int y, int width, int height, UINT id);
    void Resize(int x, int y, int width, int height);
    void Show(bool bShow);
    void RefreshData();
    void ApplyTheme();
    bool IsBusy() const;
    void RequestCancel();
    void LoadSql(const std::string& sql);
    void Execute() { RunSqlQuery(); }
    void NewDocument();
    void CloseDocument(bool confirm = true);
    void CycleDocument(bool backwards = false);
    void ShowMockServer();
    HWND GetHwnd() const { return hWnd_; }
    static bool RunSelfTests(std::wstring& failure);

    std::function<void(const std::string&)> OnToast;
    std::function<void()> OnOpenNewDbDialog;

private:
    HWND hWnd_ = nullptr;
    HWND hParent_ = nullptr;
    DbEngine* dbEngine_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    bool layoutInProgress_ = false;

    int sidebarWidth_ = 280;
    std::string activeDbId_ = "ecommerce";
    std::string activeTable_;

    // Sidebar controls
    HWND hListDatabases_ = nullptr;
    HWND hBtnNewDb_ = nullptr;
    HWND hListTables_ = nullptr;
    HWND hEditObjectFilter_ = nullptr;
    HWND hBtnConnections_ = nullptr;

    // Workbench tabs
    int workbenchTab_ = 0; // 0=SQL Console, 1=Data Grid, 2=Schema Viewer, 3=ER Diagram
    RECT rcTabSql_{};
    RECT rcTabDataGrid_{};
    RECT rcTabSchema_{};
    RECT rcTabEr_{};

    // SQL Console controls
    HWND hEditSql_ = nullptr;
    HWND hBtnRunSql_ = nullptr;
    HWND hBtnFormatSql_ = nullptr;
    HWND hBtnExplain_ = nullptr;
    HWND hComboTemplates_ = nullptr;
    HWND hListSqlResults_ = nullptr;
    HWND hSqlStatus_ = nullptr;
    HWND hBtnSqlExport_ = nullptr;
    HWND hTabs_[4]{};
    QueryResult lastQueryResult_;
    HWND hQueryDocTabs_ = nullptr, hBtnNewDoc_ = nullptr, hBtnCloseDoc_ = nullptr;
    HWND hRowLimit_ = nullptr, hRowLimitLabel_ = nullptr, hResultSet_ = nullptr;
    struct SqlDocument {
        std::string id, title, databaseId, sql;
        QueryResult result;
        int selectedResult = 0;
        bool dirty = false;
    };
    struct QueryHistoryItem { std::string sql, databaseId, timestamp, error; bool success = false; };
    std::vector<SqlDocument> documents_;
    std::vector<QueryHistoryItem> queryHistory_;
    size_t activeDocument_ = 0;
    unsigned long long documentSequence_ = 0;
    int maxRows_ = 1000;
    bool changingDocument_ = false;
    bool workspaceWritable_ = true;
    EditorLanguage queryLanguage_ = EditorLanguage::Sql;
    void UpdateQueryLanguage();
    void CaptureDocument();
    void SelectDocument(size_t index);
    void RebuildDocumentTabs();
    void LoadWorkspace();
    void SaveWorkspace();
    void ShowQueryTools();
    void ExportQueryResult();
    void SaveSqlFile();

    // Data Grid controls
    HWND hComboGridTable_ = nullptr;
    HWND hEditGridSearch_ = nullptr;
    HWND hBtnGridSearch_ = nullptr;
    HWND hBtnGridPrev_ = nullptr;
    HWND hBtnGridNext_ = nullptr;
    HWND hBtnAddRow_ = nullptr;
    HWND hBtnEditRow_ = nullptr;
    HWND hGridStatus_ = nullptr;
    HWND hBtnDeleteRow_ = nullptr;
    HWND hBtnExportCsv_ = nullptr;
    HWND hBtnExportJson_ = nullptr;
    HWND hBtnExportSql_ = nullptr;
    HWND hListDataGrid_ = nullptr;
    HWND hComboPageSize_ = nullptr, hBtnGridRefresh_ = nullptr;
    TableDataResult currentTableData_;
    int gridPage_ = 1;
    int gridPageSize_ = 25;
    std::string gridSortCol_;
    std::string gridSortDir_ = "asc";
    DatabaseSchema schema_;
    std::vector<DatabaseInfo> databases_;
    bool populating_ = false;
    bool autoSelectDatabase_ = true;

    struct Job {
        enum class Kind { Schema, Query, Grid, Mutation, Export, Action } kind = Kind::Schema;
        std::atomic_bool cancel{ false };
        std::atomic_bool done{ false };
        DatabaseSchema schema;
        QueryResult query;
        TableDataResult table;
        std::string error;
        std::string documentId, databaseId, sql;
        std::wstring destination;
        bool success = false;
    };
    void RecordQuery(const Job& completed);
    std::shared_ptr<Job> job_;
    std::thread worker_;
    bool reloadPending_ = false;

    // Schema Viewer controls
    HWND hListSchemaCols_ = nullptr;
    HWND hListSchemaFks_ = nullptr;
    HWND hListSchemaIdx_ = nullptr;
    HWND hEditSchemaDdl_ = nullptr;
    HWND hBtnCopyDdl_ = nullptr, hSchemaTitle_ = nullptr;

    // ER Diagram state
    ERDiagram erDiagram_;
    int erOffsetX_ = 0, erOffsetY_ = 0;
    int erExtentW_ = 0, erExtentH_ = 0;
    void UpdateErScrollbars();
    HWND hBtnResetEr_ = nullptr, hErStatus_ = nullptr;
    int erCanvasX_ = 0, erCanvasY_ = 0;
    std::map<std::string, std::map<std::string, POINT>> erPositions_;
    std::map<std::string, RECT> erCardRects_;
    std::string draggedTable_;
    POINT dragStart_{};
    POINT dragPosition_{};
    void EnsureErPositions(bool reset = false);
    void OpenConnection(bool editing = false);
    void ShowConnectionTools();
    std::unique_ptr<NativeMockServer> mockServer_;

    void Layout();
    void Draw(HDC hdc);
    void PopulateDatabases();
    void PopulateTables();
    void RunSqlQuery(bool explain = false, bool entireDocument = false);
    void StartJob(const std::shared_ptr<Job>& job, std::function<void(Job&)> work);
    void PollJob();
    void UpdateEnabledState();
    bool ActiveConnectionIsConnected() const;
    bool CanNavigateDocuments() const;
    void RenderResults();
    void RenderGrid();
    void EditGridRow(bool add);
    void DeleteGridRow();
    void ExportTable(const std::string& format);
    void SelectWorkbenchTab(int tab);
    void RefreshDataGrid();
    void RefreshSchemaViewer();
    void RefreshErDiagram();
    void UpdateWorkbenchTabsVisibility();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace native_app
