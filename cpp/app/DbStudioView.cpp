#include "DbStudioView.h"
#include <windowsx.h>
#include "CodeEditor.h"
#include "Modals.h"
#include <commdlg.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <exception>
#include <cctype>
#include <nlohmann/json.hpp>

namespace native_app {

static constexpr wchar_t DbStudioViewClassName[] = L"DataForgeDbStudioViewClass";

static std::wstring ToWide(const std::string& str) {
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

static std::string SaveExportAtomically(const std::wstring& destination, const std::string& data, const std::atomic_bool* cancel) {
    const std::wstring temporary = destination + L".dataforge-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "Could not create an export file. Check destination permissions and free disk space.";
    bool successful = true;
    size_t offset = 0;
    while (offset < data.size() && !(cancel && cancel->load())) {
        DWORD written = 0;
        const DWORD requested = static_cast<DWORD>(std::min<size_t>(1024 * 1024, data.size() - offset));
        if (!WriteFile(file, data.data() + offset, requested, &written, nullptr) || written != requested) { successful = false; break; }
        offset += written;
    }
    if (successful) successful = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    const bool canceled = cancel && cancel->load();
    if (!successful || canceled || offset != data.size()) {
        DeleteFileW(temporary.c_str());
        return canceled ? "Export canceled. The destination was left intact." : "Could not finish the export. Check free disk space; the destination was left intact.";
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return "Could not replace the destination. Close any application using the file and check folder permissions.";
    }
    return {};
}
static bool DestructiveSql(const std::string& sql) {
    for (size_t position = 0; position < sql.size();) {
        const char current = sql[position];
        if (current == '\'' || current == '"' || current == '`' || current == '[') {
            const char closing = current == '[' ? ']' : current;
            ++position;
            while (position < sql.size()) {
                if (sql[position++] != closing) continue;
                if (position < sql.size() && sql[position] == closing) { ++position; continue; }
                break;
            }
        } else if (current == '-' && position + 1 < sql.size() && sql[position + 1] == '-') {
            const size_t end = sql.find('\n', position + 2); position = end == std::string::npos ? sql.size() : end + 1;
        } else if (current == '/' && position + 1 < sql.size() && sql[position + 1] == '*') {
            const size_t end = sql.find("*/", position + 2); position = end == std::string::npos ? sql.size() : end + 2;
        } else if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
            std::string token;
            while (position < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[position])) || sql[position] == '_'))
                token += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[position++])));
            if (token == "DROP" || token == "DELETE" || token == "ALTER" || token == "TRUNCATE") return true;
        } else ++position;
    }
    return false;
}
DbStudioView::DbStudioView(DbEngine* dbEngine) : dbEngine_(dbEngine) {}

DbStudioView::~DbStudioView() {
    RequestCancel();
    if (worker_.joinable()) worker_.join();
    if (hWnd_ && IsWindow(hWnd_)) DestroyWindow(hWnd_);
}

bool DbStudioView::Create(HWND hParent, int x, int y, int width, int height, UINT id) {
    hParent_ = hParent;
    width_ = width;
    height_ = height;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = DbStudioViewClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT, DbStudioViewClassName, L"Database Studio",
        WS_CHILD | WS_CLIPCHILDREN | WS_HSCROLL | WS_VSCROLL,
        x, y, width, height,
        hParent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        GetModuleHandle(nullptr), this
    );

    if (!hWnd_) return false;
    ShowScrollBar(hWnd_, SB_BOTH, FALSE);

    // Sidebar: Databases List
    hListDatabases_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(3001), GetModuleHandle(nullptr), nullptr
    );
    ListView_SetExtendedListViewStyle(hListDatabases_, LVS_EX_FULLROWSELECT);
    LVCOLUMNW lvc{};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 250; lvc.pszText = const_cast<LPWSTR>(L"Database"); ListView_InsertColumn(hListDatabases_, 0, &lvc);
    SendMessage(hListDatabases_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetBoldFont()), TRUE);

    hBtnNewDb_ = CreateWindowExW(0, L"BUTTON", L"+ New Database", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 120, 26, hWnd_, reinterpret_cast<HMENU>(3002), GetModuleHandle(nullptr), nullptr);
    SendMessage(hBtnNewDb_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);

    // Sidebar: Tables List
    hListTables_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(3003), GetModuleHandle(nullptr), nullptr
    );
    ListView_SetExtendedListViewStyle(hListTables_, LVS_EX_FULLROWSELECT);
    lvc.cx = 170; lvc.pszText = const_cast<LPWSTR>(L"Tables / Views"); ListView_InsertColumn(hListTables_, 0, &lvc);
    lvc.cx = 80; lvc.pszText = const_cast<LPWSTR>(L"Rows"); ListView_InsertColumn(hListTables_, 1, &lvc);
    SendMessage(hListTables_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // SQL Console Controls
    hEditSql_ = CodeEditor::Create(hWnd_, 3101, EditorLanguage::Sql);
    CodeEditor::SetText(hEditSql_, L"SELECT * FROM products LIMIT 10;");
    SendMessage(hEditSql_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    hBtnRunSql_ = CreateWindowExW(0, L"BUTTON", L"Execute SQL", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 95, 26, hWnd_, reinterpret_cast<HMENU>(3102), GetModuleHandle(nullptr), nullptr);
    hBtnFormatSql_ = CreateWindowExW(0, L"BUTTON", L"Format SQL", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 85, 26, hWnd_, reinterpret_cast<HMENU>(3103), GetModuleHandle(nullptr), nullptr);
    hBtnExplain_ = CreateWindowExW(0, L"BUTTON", L"Explain Plan", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 95, 26, hWnd_, reinterpret_cast<HMENU>(3104), GetModuleHandle(nullptr), nullptr);
    SendMessage(hBtnRunSql_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetBoldFont()), TRUE);
    SendMessage(hBtnFormatSql_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
    SendMessage(hBtnExplain_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hComboTemplates_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 150, 150, hWnd_, reinterpret_cast<HMENU>(3105), GetModuleHandle(nullptr), nullptr);
    static const wchar_t* templates[] = { L"Templates...", L"SELECT * FROM products;", L"SELECT * FROM categories;", L"SELECT * FROM customers;", L"JOIN products and categories" };
    for (const auto* t : templates) SendMessage(hComboTemplates_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    SendMessage(hComboTemplates_, CB_SETCURSEL, 0, 0);
    SendMessage(hComboTemplates_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hSqlStatus_ = CreateWindowExW(0, L"STATIC", L"Ctrl+Enter runs selected SQL or the entire editor. Ctrl+Space opens completion.",
        WS_CHILD | WS_VISIBLE, 0, 0, 400, 26, hWnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hBtnSqlExport_ = CreateWindowExW(0, L"BUTTON", L"Save results JSON", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 150, 30, hWnd_, reinterpret_cast<HMENU>(3107), GetModuleHandle(nullptr), nullptr);
    static const wchar_t* tabNames[] = { L"SQL Console", L"Data Grid", L"Schema Viewer", L"ER Diagram" };
    for (int i = 0; i < 4; ++i) {
        hTabs_[i] = CreateWindowExW(0, L"BUTTON", tabNames[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 120, 32, hWnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(3400 + i)), GetModuleHandle(nullptr), nullptr);
    }

    hListSqlResults_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | WS_BORDER,
        0, 0, 300, 200, hWnd_, reinterpret_cast<HMENU>(3106), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListSqlResults_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessage(hListSqlResults_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    // Data Grid Controls
    hComboGridTable_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 140, 150, hWnd_, reinterpret_cast<HMENU>(3201), GetModuleHandle(nullptr), nullptr);
    SendMessage(hComboGridTable_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hEditGridSearch_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 150, 24, hWnd_, reinterpret_cast<HMENU>(3202), GetModuleHandle(nullptr), nullptr);
    SendMessage(hEditGridSearch_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hBtnGridSearch_ = CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 65, 24, hWnd_, reinterpret_cast<HMENU>(3203), GetModuleHandle(nullptr), nullptr);
    hBtnGridPrev_ = CreateWindowExW(0, L"BUTTON", L"< Prev", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 60, 24, hWnd_, reinterpret_cast<HMENU>(3204), GetModuleHandle(nullptr), nullptr);
    hBtnGridNext_ = CreateWindowExW(0, L"BUTTON", L"Next >", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 60, 24, hWnd_, reinterpret_cast<HMENU>(3205), GetModuleHandle(nullptr), nullptr);
    hBtnAddRow_ = CreateWindowExW(0, L"BUTTON", L"Add Row", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 85, 30, hWnd_, reinterpret_cast<HMENU>(3206), GetModuleHandle(nullptr), nullptr);
    hBtnEditRow_ = CreateWindowExW(0, L"BUTTON", L"Edit Row", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 85, 30, hWnd_, reinterpret_cast<HMENU>(3212), GetModuleHandle(nullptr), nullptr);
    hGridStatus_ = CreateWindowExW(0, L"STATIC", L"Select a table to browse. Double-click a row to edit.", WS_CHILD,
        0, 0, 400, 26, hWnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hBtnDeleteRow_ = CreateWindowExW(0, L"BUTTON", L"Delete Row", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 85, 24, hWnd_, reinterpret_cast<HMENU>(3207), GetModuleHandle(nullptr), nullptr);
    hBtnExportCsv_ = CreateWindowExW(0, L"BUTTON", L"Export CSV", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 85, 24, hWnd_, reinterpret_cast<HMENU>(3208), GetModuleHandle(nullptr), nullptr);
    hBtnExportJson_ = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 85, 24, hWnd_, reinterpret_cast<HMENU>(3209), GetModuleHandle(nullptr), nullptr);
    hBtnExportSql_ = CreateWindowExW(0, L"BUTTON", L"Export SQL", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 85, 24, hWnd_, reinterpret_cast<HMENU>(3210), GetModuleHandle(nullptr), nullptr);

    SendMessage(hBtnGridSearch_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnGridPrev_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnGridNext_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnDeleteRow_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnExportCsv_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnExportJson_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnExportSql_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);

    hListDataGrid_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
        0, 0, 300, 200, hWnd_, reinterpret_cast<HMENU>(3211), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListDataGrid_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessage(hListDataGrid_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Schema Viewer Controls
    hListSchemaCols_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | WS_BORDER,
        0, 0, 200, 150, hWnd_, reinterpret_cast<HMENU>(3301), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListSchemaCols_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 120; lvc.pszText = const_cast<LPWSTR>(L"Column"); ListView_InsertColumn(hListSchemaCols_, 0, &lvc);
    lvc.cx = 80; lvc.pszText = const_cast<LPWSTR>(L"Type"); ListView_InsertColumn(hListSchemaCols_, 1, &lvc);
    lvc.cx = 60; lvc.pszText = const_cast<LPWSTR>(L"PK"); ListView_InsertColumn(hListSchemaCols_, 2, &lvc);
    lvc.cx = 70; lvc.pszText = const_cast<LPWSTR>(L"Not Null"); ListView_InsertColumn(hListSchemaCols_, 3, &lvc);
    SendMessage(hListSchemaCols_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hListSchemaFks_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | WS_BORDER,
        0, 0, 200, 100, hWnd_, reinterpret_cast<HMENU>(3302), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListSchemaFks_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 100; lvc.pszText = const_cast<LPWSTR>(L"From Col"); ListView_InsertColumn(hListSchemaFks_, 0, &lvc);
    lvc.cx = 120; lvc.pszText = const_cast<LPWSTR>(L"Target Table"); ListView_InsertColumn(hListSchemaFks_, 1, &lvc);
    lvc.cx = 100; lvc.pszText = const_cast<LPWSTR>(L"To Col"); ListView_InsertColumn(hListSchemaFks_, 2, &lvc);
    SendMessage(hListSchemaFks_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hListSchemaIdx_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | WS_BORDER,
        0, 0, 200, 100, hWnd_, reinterpret_cast<HMENU>(3303), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListSchemaIdx_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 150; lvc.pszText = const_cast<LPWSTR>(L"Index Name"); ListView_InsertColumn(hListSchemaIdx_, 0, &lvc);
    lvc.cx = 60; lvc.pszText = const_cast<LPWSTR>(L"Unique"); ListView_InsertColumn(hListSchemaIdx_, 1, &lvc);
    SendMessage(hListSchemaIdx_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hEditSchemaDdl_ = CodeEditor::Create(hWnd_, 3304, EditorLanguage::Sql, true);
    SendMessage(hEditSchemaDdl_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    RefreshData();
    Layout();
    ApplyTheme();
    return true;
}

void DbStudioView::Show(bool bShow) {
    ShowWindow(hWnd_, bShow ? SW_SHOW : SW_HIDE);
}

void DbStudioView::Resize(int x, int y, int width, int height) {
    width_ = width;
    height_ = height;
    SetWindowPos(hWnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    InvalidateRect(hWnd_, nullptr, TRUE);
}

void DbStudioView::ApplyTheme() {
    Theme::ApplyToWindow(hWnd_);
    CodeEditor::ApplyTheme(hEditSql_);
    CodeEditor::ApplyTheme(hEditSchemaDdl_);
    Layout();
    InvalidateRect(hWnd_, nullptr, TRUE);
}

bool DbStudioView::IsBusy() const { return job_ != nullptr; }
void DbStudioView::RequestCancel() {
    if (job_) {
        job_->cancel.store(true);
        SetWindowTextW(hSqlStatus_, L"Cancellation requested. Waiting for the database operation to finish...");
        SetWindowTextW(hGridStatus_, L"Cancellation requested...");
    }
}
void DbStudioView::LoadSql(const std::string& sql) {
    CodeEditor::SetText(hEditSql_, ToWide(sql));
    SelectWorkbenchTab(0);
    SetFocus(hEditSql_);
}

void DbStudioView::StartJob(const std::shared_ptr<Job>& job, std::function<void(Job&)> work) {
    if (IsBusy()) return;
    if (worker_.joinable()) worker_.join();
    job_ = job;
    SetWindowTextW(hSqlStatus_, L"Working... Execute SQL changes to Cancel while an operation is running.");
    SetWindowTextW(hGridStatus_, L"Working... Switch to SQL Console and press Cancel to stop a query.");
    UpdateEnabledState();
    SetTimer(hWnd_, 71, 50, nullptr);
    try {
        worker_ = std::thread([job, work = std::move(work)]() {
            try { work(*job); }
            catch (const std::exception& e) { job->error = e.what(); }
            catch (...) { job->error = "The database operation failed unexpectedly."; }
            job->done.store(true, std::memory_order_release);
        });
    } catch (const std::exception& e) {
        job->error = e.what();
        job->done.store(true, std::memory_order_release);
    }
}

void DbStudioView::PollJob() {
    if (!job_ || !job_->done.load(std::memory_order_acquire)) return;
    auto complete = job_;
    if (worker_.joinable()) worker_.join();
    job_.reset();
    KillTimer(hWnd_, 71);
    if (!complete->error.empty()) {
        SetWindowTextW(hSqlStatus_, ToWide(complete->error).c_str());
        SetWindowTextW(hGridStatus_, ToWide(complete->error).c_str());
        if (complete->kind == Job::Kind::Query) { lastQueryResult_ = {}; lastQueryResult_.error = complete->error; RenderResults(); }
        else if (complete->kind == Job::Kind::Grid) { currentTableData_ = {}; currentTableData_.error = complete->error; RenderGrid(); }
        if (OnToast) OnToast(complete->error);
    } else if (complete->kind == Job::Kind::Schema) {
        schema_ = std::move(complete->schema);
        PopulateTables();
        if (workbenchTab_ == 1) RefreshDataGrid();
        else if (workbenchTab_ == 2) RefreshSchemaViewer();
        else if (workbenchTab_ == 3) RefreshErDiagram();
        if (!lastQueryResult_.query.empty()) RenderResults();
        else SetWindowTextW(hSqlStatus_, L"Ctrl+Enter runs selected SQL or the entire editor. Ctrl+Space opens completion.");
    } else if (complete->kind == Job::Kind::Query) {
        lastQueryResult_ = std::move(complete->query);
        RenderResults();
        if (lastQueryResult_.error.empty()) RefreshData();
    } else if (complete->kind == Job::Kind::Grid) {
        currentTableData_ = std::move(complete->table);
        gridPage_ = currentTableData_.page;
        RenderGrid();
    } else if (complete->kind == Job::Kind::Mutation) {
        if (complete->success) { SetWindowTextW(hGridStatus_, L"Row saved."); RefreshData(); }
        else { SetWindowTextW(hGridStatus_, L"Row could not be saved. Check constraints, required fields, and primary keys.");
            MessageBoxW(hWnd_, L"The row could not be saved. Check required values, unique constraints, and foreign keys. No successful change was reported.", L"Database change failed", MB_OK | MB_ICONERROR); }
    } else if (complete->kind == Job::Kind::Export) {
        if (complete->success) {
            SetWindowTextW(hSqlStatus_, L"Export saved successfully."); SetWindowTextW(hGridStatus_, L"Export saved successfully.");
            if (OnToast) OnToast("Export saved successfully.");
        }
    }
    UpdateEnabledState();
    if (reloadPending_ && !IsBusy()) { reloadPending_ = false; RefreshData(); }
}

void DbStudioView::UpdateEnabledState() {
    const bool available = !IsBusy();
    const HWND controls[] = { hListDatabases_, hListTables_, hBtnNewDb_, hBtnFormatSql_, hBtnExplain_,
        hComboTemplates_, hComboGridTable_, hEditGridSearch_, hBtnGridSearch_, hBtnExportCsv_, hBtnExportJson_, hBtnExportSql_ };
    for (HWND control : controls) EnableWindow(control, available);
    EnableWindow(hBtnRunSql_, IsBusy() || !activeDbId_.empty());
    SetWindowTextW(hBtnRunSql_, IsBusy() ? L"Cancel" : L"Execute SQL");
    EnableWindow(hBtnSqlExport_, available && lastQueryResult_.error.empty() && !lastQueryResult_.columns.empty());
    EnableWindow(hBtnGridPrev_, available && gridPage_ > 1);
    EnableWindow(hBtnGridNext_, available && gridPage_ < currentTableData_.totalPages);
    bool editable = false;
    for (const auto& table : schema_.tables) if (table.name == activeTable_) editable = table.editable && table.type == "table";
    const int selection = ListView_GetNextItem(hListDataGrid_, -1, LVNI_SELECTED);
    EnableWindow(hBtnAddRow_, available && editable && !currentTableData_.columns.empty() && currentTableData_.error.empty());
    const bool identifiable = available && editable && !currentTableData_.primaryKeys.empty() && selection >= 0;
    EnableWindow(hBtnEditRow_, identifiable);
    EnableWindow(hBtnDeleteRow_, identifiable);
}

void DbStudioView::RefreshData() {
    if (IsBusy()) { reloadPending_ = true; return; }
    PopulateDatabases();
    if (activeDbId_.empty()) { schema_ = {}; PopulateTables(); UpdateEnabledState(); return; }
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Schema;
    const std::string database = activeDbId_;
    DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database](Job& result) { result.schema = engine->GetSchema(database); });
}

void DbStudioView::PopulateDatabases() {
    populating_ = true;
    ListView_DeleteAllItems(hListDatabases_);
    databases_ = dbEngine_->ListDatabases();
    int active = -1;
    for (int i = 0; i < static_cast<int>(databases_.size()); ++i) {
        const auto& database = databases_[i];
        const std::wstring name = ToWide(database.name) + (database.isSample ? L" (Sample)" : L"");
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = const_cast<LPWSTR>(name.c_str());
        ListView_InsertItem(hListDatabases_, &item);
        if (database.id == activeDbId_) active = i;
    }
    if (active < 0 && !databases_.empty()) { active = 0; activeDbId_ = databases_[0].id; activeTable_.clear(); }
    if (active >= 0) { ListView_SetItemState(hListDatabases_, active, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
    else activeDbId_.clear();
    populating_ = false;
}

void DbStudioView::PopulateTables() {
    populating_ = true;
    ListView_DeleteAllItems(hListTables_);
    SendMessageW(hComboGridTable_, CB_RESETCONTENT, 0, 0);
    SendMessageW(hComboTemplates_, CB_RESETCONTENT, 0, 0);
    SendMessageW(hComboTemplates_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Templates..."));
    std::vector<std::wstring> completions;
    int active = -1;
    for (int i = 0; i < static_cast<int>(schema_.tables.size()); ++i) {
        const auto& table = schema_.tables[i];
        const std::wstring name = ToWide(table.name);
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = const_cast<LPWSTR>(name.c_str());
        ListView_InsertItem(hListTables_, &item);
        const std::wstring rows = std::to_wstring(table.rowCount);
        ListView_SetItemText(hListTables_, i, 1, const_cast<LPWSTR>(rows.c_str()));
        SendMessageW(hComboGridTable_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        const std::wstring label = L"SELECT from " + name;
        SendMessageW(hComboTemplates_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        completions.push_back(name);
        for (const auto& column : table.columns) { completions.push_back(ToWide(column.name)); completions.push_back(name + L"." + ToWide(column.name)); }
        if (table.name == activeTable_) active = i;
    }
    if (active < 0 && !schema_.tables.empty()) { active = 0; activeTable_ = schema_.tables[0].name; gridPage_ = 1; gridSortCol_.clear(); }
    if (active < 0) { activeTable_.clear(); currentTableData_ = {}; RenderGrid(); }
    SendMessageW(hComboGridTable_, CB_SETCURSEL, active, 0);
    SendMessageW(hComboTemplates_, CB_SETCURSEL, 0, 0);
    if (active >= 0) ListView_SetItemState(hListTables_, active, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    CodeEditor::SetCompletions(hEditSql_, completions);
    populating_ = false;
}

void DbStudioView::RunSqlQuery(bool explain) {
    if (IsBusy()) { RequestCancel(); return; }
    if (activeDbId_.empty()) return;
    std::wstring text = CodeEditor::GetText(hEditSql_);
    DWORD start = 0, end = 0;
    SendMessageW(hEditSql_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (end > start && end <= text.size()) text = text.substr(start, end - start);
    const std::string sql = ToUtf8(text);
    if (sql.find_first_not_of(" \t\r\n") == std::string::npos) return;
    if (!explain && DestructiveSql(sql) && MessageBoxW(hWnd_, L"This SQL can remove or change database data or objects. Execute it?",
        L"Confirm SQL execution", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Query;
    const std::string database = activeDbId_;
    DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database, sql, explain](Job& result) {
        result.query = engine->ExecuteQuery(database, explain ? "EXPLAIN QUERY PLAN " + sql : sql, 1000, &result.cancel);
    });
}

static void FillRows(HWND list, const std::vector<std::string>& columns,
    const std::vector<std::map<std::string, std::string>>& rows, const std::vector<std::string>& primaryKeys = {}) {
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (int i = Header_GetItemCount(ListView_GetHeader(list)) - 1; i >= 0; --i) ListView_DeleteColumn(list, i);
    for (int c = 0; c < static_cast<int>(columns.size()); ++c) {
        std::wstring label = ToWide(columns[c]);
        if (std::find(primaryKeys.begin(), primaryKeys.end(), columns[c]) != primaryKeys.end()) label += L" [PK]";
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = Theme::Scale(155); column.pszText = const_cast<LPWSTR>(label.c_str());
        ListView_InsertColumn(list, c, &column);
    }
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        for (int c = 0; c < static_cast<int>(columns.size()); ++c) {
            const auto value = rows[r].find(columns[c]);
            const std::wstring text = value == rows[r].end() ? L"" : ToWide(value->second);
            if (c == 0) { LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = r; item.pszText = const_cast<LPWSTR>(text.c_str()); ListView_InsertItem(list, &item); }
            else ListView_SetItemText(list, r, c, const_cast<LPWSTR>(text.c_str()));
        }
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void DbStudioView::RenderResults() {
    if (!lastQueryResult_.error.empty()) {
        FillRows(hListSqlResults_, { "Execution error" }, { { { "Execution error", lastQueryResult_.error } } });
        ListView_SetColumnWidth(hListSqlResults_, 0, Theme::Scale(600));
        SetWindowTextW(hSqlStatus_, ToWide(lastQueryResult_.error).c_str());
        return;
    }
    FillRows(hListSqlResults_, lastQueryResult_.columns, lastQueryResult_.rows);
    std::wostringstream status;
    status << L"Returned " << lastQueryResult_.rows.size() << L" rows; affected " << lastQueryResult_.rowsAffected
        << L"; " << static_cast<int>(lastQueryResult_.executionTimeMs) << L" ms";
    if (lastQueryResult_.truncated) status << L". Results truncated at 1,000 rows.";
    SetWindowTextW(hSqlStatus_, status.str().c_str());
}

void DbStudioView::RefreshDataGrid() {
    if (IsBusy() || activeTable_.empty()) return;
    const int length = GetWindowTextLengthW(hEditGridSearch_);
    std::wstring search(length + 1, L'\0'); GetWindowTextW(hEditGridSearch_, search.data(), length + 1); search.resize(length);
    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Grid;
    const std::string database = activeDbId_, table = activeTable_, sort = gridSortCol_, direction = gridSortDir_, filter = ToUtf8(search);
    const int page = gridPage_, pageSize = gridPageSize_;
    DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database, table, sort, direction, filter, page, pageSize](Job& result) {
        result.table = engine->GetTableData(database, table, page, pageSize, sort, direction, filter);
    });
}

void DbStudioView::RenderGrid() {
    FillRows(hListDataGrid_, currentTableData_.columns, currentTableData_.rows, currentTableData_.primaryKeys);
    std::wstring status;
    if (!currentTableData_.error.empty()) status = ToWide(currentTableData_.error);
    else status = L"Page " + std::to_wstring(currentTableData_.page) + L" of " + std::to_wstring(currentTableData_.totalPages)
        + L" | " + std::to_wstring(currentTableData_.totalRows) + L" matching rows | Click a column to sort; double-click a row to edit.";
    SetWindowTextW(hGridStatus_, status.c_str());
    UpdateEnabledState();
}

struct RowEditorContext {
    const TableDataResult* tableData = nullptr;
    const std::map<std::string, std::string>* original = nullptr;
    std::map<std::string, std::string>* changes = nullptr;
    bool add = false;
    bool saved = false;
    std::vector<HWND> editControls;
};

static LRESULT CALLBACK RowEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto ctx = reinterpret_cast<RowEditorContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<RowEditorContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        int y = Theme::Scale(16);
        const int labelWidth = Theme::Scale(120);
        const int editWidth = Theme::Scale(280);
        const int rowHeight = Theme::Scale(26);
        const int rowSpacing = Theme::Scale(34);
        for (size_t i = 0; i < ctx->tableData->columns.size(); ++i) {
            const auto& col = ctx->tableData->columns[i];
            std::wstring label = ToWide(col) + L":";
            HWND hLbl = CreateWindowExW(0, L"STATIC", label.c_str(), WS_CHILD | WS_VISIBLE | SS_RIGHT,
                Theme::Scale(16), y + Theme::Scale(4), labelWidth, rowHeight, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

            std::wstring initialVal;
            if (!ctx->add && ctx->original) {
                auto it = ctx->original->find(col);
                if (it != ctx->original->end()) initialVal = ToWide(it->second);
            }
            HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initialVal.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                Theme::Scale(144), y, editWidth, rowHeight, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
            ctx->editControls.push_back(hEdit);
            y += rowSpacing;
        }
        HWND hBtnSave = CreateWindowExW(0, L"BUTTON", ctx->add ? L"Add" : L"Save",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            Theme::Scale(240), y + Theme::Scale(10), Theme::Scale(80), Theme::Scale(30),
            hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            Theme::Scale(330), y + Theme::Scale(10), Theme::Scale(80), Theme::Scale(30),
            hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        SendMessageW(hBtnSave, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
        SendMessageW(hBtnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
        Theme::ApplyToWindow(hwnd);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK && ctx) {
            for (size_t i = 0; i < ctx->tableData->columns.size() && i < ctx->editControls.size(); ++i) {
                const auto& col = ctx->tableData->columns[i];
                int len = GetWindowTextLengthW(ctx->editControls[i]);
                std::wstring text(len + 1, L'\0');
                GetWindowTextW(ctx->editControls[i], text.data(), len + 1);
                text.resize(len);
                std::string val = ToUtf8(text);
                if (ctx->add) {
                    (*ctx->changes)[col] = val;
                } else if (ctx->original) {
                    auto it = ctx->original->find(col);
                    if (it == ctx->original->end() || it->second != val) {
                        (*ctx->changes)[col] = val;
                    }
                }
            }
            ctx->saved = true;
            DestroyWindow(hwnd);
            return 0;
        } else if (id == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool ShowRowEditorDialog(HWND owner, const TableDataResult& tableData, const std::map<std::string, std::string>& original, bool add, std::map<std::string, std::string>& changes) {
    static bool registered = false;
    static const wchar_t className[] = L"DataForgeRowEditorClass";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RowEditorWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = Theme::GetBackgroundBrush();
        RegisterClassExW(&wc);
        registered = true;
    }

    RowEditorContext ctx;
    ctx.tableData = &tableData;
    ctx.original = &original;
    ctx.changes = &changes;
    ctx.add = add;

    int dlgWidth = Theme::Scale(460);
    int dlgHeight = Theme::Scale(100) + static_cast<int>(tableData.columns.size()) * Theme::Scale(34);
    if (dlgHeight > Theme::Scale(600)) dlgHeight = Theme::Scale(600);

    RECT rcOwner{};
    GetWindowRect(owner, &rcOwner);
    int x = rcOwner.left + (rcOwner.right - rcOwner.left - dlgWidth) / 2;
    int y = rcOwner.top + (rcOwner.bottom - rcOwner.top - dlgHeight) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        className,
        add ? L"Add New Row" : L"Edit Row",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlgWidth, dlgHeight,
        owner, nullptr, GetModuleHandleW(nullptr), &ctx
    );

    if (!hDlg) return false;

    EnableWindow(owner, FALSE);
    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    return ctx.saved;
}

void DbStudioView::EditGridRow(bool add) {
    if (IsBusy() || activeTable_.empty()) return;
    const int selected = ListView_GetNextItem(hListDataGrid_, -1, LVNI_SELECTED);
    if (!add && (selected < 0 || selected >= static_cast<int>(currentTableData_.rows.size()) || currentTableData_.primaryKeys.empty())) return;
    std::map<std::string, std::string> original, primary, changes;
    if (!add) {
        original = currentTableData_.rows[selected];
        for (const auto& key : currentTableData_.primaryKeys) { auto value = original.find(key); if (value == original.end()) return; primary[key] = value->second; }
    }
    if (!ShowRowEditorDialog(hWnd_, currentTableData_, original, add, changes) || (!add && changes.empty())) return;
    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Mutation;
    const std::string database = activeDbId_, table = activeTable_; DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database, table, primary, changes, add](Job& result) {
        result.success = add ? engine->InsertTableRow(database, table, changes) : engine->UpdateTableRow(database, table, primary, changes);
    });
}

void DbStudioView::DeleteGridRow() {
    if (IsBusy() || currentTableData_.primaryKeys.empty()) return;
    const int selected = ListView_GetNextItem(hListDataGrid_, -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(currentTableData_.rows.size())) return;
    if (MessageBoxW(hWnd_, L"Delete the selected row permanently?", L"Delete row", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    std::map<std::string, std::string> primary;
    for (const auto& key : currentTableData_.primaryKeys) { auto value = currentTableData_.rows[selected].find(key); if (value == currentTableData_.rows[selected].end()) return; primary[key] = value->second; }
    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Mutation;
    const std::string database = activeDbId_, table = activeTable_; DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database, table, primary](Job& result) { result.success = engine->DeleteTableRow(database, table, primary); });
}

void DbStudioView::ExportTable(const std::string& format) {
    if (IsBusy() || activeTable_.empty()) return;
    wchar_t file[32768]{};
    const std::wstring extension = ToWide(format);
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = hWnd_; dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file)); dialog.lpstrFilter = L"Export files\0*.*\0\0"; dialog.lpstrDefExt = extension.c_str();
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;
    const int length = GetWindowTextLengthW(hEditGridSearch_);
    std::wstring search(length + 1, L'\0'); GetWindowTextW(hEditGridSearch_, search.data(), length + 1); search.resize(length);
    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Export; job->destination = file;
    const std::string database = activeDbId_, table = activeTable_, sort = gridSortCol_, direction = gridSortDir_, filter = ToUtf8(search);
    DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database, table, format, sort, direction, filter](Job& result) {
        const std::string data = engine->ExportTableData(database, table, format, filter, sort, direction);
        if (result.cancel.load()) { result.error = "Export canceled."; return; }
        if (data.empty()) { result.error = "The database did not return an export. Check that the table exists and is readable."; return; }
        result.error = SaveExportAtomically(result.destination, data, &result.cancel);
        result.success = result.error.empty();
    });
}

void DbStudioView::SelectWorkbenchTab(int tab) {
    workbenchTab_ = tab;
    UpdateWorkbenchTabsVisibility();
    if (!IsBusy()) {
        if (tab == 1) RefreshDataGrid();
        else if (tab == 2) RefreshSchemaViewer();
        else if (tab == 3) RefreshErDiagram();
    }
    InvalidateRect(hWnd_, nullptr, TRUE);
}
void DbStudioView::RefreshSchemaViewer() {
    if (activeTable_.empty()) return;
    const DatabaseSchema& schema = schema_;

    ListView_DeleteAllItems(hListSchemaCols_);
    ListView_DeleteAllItems(hListSchemaFks_);
    ListView_DeleteAllItems(hListSchemaIdx_);
    CodeEditor::SetText(hEditSchemaDdl_, L"");

    for (const auto& t : schema.tables) {
        if (t.name == activeTable_) {
            // DDL
            CodeEditor::SetText(hEditSchemaDdl_, ToWide(t.ddl));

            // Columns
            for (int i = 0; i < static_cast<int>(t.columns.size()); ++i) {
                const auto& col = t.columns[i];
                LVITEMW itm{};
                itm.mask = LVIF_TEXT;
                itm.iItem = i;
                std::wstring name = ToWide(col.name);
                itm.pszText = const_cast<LPWSTR>(name.c_str());
                ListView_InsertItem(hListSchemaCols_, &itm);

                std::wstring type = ToWide(col.type);
                ListView_SetItemText(hListSchemaCols_, i, 1, const_cast<LPWSTR>(type.c_str()));

                std::wstring pk = col.pk ? L"YES" : L"NO";
                ListView_SetItemText(hListSchemaCols_, i, 2, const_cast<LPWSTR>(pk.c_str()));

                std::wstring nn = col.notnull ? L"YES" : L"NO";
                ListView_SetItemText(hListSchemaCols_, i, 3, const_cast<LPWSTR>(nn.c_str()));
            }

            // Foreign Keys
            for (int i = 0; i < static_cast<int>(t.foreignKeys.size()); ++i) {
                const auto& fk = t.foreignKeys[i];
                LVITEMW itm{};
                itm.mask = LVIF_TEXT;
                itm.iItem = i;
                std::wstring fromCol = ToWide(fk.fromColumn);
                itm.pszText = const_cast<LPWSTR>(fromCol.c_str());
                ListView_InsertItem(hListSchemaFks_, &itm);

                std::wstring tgtTbl = ToWide(fk.targetTable);
                ListView_SetItemText(hListSchemaFks_, i, 1, const_cast<LPWSTR>(tgtTbl.c_str()));

                std::wstring toCol = ToWide(fk.toColumn);
                ListView_SetItemText(hListSchemaFks_, i, 2, const_cast<LPWSTR>(toCol.c_str()));
            }

            // Indexes
            for (int i = 0; i < static_cast<int>(t.indexes.size()); ++i) {
                const auto& idx = t.indexes[i];
                LVITEMW itm{};
                itm.mask = LVIF_TEXT;
                itm.iItem = i;
                std::wstring name = ToWide(idx.name);
                itm.pszText = const_cast<LPWSTR>(name.c_str());
                ListView_InsertItem(hListSchemaIdx_, &itm);

                std::wstring u = idx.unique ? L"YES" : L"NO";
                ListView_SetItemText(hListSchemaIdx_, i, 1, const_cast<LPWSTR>(u.c_str()));
            }
            break;
        }
    }
}

void DbStudioView::RefreshErDiagram() {
    erDiagram_.nodes.clear(); erDiagram_.links.clear();
    erOffsetX_ = erOffsetY_ = 0;
    erDiagram_.databaseId = activeDbId_;
    for (const auto& table : schema_.tables) {
        ERNode node; node.id = table.name; node.name = table.name; node.columns = table.columns; node.rowCount = table.rowCount;
        erDiagram_.nodes.push_back(std::move(node));
        for (const auto& key : table.foreignKeys) {
            ERLink link; link.id = table.name + "." + key.fromColumn; link.source = table.name; link.sourceCol = key.fromColumn;
            link.target = key.targetTable; link.targetCol = key.toColumn; link.onUpdate = key.onUpdate; link.onDelete = key.onDelete;
            erDiagram_.links.push_back(std::move(link));
        }
    }
    UpdateErScrollbars();
    InvalidateRect(hWnd_, nullptr, TRUE);
}

void DbStudioView::UpdateErScrollbars() {
    const int availableHeight = std::max(Theme::Scale(120), height_ - Theme::Scale(84));
    const int availableWidth = std::max(Theme::Scale(100), width_ - sidebarWidth_ - Theme::Scale(32));
    const int cardWidth = Theme::Scale(235), gap = Theme::Scale(30), columnHeight = Theme::Scale(24);
    int x = 0, y = 0, maximumY = 0;
    for (const auto& node : erDiagram_.nodes) {
        const int cardHeight = Theme::Scale(38) + static_cast<int>(node.columns.size()) * columnHeight;
        if (y > 0 && y + cardHeight > availableHeight) { y = 0; x += cardWidth + gap; }
        maximumY = std::max(maximumY, y + cardHeight);
        y += cardHeight + gap;
    }
    erExtentW_ = erDiagram_.nodes.empty() ? 0 : x + cardWidth;
    erExtentH_ = maximumY;
    const bool diagram = workbenchTab_ == 3;
    ShowScrollBar(hWnd_, SB_HORZ, diagram && erExtentW_ > availableWidth);
    ShowScrollBar(hWnd_, SB_VERT, diagram && erExtentH_ > availableHeight);
    erOffsetX_ = std::max(0, std::min(erOffsetX_, std::max(0, erExtentW_ - availableWidth)));
    erOffsetY_ = std::max(0, std::min(erOffsetY_, std::max(0, erExtentH_ - availableHeight)));
    SCROLLINFO horizontal{}; horizontal.cbSize = sizeof(horizontal); horizontal.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    horizontal.nMax = std::max(0, erExtentW_ - 1); horizontal.nPage = availableWidth; horizontal.nPos = erOffsetX_;
    SCROLLINFO vertical{}; vertical.cbSize = sizeof(vertical); vertical.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    vertical.nMax = std::max(0, erExtentH_ - 1); vertical.nPage = availableHeight; vertical.nPos = erOffsetY_;
    SetScrollInfo(hWnd_, SB_HORZ, &horizontal, TRUE);
    SetScrollInfo(hWnd_, SB_VERT, &vertical, TRUE);
    ShowScrollBar(hWnd_, SB_HORZ, diagram && erExtentW_ > availableWidth);
    ShowScrollBar(hWnd_, SB_VERT, diagram && erExtentH_ > availableHeight);
}

void DbStudioView::Layout() {
    if (width_ <= 0 || height_ <= 0) return;
    const auto scale = [](int value) { return Theme::Scale(value); };
    const int gap = scale(8), row = scale(34), margin = scale(12);
    sidebarWidth_ = std::min(scale(265), std::max(scale(170), width_ / 4));
    auto place = [](HWND control, int x, int y, int width, int height) {
        SetWindowPos(control, nullptr, x, y, std::max(1, width), std::max(1, height), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const int dbHeight = std::min(scale(160), height_ / 3);
    place(hListDatabases_, gap, gap, sidebarWidth_ - gap * 2, dbHeight);
    place(hBtnNewDb_, gap, dbHeight + gap * 2, sidebarWidth_ - gap * 2, row);
    const int tablesY = dbHeight + row + gap * 3;
    place(hListTables_, gap, tablesY, sidebarWidth_ - gap * 2, height_ - tablesY - gap);
    ListView_SetColumnWidth(hListDatabases_, 0, sidebarWidth_ - scale(26));
    ListView_SetColumnWidth(hListTables_, 0, std::max(scale(90), sidebarWidth_ - scale(92)));
    ListView_SetColumnWidth(hListTables_, 1, scale(65));
    const int mainX = sidebarWidth_ + margin;
    const int contentW = std::max(scale(100), width_ - mainX - margin);
    const int tabWidth = std::min(scale(145), contentW / 4);
    for (int i = 0; i < 4; ++i) place(hTabs_[i], mainX + i * tabWidth, gap, tabWidth - scale(2), row);
    const int contentY = row + gap * 2, contentH = height_ - contentY - gap;
    const int editorHeight = std::max(scale(90), std::min(scale(260), contentH * 2 / 5));
    place(hEditSql_, mainX, contentY, contentW, editorHeight);
    int x = mainX, y = contentY + editorHeight + gap;
    const std::pair<HWND,int> sqlControls[] = { {hBtnRunSql_,120}, {hBtnFormatSql_,110}, {hBtnExplain_,115}, {hComboTemplates_,185}, {hBtnSqlExport_,155} };
    for (const auto& control : sqlControls) {
        const int w = scale(control.second);
        if (x > mainX && x + w > mainX + contentW) { x = mainX; y += row + gap; }
        place(control.first, x, y, std::min(w, contentW), control.first == hComboTemplates_ ? scale(240) : row);
        x += w + gap;
    }
    const int sqlStatusY = y + row + gap;
    place(hSqlStatus_, mainX, sqlStatusY, contentW, scale(28));
    const int resultY = sqlStatusY + scale(32);
    place(hListSqlResults_, mainX, resultY, contentW, height_ - resultY - gap);
    x = mainX; y = contentY;
    const std::pair<HWND,int> gridControls[] = { {hComboGridTable_,170}, {hEditGridSearch_,185}, {hBtnGridSearch_,85},
        {hBtnGridPrev_,80}, {hBtnGridNext_,80}, {hBtnAddRow_,90}, {hBtnEditRow_,90}, {hBtnDeleteRow_,100},
        {hBtnExportCsv_,105}, {hBtnExportJson_,115}, {hBtnExportSql_,105} };
    for (const auto& control : gridControls) {
        const int w = scale(control.second);
        if (x > mainX && x + w > mainX + contentW) { x = mainX; y += row + gap; }
        place(control.first, x, y, std::min(w, contentW), control.first == hComboGridTable_ ? scale(240) : row);
        x += w + gap;
    }
    const int gridStatusY = y + row + gap;
    place(hGridStatus_, mainX, gridStatusY, contentW, scale(28));
    const int gridY = gridStatusY + scale(32);
    place(hListDataGrid_, mainX, gridY, contentW, height_ - gridY - gap);
    const int halfW = (contentW - gap) / 2, halfH = (contentH - gap) / 2;
    place(hListSchemaCols_, mainX, contentY, halfW, halfH);
    place(hListSchemaFks_, mainX + halfW + gap, contentY, halfW, halfH / 2);
    place(hListSchemaIdx_, mainX + halfW + gap, contentY + halfH / 2 + gap, halfW, halfH / 2 - gap);
    place(hEditSchemaDdl_, mainX, contentY + halfH + gap, contentW, halfH);
    UpdateWorkbenchTabsVisibility();
}
void DbStudioView::UpdateWorkbenchTabsVisibility() {
    UpdateErScrollbars();
    for (int i = 0; i < 4; ++i)
        SendMessageW(hTabs_[i], WM_SETFONT, reinterpret_cast<WPARAM>(i == workbenchTab_ ? Theme::GetBoldFont() : Theme::GetMainFont()), TRUE);
    for (int i = 0; i < 4; ++i) {
        RemovePropW(hTabs_[i], L"DataForge.Active");
        if (i == workbenchTab_) SetPropW(hTabs_[i], L"DataForge.Active", reinterpret_cast<HANDLE>(1));
        InvalidateRect(hTabs_[i], nullptr, FALSE);
    }
    bool isSql = (workbenchTab_ == 0);
    ShowWindow(hEditSql_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnRunSql_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnFormatSql_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnExplain_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hComboTemplates_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hListSqlResults_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hSqlStatus_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnSqlExport_, isSql ? SW_SHOW : SW_HIDE);

    bool isGrid = (workbenchTab_ == 1);
    ShowWindow(hComboGridTable_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditGridSearch_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnGridSearch_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnGridPrev_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnGridNext_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnDeleteRow_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnAddRow_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnEditRow_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hGridStatus_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnExportCsv_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnExportJson_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnExportSql_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hListDataGrid_, isGrid ? SW_SHOW : SW_HIDE);

    bool isSchema = (workbenchTab_ == 2);
    ShowWindow(hListSchemaCols_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hListSchemaFks_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hListSchemaIdx_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditSchemaDdl_, isSchema ? SW_SHOW : SW_HIDE);
}

void DbStudioView::Draw(HDC hdc) {
    const auto& tc = Theme::Get();

    RECT clientRc{ 0, 0, width_, height_ };
    FillRect(hdc, &clientRc, Theme::GetBgPrimaryBrush());

    // Sidebar background
    RECT sbRc{ 0, 0, sidebarWidth_, height_ };
    FillRect(hdc, &sbRc, Theme::GetBgSecondaryBrush());

    // Separator line
    HPEN hSepPen = CreatePen(PS_SOLID, 1, tc.borderColor);
    HGDIOBJ oldPen = SelectObject(hdc, hSepPen);
    MoveToEx(hdc, sidebarWidth_, 0, nullptr);
    LineTo(hdc, sidebarWidth_, height_);

    // If ER diagram tab is active, draw the visual ER diagram!
    if (workbenchTab_ == 3) {
        const int savedDc = SaveDC(hdc);
        IntersectClipRect(hdc, sidebarWidth_ + Theme::Scale(8), Theme::Scale(52), width_, height_);
        int mainX = sidebarWidth_ + Theme::Scale(20) - erOffsetX_;
        int startY = Theme::Scale(64) - erOffsetY_;
        int cardW = Theme::Scale(235);
        int colH = Theme::Scale(24);

        std::map<std::string, RECT> cardRects;
        int curX = mainX;
        int curY = startY;

        for (const auto& node : erDiagram_.nodes) {
            int cardH = Theme::Scale(38) + static_cast<int>(node.columns.size()) * colH;
            if (curY > startY && curY + cardH > height_ - Theme::Scale(20) - erOffsetY_) {
                curY = startY;
                curX += cardW + Theme::Scale(30);
            }

            RECT rcCard = { curX, curY, curX + cardW, curY + cardH };
            cardRects[node.name] = rcCard;

            // Draw Card Background
            HBRUSH brCard = CreateSolidBrush(tc.bgSecondary);
            HPEN penBorder = CreatePen(PS_SOLID, 1, tc.borderColor);
            HGDIOBJ ob = SelectObject(hdc, brCard);
            HGDIOBJ op = SelectObject(hdc, penBorder);
            RoundRect(hdc, rcCard.left, rcCard.top, rcCard.right, rcCard.bottom, 6, 6);
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(penBorder);
            DeleteObject(brCard);

            // Card Header
            RECT rcHdr = { rcCard.left, rcCard.top, rcCard.right, rcCard.top + Theme::Scale(30) };
            HBRUSH brHdr = CreateSolidBrush(tc.bgTertiary);
            FillRect(hdc, &rcHdr, brHdr);
            DeleteObject(brHdr);

            SelectObject(hdc, Theme::GetBoldFont());
            SetTextColor(hdc, tc.textPrimary);
            SetBkMode(hdc, TRANSPARENT);
            RECT trHdr = { rcHdr.left + 8, rcHdr.top, rcHdr.right - 8, rcHdr.bottom };
            DrawTextW(hdc, ToWide(node.name).c_str(), -1, &trHdr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Draw columns
            int colY = rcCard.top + Theme::Scale(32);
            for (const auto& c : node.columns) {
                RECT cr = { rcCard.left + 8, colY, rcCard.right - 8, colY + colH };
                SelectObject(hdc, Theme::GetSmallFont());
                if (c.pk) {
                    SetTextColor(hdc, tc.methodPost);
                    std::wstring txt = L"\x2605 " + ToWide(c.name) + L" (" + ToWide(c.type) + L")";
                    DrawTextW(hdc, txt.c_str(), -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    SetTextColor(hdc, tc.textSecondary);
                    std::wstring txt = ToWide(c.name) + L" (" + ToWide(c.type) + L")";
                    DrawTextW(hdc, txt.c_str(), -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                colY += colH;
            }

            curY += cardH + Theme::Scale(30);
        }

        // Draw Links
        HPEN linkPen = CreatePen(PS_SOLID, 2, tc.accent);
        HGDIOBJ op = SelectObject(hdc, linkPen);
        for (const auto& link : erDiagram_.links) {
            auto srcIt = cardRects.find(link.source);
            auto tgtIt = cardRects.find(link.target);
            if (srcIt != cardRects.end() && tgtIt != cardRects.end()) {
                POINT pt1 = { srcIt->second.right, (srcIt->second.top + srcIt->second.bottom) / 2 };
                POINT pt2 = { tgtIt->second.left, (tgtIt->second.top + tgtIt->second.bottom) / 2 };
                MoveToEx(hdc, pt1.x, pt1.y, nullptr);
                LineTo(hdc, pt2.x, pt2.y);
            }
        }
        SelectObject(hdc, op);
        DeleteObject(linkPen);
        RestoreDC(hdc, savedDc);
    }

    SelectObject(hdc, oldPen);
    DeleteObject(hSepPen);
}

LRESULT CALLBACK DbStudioView::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DbStudioView* pThis = reinterpret_cast<DbStudioView*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<DbStudioView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
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
    case WM_HSCROLL:
    case WM_VSCROLL: {
        if (!pThis || pThis->workbenchTab_ != 3) break;
        const int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT;
        SCROLLINFO scroll{}; scroll.cbSize = sizeof(scroll); scroll.fMask = SIF_ALL;
        GetScrollInfo(hWnd, bar, &scroll);
        int position = scroll.nPos;
        const int line = Theme::Scale(32);
        switch (LOWORD(wParam)) {
        case SB_LINEUP: position -= line; break;
        case SB_LINEDOWN: position += line; break;
        case SB_PAGEUP: position -= scroll.nPage; break;
        case SB_PAGEDOWN: position += scroll.nPage; break;
        case SB_THUMBTRACK: position = scroll.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = scroll.nMax; break;
        default: return 0;
        }
        if (bar == SB_HORZ) pThis->erOffsetX_ = position;
        else pThis->erOffsetY_ = position;
        pThis->UpdateErScrollbars(); InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (pThis && pThis->workbenchTab_ == 3) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * Theme::Scale(72);
            if ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) || pThis->erExtentH_ <= pThis->height_ - Theme::Scale(84)) pThis->erOffsetX_ -= delta;
            else pThis->erOffsetY_ -= delta;
            pThis->UpdateErScrollbars(); InvalidateRect(hWnd, nullptr, TRUE); return 0;
        }
        break;
    case WM_TIMER:
        if (pThis && wParam == 71) { pThis->PollJob(); return 0; }
        break;
    case WM_COMMAND: {
        if (!pThis) break;
        const WORD id = LOWORD(wParam), code = HIWORD(wParam);
        try {
            if (id >= 3400 && id <= 3403) pThis->SelectWorkbenchTab(id - 3400);
            else if (id == 3102) pThis->RunSqlQuery();
            else if (id == 3103 && !pThis->IsBusy()) {
                CodeEditor::SetText(pThis->hEditSql_, ToWide(pThis->dbEngine_->FormatQuery(ToUtf8(CodeEditor::GetText(pThis->hEditSql_)))));
            } else if (id == 3104 && !pThis->IsBusy()) pThis->RunSqlQuery(true);
            else if (id == 3105 && code == CBN_SELCHANGE && !pThis->IsBusy()) {
                const int selected = static_cast<int>(SendMessageW(pThis->hComboTemplates_, CB_GETCURSEL, 0, 0)) - 1;
                if (selected >= 0 && selected < static_cast<int>(pThis->schema_.tables.size())) {
                    std::string table = pThis->schema_.tables[selected].name;
                    std::string quoted = "\"";
                    for (char c : table) { if (c == '"') quoted += '"'; quoted += c; }
                    quoted += '"';
                    CodeEditor::SetText(pThis->hEditSql_, ToWide("SELECT * FROM " + quoted + " LIMIT 25;"));
                }
            } else if (id == 3107 && !pThis->IsBusy()) {
                wchar_t file[32768]{};
                OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = hWnd; dialog.lpstrFile = file;
                dialog.nMaxFile = static_cast<DWORD>(std::size(file)); dialog.lpstrFilter = L"JSON (*.json)\0*.json\0\0"; dialog.lpstrDefExt = L"json";
                dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                if (GetSaveFileNameW(&dialog)) {
                    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Export; job->destination = file;
                    const auto rows = pThis->lastQueryResult_.rows;
                    pThis->StartJob(job, [rows](Job& result) {
                        const std::string data = nlohmann::json(rows).dump(2);
                        result.error = SaveExportAtomically(result.destination, data, &result.cancel);
                        result.success = result.error.empty();
                    });
                }
            } else if (id == 3002 && !pThis->IsBusy()) {
                if (pThis->OnOpenNewDbDialog) pThis->OnOpenNewDbDialog();
            } else if (id == 3201 && code == CBN_SELCHANGE && !pThis->IsBusy()) {
                const int selected = static_cast<int>(SendMessageW(pThis->hComboGridTable_, CB_GETCURSEL, 0, 0));
                if (selected >= 0 && selected < static_cast<int>(pThis->schema_.tables.size())) {
                    pThis->activeTable_ = pThis->schema_.tables[selected].name;
                    pThis->gridPage_ = 1; pThis->gridSortCol_.clear(); pThis->RefreshDataGrid();
                }
            } else if (id == 3203 && !pThis->IsBusy()) { pThis->gridPage_ = 1; pThis->RefreshDataGrid(); }
            else if (id == 3204 && !pThis->IsBusy() && pThis->gridPage_ > 1) { --pThis->gridPage_; pThis->RefreshDataGrid(); }
            else if (id == 3205 && !pThis->IsBusy() && pThis->gridPage_ < pThis->currentTableData_.totalPages) { ++pThis->gridPage_; pThis->RefreshDataGrid(); }
            else if (id == 3206) pThis->EditGridRow(true);
            else if (id == 3212) pThis->EditGridRow(false);
            else if (id == 3207) pThis->DeleteGridRow();
            else if (id == 3208) pThis->ExportTable("csv");
            else if (id == 3209) pThis->ExportTable("json");
            else if (id == 3210) pThis->ExportTable("sql");
        } catch (const std::exception& error) {
            MessageBoxW(hWnd, ToWide(error.what()).c_str(), L"Database operation failed", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_NOTIFY: {
        if (!pThis) break;
        NMHDR* notification = reinterpret_cast<NMHDR*>(lParam);
        if (notification->idFrom == 3211) {
            if (notification->code == LVN_ITEMCHANGED) pThis->UpdateEnabledState();
            else if (notification->code == NM_DBLCLK) pThis->EditGridRow(false);
            else if (notification->code == LVN_COLUMNCLICK && !pThis->IsBusy()) {
                const auto* click = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (click->iSubItem >= 0 && click->iSubItem < static_cast<int>(pThis->currentTableData_.columns.size())) {
                    const std::string column = pThis->currentTableData_.columns[click->iSubItem];
                    pThis->gridSortDir_ = pThis->gridSortCol_ == column && pThis->gridSortDir_ == "asc" ? "desc" : "asc";
                    pThis->gridSortCol_ = column; pThis->gridPage_ = 1; pThis->RefreshDataGrid();
                }
            }
        } else if (notification->code == LVN_ITEMCHANGED && !pThis->populating_ && !pThis->IsBusy()) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
            if (!(change->uNewState & LVIS_SELECTED) || (change->uOldState & LVIS_SELECTED)) return 0;
            if (notification->idFrom == 3001 && change->iItem >= 0 && change->iItem < static_cast<int>(pThis->databases_.size())) {
                pThis->activeDbId_ = pThis->databases_[change->iItem].id;
                pThis->activeTable_.clear(); pThis->gridPage_ = 1; pThis->gridSortCol_.clear();
                pThis->lastQueryResult_ = {}; pThis->RenderResults(); pThis->RefreshData();
            } else if (notification->idFrom == 3003 && change->iItem >= 0 && change->iItem < static_cast<int>(pThis->schema_.tables.size())) {
                pThis->activeTable_ = pThis->schema_.tables[change->iItem].name;
                pThis->gridPage_ = 1; pThis->gridSortCol_.clear();
                SendMessageW(pThis->hComboGridTable_, CB_SETCURSEL, change->iItem, 0);
                if (pThis->workbenchTab_ == 1) pThis->RefreshDataGrid();
                else if (pThis->workbenchTab_ == 2) pThis->RefreshSchemaViewer();
            }
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, 71);
        if (pThis) pThis->RequestCancel();
        return 0;
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

bool DbStudioView::RunSelfTests(std::wstring& failure) {
    const auto check = [&failure](bool condition, const wchar_t* label) { if (!condition) failure += std::wstring(label) + L"; "; return condition; };
    bool passed = true;
    std::filesystem::path workspace;
    HWND parent = nullptr;
    bool ownsWorkspace = false;
    try {
        wchar_t temporary[32768]{};
        if (!GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary)) throw std::runtime_error("Cannot locate temporary directory");
        workspace = std::filesystem::path(temporary) / (L"DataForge-db-ui-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(workspace)) throw std::runtime_error("Cannot create isolated test directory");
        ownsWorkspace = true;
        DbEngine engine(ToUtf8(workspace.wstring()), false);
        engine.CreateDatabase("UI acceptance", "blank");
        parent = CreateWindowExW(0, L"STATIC", L"Native DB acceptance", WS_OVERLAPPEDWINDOW, 0, 0, 1200, 850, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!parent) throw std::runtime_error("Cannot create hidden test parent");
        {
            DbStudioView view(&engine);
            if (!view.Create(parent, 0, 0, 1200, 800, 99)) throw std::runtime_error("Cannot create database view");
            const auto waitForCompletion = [&view]() {
                const ULONGLONG deadline = GetTickCount64() + 10000;
                while (view.IsBusy() && GetTickCount64() < deadline) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
                        TranslateMessage(&message); DispatchMessageW(&message);
                    }
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                }
                if (view.IsBusy()) { view.RequestCancel(); return false; }
                return true;
            };
            passed &= check(waitForCompletion(), L"asynchronous schema initialization");
            view.LoadSql("CREATE TABLE ui_rows(id INTEGER PRIMARY KEY, label TEXT); INSERT INTO ui_rows VALUES(1, 'h\xC3\xA9llo');");
            view.Execute();
            passed &= check(view.IsBusy(), L"query schedules an owned worker");
            passed &= check(waitForCompletion() && view.lastQueryResult_.error.empty(), L"native SQL mutation and schema refresh");
            view.LoadSql("SELECT * FROM ui_rows;"); view.Execute();
            passed &= check(waitForCompletion() && view.lastQueryResult_.rows.size() == 1
                && view.lastQueryResult_.rows[0].at("label") == "h\xC3\xA9llo", L"Unicode SQL results survive the native editor");
            view.SelectWorkbenchTab(1);
            passed &= check(waitForCompletion() && view.currentTableData_.rows.size() == 1
                && ListView_GetItemCount(view.hListDataGrid_) == 1, L"paginated native grid renders real rows");
            view.LoadSql("WITH RECURSIVE counter(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM counter WHERE n<100000000) SELECT sum(n) FROM counter;");
            view.Execute(); view.RequestCancel();
            passed &= check(waitForCompletion() && !view.lastQueryResult_.error.empty(), L"active SQL cancellation reaches SQLite");
            passed &= check(!view.worker_.joinable(), L"completion joins the worker before destroying the view");
        }
        DestroyWindow(parent); parent = nullptr;
        const auto exported = workspace / L"existing export.json";
        { std::ofstream stream(exported, std::ios::binary); stream << "original"; }
        std::atomic_bool cancellation{ true };
        passed &= check(!SaveExportAtomically(exported.wstring(), "replacement", &cancellation).empty(), L"canceled export reports failure");
        std::ifstream saved(exported, std::ios::binary); std::string contents((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
        passed &= check(contents == "original", L"canceled export preserves the existing destination");
        saved.close();
        cancellation.store(false);
        passed &= check(SaveExportAtomically(exported.wstring(), "replacement", &cancellation).empty(), L"native atomic export succeeds");
    } catch (const std::exception& error) {
        failure += L"Database UI self-test failed: " + ToWide(error.what()) + L"; "; passed = false;
    }
    if (parent && IsWindow(parent)) DestroyWindow(parent);
    if (ownsWorkspace) { std::error_code ignored; std::filesystem::remove_all(workspace, ignored); }
    return passed;
}
} // namespace native_app
