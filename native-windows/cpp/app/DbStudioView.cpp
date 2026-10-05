#include "DbStudioView.h"
#include <windowsx.h>
#include "ErLayout.h"
#include <uxtheme.h>
#include "CodeEditor.h"
#include "Modals.h"
#include "NativeDialogs.h"
#include "NativeMockServer.h"
#include <wincrypt.h>
#include <iomanip>
#include <cwctype>
#pragma comment(lib, "crypt32.lib")
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

static bool Closing(HWND window) {
    return window && GetPropW(GetAncestor(window, GA_ROOT), L"DataForge.Closing") != nullptr;
}

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
static bool CopyNativeText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool copied = false;
    if (EmptyClipboard()) {
        const size_t length = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE, length)) {
            if (void* destination = GlobalLock(allocation)) {
                memcpy(destination, text.c_str(), length); GlobalUnlock(allocation);
                if (SetClipboardData(CF_UNICODETEXT, allocation)) copied = true;
                else GlobalFree(allocation);
            } else GlobalFree(allocation);
        }
    }
    CloseClipboard(); return copied;
}
static nlohmann::json TypedRowsJson(const std::vector<TypedRow>& rows) {
    auto result = nlohmann::json::array();
    for (const auto& row : rows) {
        auto item = nlohmann::json::object();
        for (const auto& field : row) {
            const auto& value = field.second;
            switch (value.type) {
            case DbValueType::Null: item[field.first] = nullptr; break;
            case DbValueType::Integer: item[field.first] = std::stoll(value.text); break;
            case DbValueType::Real: item[field.first] = std::stod(value.text); break;
            case DbValueType::Boolean: item[field.first] = value.text == "true" || value.text == "1"; break;
            case DbValueType::Json: item[field.first] = nlohmann::json::parse(value.text); break;
            case DbValueType::Blob: {
                std::string hexadecimal; static constexpr char digits[] = "0123456789abcdef";
                for (unsigned char byte : value.text) { hexadecimal += digits[byte >> 4]; hexadecimal += digits[byte & 15]; }
                item[field.first] = {{"$binary", hexadecimal}, {"$encoding", "hex"}}; break;
            }
            default: item[field.first] = value.text; break;
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}
static std::string ProtectWorkspace(const std::string& clear) {
    if (clear.size() > 128 * 1024 * 1024) throw std::runtime_error("The query workspace is too large to save. Save large documents to files and close them.");
    DATA_BLOB input{static_cast<DWORD>(clear.size()), reinterpret_cast<BYTE*>(const_cast<char*>(clear.data()))}, output{};
    if (!CryptProtectData(&input, L"DataForge native query workspace", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot protect the query workspace for this Windows account.");
    std::string encrypted(reinterpret_cast<const char*>(output.pbData), output.cbData); LocalFree(output.pbData); return encrypted;
}
static std::string UnprotectWorkspace(const std::string& encrypted) {
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()))}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("This query workspace cannot be opened by the current Windows account.");
    std::string clear(reinterpret_cast<const char*>(output.pbData), output.cbData); LocalFree(output.pbData); return clear;
}
static std::string QuoteTableName(const std::string& name, const std::string& dialect) {
    const char opening = dialect == "mysql" || dialect == "mariadb" ? '`' : dialect == "mssql" ? '[' : '"';
    const char closing = opening == '[' ? ']' : opening;
    std::string quoted(1, opening);
    for (char character : name) { if (character == closing) quoted += character; quoted += character; }
    quoted += closing; return quoted;
}
static std::string LimitTemplate(std::string body, const std::string& dialect, int limit) {
    if (dialect == "mssql" || dialect == "sqlserver") { body.insert(7, "TOP " + std::to_string(limit) + " "); return body + ";"; }
    if (dialect == "oracle" || dialect == "db2") return body + " FETCH FIRST " + std::to_string(limit) + " ROWS ONLY;";
    return body + " LIMIT " + std::to_string(limit) + ";";
}
static std::string TemplateTable(const std::string& table, const std::string& dialect, const std::string& namespaceName) {
    return (namespaceName.empty() ? std::string{} : QuoteTableName(namespaceName, dialect) + ".") + QuoteTableName(table, dialect);
}
static std::string SelectTemplate(const std::string& table, const DatabaseSchema& schema, int maxRows, const std::string& namespaceName = {}) {
    const int limit = std::min(maxRows, 25);
    if (schema.kind == "document") return nlohmann::json({{"collection", table}, {"operation", "find"}, {"filter", nlohmann::json::object()}, {"limit", maxRows}}).dump(2);
    if (schema.kind == "keyvalue") return "SCAN 0 COUNT " + std::to_string(std::min(maxRows, 100));
    return LimitTemplate("SELECT * FROM " + TemplateTable(table, schema.dialect, namespaceName), schema.dialect, limit);
}
struct QueryTemplate { std::string name, text; bool writes = false; };
static std::vector<QueryTemplate> QueryTemplates(const DatabaseSchema& schema, int maxRows, const std::string& namespaceName) {
    const auto first = schema.tables.empty() ? nullptr : &schema.tables.front();
    const int limit = std::min(maxRows, 50);
    if (schema.kind == "document") {
        const std::string collection = first ? first->name : "collection_name";
        return {
            {"Find", nlohmann::json({{"collection", collection}, {"operation", "find"}, {"filter", nlohmann::json::object()}, {"limit", limit}}).dump(2)},
            {"Aggregate", nlohmann::json({{"collection", collection}, {"operation", "aggregate"}, {"pipeline", nlohmann::json::array({{{"$limit", limit}}})}}).dump(2)},
            {"Count", nlohmann::json({{"collection", collection}, {"operation", "count"}, {"filter", nlohmann::json::object()}}).dump(2)},
            {"Insert", nlohmann::json({{"collection", collection}, {"operation", "insert_one"}, {"document", nlohmann::json::object()}}).dump(2), true}
        };
    }
    if (schema.kind == "keyvalue") return {{"Scan", "SCAN 0 COUNT " + std::to_string(std::min(maxRows, 100))}, {"Info", "INFO"}, {"Size", "DBSIZE"}};
    std::vector<QueryTemplate> result{{"SELECT", first ? LimitTemplate("SELECT * FROM " + TemplateTable(first->name, schema.dialect, namespaceName), schema.dialect, limit)
        : schema.dialect == "oracle" ? "SELECT 1 FROM DUAL;" : "SELECT 1;"}};
    bool joinAdded = false;
    for (const auto& table : schema.tables) {
        for (const auto& foreign : table.foreignKeys) {
            const auto target = std::find_if(schema.tables.begin(), schema.tables.end(), [&](const auto& candidate) { return candidate.name == foreign.targetTable; });
            if (target == schema.tables.end() || foreign.fromColumn.empty()) continue;
            const auto targetColumn = !foreign.toColumn.empty() ? foreign.toColumn : target->primaryKeys.size() == 1 ? target->primaryKeys.front() : std::string{};
            if (targetColumn.empty()) continue;
            result.push_back({"JOIN", LimitTemplate("SELECT a.*, b.* FROM " + TemplateTable(table.name, schema.dialect, namespaceName) + " a JOIN "
                + TemplateTable(target->name, schema.dialect, namespaceName) + " b ON a." + QuoteTableName(foreign.fromColumn, schema.dialect)
                + " = b." + QuoteTableName(targetColumn, schema.dialect), schema.dialect, limit)});
            joinAdded = true; break;
        }
        if (joinAdded) break;
    }
    std::string identity = "BIGINT PRIMARY KEY";
    if (schema.dialect == "sqlite") identity = "INTEGER PRIMARY KEY";
    else if (schema.dialect == "postgresql") identity = "BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY";
    else if (schema.dialect == "mysql" || schema.dialect == "mariadb") identity = "BIGINT AUTO_INCREMENT PRIMARY KEY";
    else if (schema.dialect == "mssql" || schema.dialect == "sqlserver") identity = "BIGINT IDENTITY(1,1) PRIMARY KEY";
    else if (schema.dialect == "oracle") identity = "NUMBER GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY";
    result.push_back({"CREATE TABLE", "CREATE TABLE " + TemplateTable("new_table", schema.dialect, namespaceName) + " (\n  "
        + QuoteTableName("id", schema.dialect) + " " + identity + ",\n  " + QuoteTableName("name", schema.dialect)
        + (schema.dialect == "oracle" ? " VARCHAR2(255)\n);" : " VARCHAR(255)\n);"), true});
    return result;
}
static LRESULT CALLBACK QueryTabProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) { RECT client{}; GetClientRect(window, &client); FillRect(reinterpret_cast<HDC>(wParam), &client, Theme::GetBgPrimaryBrush()); return 1; }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, QueryTabProc, 92);
    return DefSubclassProc(window, message, wParam, lParam);
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
static bool DestructiveQuery(const std::string& text, const std::string& kind) {
    if (kind == "sql") return DestructiveSql(text);
    try {
        if (kind == "document") {
            const auto command = nlohmann::json::parse(text);
            const auto operation = command.value("operation", "find");
            if (operation == "insert_one" || operation == "update_one" || operation == "delete_one") return true;
            if (command.contains("pipeline") && command["pipeline"].is_array())
                for (const auto& stage : command["pipeline"])
                    if (stage.is_object() && (stage.contains("$out") || stage.contains("$merge"))) return true;
            return false;
        }
        if (kind == "keyvalue") {
            const size_t start = text.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) return false;
            std::string command;
            if (text[start] == '{') command = nlohmann::json::parse(text).at("command").get<std::string>();
            else {
                size_t position = start;
                if (text[position] == '\'' || text[position] == '"') ++position;
                while (position < text.size() && std::isalpha(static_cast<unsigned char>(text[position]))) command += text[position++];
            }
            std::transform(command.begin(), command.end(), command.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
            return command == "FLUSHALL" || command == "FLUSHDB" || command == "DEL" || command == "UNLINK" || command == "RENAME" || command == "RESTORE";
        }
    } catch (const nlohmann::json::exception&) { /* Invalid input is diagnosed by the provider. */ }
    return false;
}
DbStudioView::DbStudioView(DbEngine* dbEngine) : dbEngine_(dbEngine) {}

DbStudioView::~DbStudioView() {
    SaveWorkspace();
    if (mockServer_) mockServer_->Stop();
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

    hBtnNewDb_ = CreateWindowExW(0, L"BUTTON", L"+ New Connection", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 120, 26, hWnd_, reinterpret_cast<HMENU>(3002), GetModuleHandle(nullptr), nullptr);
    SendMessage(hBtnNewDb_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);

    hBtnConnections_ = CreateWindowExW(0, L"BUTTON", L"Connection tools...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 100, 30, hWnd_, reinterpret_cast<HMENU>(3006), GetModuleHandle(nullptr), nullptr);
    hEditObjectFilter_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 30, hWnd_, reinterpret_cast<HMENU>(3005), GetModuleHandle(nullptr), nullptr);
    SendMessageW(hEditObjectFilter_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter tables / collections..."));
    hListTables_ = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(3003), GetModuleHandle(nullptr), nullptr);
    // SQL Console Controls
    hEditSql_ = CodeEditor::Create(hWnd_, 3101, EditorLanguage::Sql);
    CodeEditor::SetText(hEditSql_, L"SELECT * FROM products LIMIT 10;");
    SendMessage(hEditSql_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    hBtnRunSql_ = CreateWindowExW(0, L"BUTTON", L"Execute SQL", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 95, 26, hWnd_, reinterpret_cast<HMENU>(3102), GetModuleHandle(nullptr), nullptr);
    hBtnFormatSql_ = CreateWindowExW(0, L"BUTTON", L"Format", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 85, 26, hWnd_, reinterpret_cast<HMENU>(3103), GetModuleHandle(nullptr), nullptr);
    hBtnExplain_ = CreateWindowExW(0, L"BUTTON", L"Query tools...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
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
        WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 0, 0, 400, 26, hWnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hBtnSqlExport_ = CreateWindowExW(0, L"BUTTON", L"Save results JSON", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 150, 30, hWnd_, reinterpret_cast<HMENU>(3107), GetModuleHandle(nullptr), nullptr);
    static const wchar_t* tabNames[] = { L"SQL Console", L"Data Grid", L"Schema", L"ER Diagram" };
    for (int i = 0; i < 4; ++i) {
        hTabs_[i] = CreateWindowExW(0, L"BUTTON", tabNames[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 120, 32, hWnd_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(3400 + i)), GetModuleHandle(nullptr), nullptr);
    }

    hListSqlResults_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | WS_BORDER,
        0, 0, 300, 200, hWnd_, reinterpret_cast<HMENU>(3106), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListSqlResults_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessage(hListSqlResults_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    hQueryDocTabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"Query documents", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH | TCS_BUTTONS,
        0, 0, 300, 35, hWnd_, reinterpret_cast<HMENU>(3500), GetModuleHandle(nullptr), nullptr);
    SetWindowTheme(hQueryDocTabs_, L"", L""); SetWindowSubclass(hQueryDocTabs_, QueryTabProc, 92, 0);
    hBtnNewDoc_ = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 35, 35, hWnd_, reinterpret_cast<HMENU>(3501), GetModuleHandle(nullptr), nullptr);
    hBtnCloseDoc_ = CreateWindowExW(0, L"BUTTON", L"x", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 35, 35, hWnd_, reinterpret_cast<HMENU>(3502), GetModuleHandle(nullptr), nullptr);
    hRowLimitLabel_ = CreateWindowExW(0, L"STATIC", L"Rows", WS_CHILD | WS_VISIBLE, 0, 0, 45, 30, hWnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hRowLimit_ = CreateWindowExW(0, L"EDIT", L"1000", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        0, 0, 70, 30, hWnd_, reinterpret_cast<HMENU>(3503), GetModuleHandle(nullptr), nullptr);
    SendMessageW(hRowLimit_, EM_SETLIMITTEXT, 5, 0);
    hResultSet_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 180, 240, hWnd_, reinterpret_cast<HMENU>(3504), GetModuleHandle(nullptr), nullptr);
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
    hGridStatus_ = CreateWindowExW(0, L"STATIC", L"Select a table to browse. Double-click a row to edit.", WS_CHILD | SS_ENDELLIPSIS,
        0, 0, 400, 26, hWnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hBtnDeleteRow_ = CreateWindowExW(0, L"BUTTON", L"Delete Row", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 85, 24, hWnd_, reinterpret_cast<HMENU>(3207), GetModuleHandle(nullptr), nullptr);
    hBtnExportCsv_ = CreateWindowExW(0, L"BUTTON", L"Export...", WS_CHILD | BS_PUSHBUTTON,
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

    hComboPageSize_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 180, hWnd_, reinterpret_cast<HMENU>(3213), GetModuleHandle(nullptr), nullptr);
    for (const wchar_t* choice : {L"10 / page", L"25 / page", L"50 / page", L"100 / page"}) SendMessageW(hComboPageSize_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
    SendMessageW(hComboPageSize_, CB_SETCURSEL, 1, 0);
    hBtnGridRefresh_ = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_TABSTOP, 0, 0, 100, 30, hWnd_, reinterpret_cast<HMENU>(3214), GetModuleHandle(nullptr), nullptr);
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

    lvc.cx = 160; lvc.pszText = const_cast<LPWSTR>(L"Default"); ListView_InsertColumn(hListSchemaCols_, 4, &lvc);
    lvc.cx = 120; lvc.pszText = const_cast<LPWSTR>(L"On update"); ListView_InsertColumn(hListSchemaFks_, 3, &lvc);
    lvc.pszText = const_cast<LPWSTR>(L"On delete"); ListView_InsertColumn(hListSchemaFks_, 4, &lvc);
    lvc.pszText = const_cast<LPWSTR>(L"Origin"); ListView_InsertColumn(hListSchemaIdx_, 2, &lvc);
    lvc.cx = 200; lvc.pszText = const_cast<LPWSTR>(L"Columns"); ListView_InsertColumn(hListSchemaIdx_, 3, &lvc);
    hSchemaTitle_ = CreateWindowExW(0, L"STATIC", L"Select a database object", WS_CHILD, 0, 0, 300, 30, hWnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hBtnCopyDdl_ = CreateWindowExW(0, L"BUTTON", L"Copy DDL", WS_CHILD | WS_TABSTOP, 0, 0, 130, 30, hWnd_, reinterpret_cast<HMENU>(3305), GetModuleHandle(nullptr), nullptr);
    hBtnResetEr_ = CreateWindowExW(0, L"BUTTON", L"Auto arrange", WS_CHILD | WS_TABSTOP, 0, 0, 140, 30, hWnd_, reinterpret_cast<HMENU>(3405), GetModuleHandle(nullptr), nullptr);
    hErStatus_ = CreateWindowExW(0, L"STATIC", L"Drag a card header to move a table. Scroll to explore relationships.", WS_CHILD | SS_ENDELLIPSIS,
        0, 0, 400, 30, hWnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hEditSchemaDdl_ = CodeEditor::Create(hWnd_, 3304, EditorLanguage::Sql, true);
    SendMessage(hEditSchemaDdl_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    LoadWorkspace();
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
    RECT client{}; GetClientRect(hWnd_, &client);
    width_ = client.right; height_ = client.bottom;
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
    CaptureDocument();
    SelectWorkbenchTab(0);
    SetFocus(hEditSql_);
}

void DbStudioView::CaptureDocument() {
    if (changingDocument_ || activeDocument_ >= documents_.size() || !IsWindow(hEditSql_)) return;
    UpdateQueryLanguage();
    auto& document = documents_[activeDocument_];
    document.sql = ToUtf8(CodeEditor::GetText(hEditSql_)); document.databaseId = activeDbId_;
    wchar_t limit[16]{}; GetWindowTextW(hRowLimit_, limit, 16);
    if (*limit) maxRows_ = std::max(1, std::min(10000, _wtoi(limit)));
}
void DbStudioView::UpdateQueryLanguage() {
    EditorLanguage language = EditorLanguage::Sql;
    if (schema_.kind == "document") language = EditorLanguage::Json;
    else if (schema_.kind == "keyvalue") {
        const auto text = CodeEditor::GetText(hEditSql_);
        const size_t first = text.find_first_not_of(L" \t\r\n");
        language = first != std::wstring::npos && text[first] == L'{' ? EditorLanguage::Json : EditorLanguage::Shell;
    }
    if (language != queryLanguage_) { queryLanguage_ = language; CodeEditor::SetLanguage(hEditSql_, language); }
}
void DbStudioView::RebuildDocumentTabs() {
    TabCtrl_DeleteAllItems(hQueryDocTabs_);
    for (int index = 0; index < static_cast<int>(documents_.size()); ++index) {
        const std::wstring title = ToWide(documents_[index].title) + (documents_[index].dirty ? L" *" : L"");
        TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<LPWSTR>(title.c_str());
        TabCtrl_InsertItem(hQueryDocTabs_, index, &item);
    }
    TabCtrl_SetCurSel(hQueryDocTabs_, static_cast<int>(activeDocument_));
    EnableWindow(hBtnCloseDoc_, documents_.size() > 1 && CanNavigateDocuments());
}
void DbStudioView::SelectDocument(size_t index) {
    if (index >= documents_.size() || !CanNavigateDocuments()) return;
    CaptureDocument(); activeDocument_ = index;
    auto& document = documents_[index]; changingDocument_ = true;
    CodeEditor::SetText(hEditSql_, ToWide(document.sql)); changingDocument_ = false;
    lastQueryResult_ = document.result; TabCtrl_SetCurSel(hQueryDocTabs_, static_cast<int>(index)); RenderResults();
    if (!document.databaseId.empty() && document.databaseId != activeDbId_) { activeDbId_ = document.databaseId; activeTable_.clear(); RefreshData(); }
    SaveWorkspace(); SetFocus(hEditSql_);
}
void DbStudioView::NewDocument() {
    if (!CanNavigateDocuments()) return;
    CaptureDocument();
    SqlDocument document; document.id = "query_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()) + "_" + std::to_string(++documentSequence_);
    document.title = "Query " + std::to_string(documents_.size() + 1); document.databaseId = activeDbId_;
    documents_.push_back(std::move(document)); activeDocument_ = documents_.size() - 1;
    changingDocument_ = true; CodeEditor::SetText(hEditSql_, L""); changingDocument_ = false;
    lastQueryResult_ = {}; RebuildDocumentTabs(); RenderResults(); SelectWorkbenchTab(0); SaveWorkspace(); SetFocus(hEditSql_);
}
void DbStudioView::CloseDocument(bool confirm) {
    if (!CanNavigateDocuments() || documents_.size() <= 1) return;
    CaptureDocument(); const auto& document = documents_[activeDocument_];
    if (confirm && document.dirty && MessageBoxW(hWnd_, L"Close this edited query document? Its text remains in executed query history if it has been run.",
        L"Close query", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    if (job_ && job_->kind == Job::Kind::Query && job_->documentId == document.id) job_->cancel.store(true);
    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(activeDocument_));
    activeDocument_ = std::min(activeDocument_, documents_.size() - 1);
    changingDocument_ = true; CodeEditor::SetText(hEditSql_, ToWide(documents_[activeDocument_].sql)); changingDocument_ = false;
    const std::string database = documents_[activeDocument_].databaseId;
    const bool databaseChanged = database != activeDbId_;
    activeDbId_ = database;
    if (databaseChanged) { activeTable_.clear(); RefreshData(); }
    lastQueryResult_ = documents_[activeDocument_].result; RebuildDocumentTabs(); RenderResults(); SaveWorkspace(); SetFocus(hEditSql_);
}
void DbStudioView::CycleDocument(bool backwards) {
    if (documents_.empty()) return;
    SelectDocument((activeDocument_ + documents_.size() + (backwards ? -1 : 1)) % documents_.size());
}
void DbStudioView::LoadWorkspace() {
    try {
        const auto path = std::filesystem::u8path(dbEngine_->WorkspaceDirectory()) / "query-workspace.bin";
        if (std::filesystem::exists(path)) {
            if (std::filesystem::file_size(path) > 128 * 1024 * 1024) throw std::runtime_error("The protected query workspace exceeds the supported size.");
            std::ifstream stream(path, std::ios::binary); if (!stream) throw std::runtime_error("Cannot read the protected query workspace.");
            std::string encrypted((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            const auto state = nlohmann::json::parse(UnprotectWorkspace(encrypted));
            if (state.value("version", 0) != 1) throw std::runtime_error("This query workspace version is unsupported.");
            maxRows_ = std::max(1, std::min(10000, state.value("maxRows", 1000)));
            for (const auto& item : state.value("documents", nlohmann::json::array())) {
                SqlDocument document; document.id = item.at("id").get<std::string>(); document.title = item.at("title").get<std::string>();
                document.databaseId = item.value("databaseId", ""); document.sql = item.value("sql", ""); documents_.push_back(std::move(document));
            }
            activeDocument_ = documents_.empty() ? 0 : std::min(state.value("activeDocument", size_t{}), documents_.size() - 1);
            for (const auto& item : state.value("history", nlohmann::json::array())) {
                queryHistory_.push_back({item.at("sql").get<std::string>(), item.value("databaseId", ""), item.value("timestamp", ""), item.value("error", ""), item.value("success", false)});
                if (queryHistory_.size() >= 50) break;
            }
            const auto savedPositions = state.value("erPositions", nlohmann::json::object());
            for (auto database = savedPositions.begin(); database != savedPositions.end(); ++database) {
                for (auto table = database.value().begin(); table != database.value().end(); ++table)
                    erPositions_[database.key()][table.key()] = {table.value().at("x").get<int>(), table.value().at("y").get<int>()};
            }
        }
    } catch (const std::exception& error) {
        const auto path = std::filesystem::u8path(dbEngine_->WorkspaceDirectory()) / "query-workspace.bin";
        const auto backup = path.wstring() + L".unreadable-" + std::to_wstring(GetTickCount64());
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && !MoveFileW(path.c_str(), backup.c_str())) workspaceWritable_ = false;
        SetWindowTextW(hSqlStatus_, ToWide(std::string(error.what()) + (workspaceWritable_ ? " The original protected file was preserved as a backup." : " The original protected file was preserved; new workspace changes cannot be saved.")).c_str());
        documents_.clear(); queryHistory_.clear(); erPositions_.clear(); activeDocument_ = 0;
    }
    if (documents_.empty()) { SqlDocument document; document.id = "query_1"; document.title = "Query 1"; documents_.push_back(std::move(document)); }
    changingDocument_ = true;
    CodeEditor::SetText(hEditSql_, ToWide(documents_[activeDocument_].sql)); SetWindowTextW(hRowLimit_, std::to_wstring(maxRows_).c_str());
    changingDocument_ = false;
    if (!documents_[activeDocument_].databaseId.empty()) activeDbId_ = documents_[activeDocument_].databaseId;
    RebuildDocumentTabs();
}
void DbStudioView::SaveWorkspace() {
    if (documents_.empty() || !workspaceWritable_) return;
    try {
        CaptureDocument(); nlohmann::json state = {{"version", 1}, {"activeDocument", activeDocument_}, {"maxRows", maxRows_}, {"documents", nlohmann::json::array()}, {"history", nlohmann::json::array()}, {"erPositions", nlohmann::json::object()}};
        for (const auto& document : documents_) state["documents"].push_back({{"id", document.id}, {"title", document.title}, {"databaseId", document.databaseId}, {"sql", document.sql}});
        for (const auto& item : queryHistory_) state["history"].push_back({{"sql", item.sql}, {"databaseId", item.databaseId}, {"timestamp", item.timestamp}, {"error", item.error}, {"success", item.success}});
        for (const auto& database : erPositions_) for (const auto& table : database.second)
            state["erPositions"][database.first][table.first] = {{"x", table.second.x}, {"y", table.second.y}};
        const auto path = std::filesystem::u8path(dbEngine_->WorkspaceDirectory()) / "query-workspace.bin";
        const std::string error = SaveExportAtomically(path.wstring(), ProtectWorkspace(state.dump()), nullptr);
        if (!error.empty()) throw std::runtime_error(error);
    } catch (const std::exception& error) { if (IsWindow(hSqlStatus_)) SetWindowTextW(hSqlStatus_, (L"Workspace save failed: " + ToWide(error.what())).c_str()); }
}
void DbStudioView::RecordQuery(const Job& completed) {
    QueryResult result = completed.query; if (!completed.error.empty()) result.error = completed.error;
    if (result.query.empty()) result.query = completed.sql;
    for (auto& document : documents_) if (document.id == completed.documentId) {
        document.result = result; document.selectedResult = 0;
        if (&document == &documents_[activeDocument_]) lastQueryResult_ = result;
    }
    SYSTEMTIME now{}; GetSystemTime(&now); std::ostringstream timestamp;
    timestamp << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth << '-' << std::setw(2) << now.wDay
        << ' ' << std::setw(2) << now.wHour << ':' << std::setw(2) << now.wMinute << ':' << std::setw(2) << now.wSecond << " UTC";
    queryHistory_.insert(queryHistory_.begin(), {completed.sql, completed.databaseId, timestamp.str(), result.error, result.error.empty()});
    if (queryHistory_.size() > 50) queryHistory_.resize(50);
    SaveWorkspace();
}
void DbStudioView::SaveSqlFile() {
    CaptureDocument(); const auto path = ChooseNativeFile(hWnd_, L"Save query document", L"SQL query (*.sql)\0*.sql\0All files\0*.*\0\0", true, L"query.sql");
    if (path.empty()) return;
    WriteNativeFileAtomic(path, ToUtf8(CodeEditor::GetText(hEditSql_)));
    documents_[activeDocument_].dirty = false; RebuildDocumentTabs(); SaveWorkspace(); if (OnToast) OnToast("Query document saved.");
}
void DbStudioView::ExportQueryResult() {
    if (IsBusy() || !lastQueryResult_.error.empty()) return;
    const auto path = ChooseNativeFile(hWnd_, L"Export selected result set", L"JSON (*.json)\0*.json\0\0", true, L"query-results.json");
    if (path.empty()) return;
    const QueryResultSet* selected = nullptr;
    if (!lastQueryResult_.resultSets.empty()) {
        const int index = documents_[activeDocument_].selectedResult;
        selected = &lastQueryResult_.resultSets[std::max(0, std::min(index, static_cast<int>(lastQueryResult_.resultSets.size()) - 1))];
    }
    const auto rows = selected ? selected->rows : lastQueryResult_.rows;
    const auto typed = selected ? selected->typedRows : lastQueryResult_.typedRows;
    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Export; job->destination = path;
    StartJob(job, [rows, typed](Job& result) {
        const std::string data = typed.empty() ? nlohmann::json(rows).dump(2) : TypedRowsJson(typed).dump(2);
        result.error = SaveExportAtomically(result.destination, data, &result.cancel); result.success = result.error.empty();
    });
}
void DbStudioView::ShowQueryTools() {
    if (IsBusy()) return;
    HMENU menu = CreatePopupMenu(), templates = CreatePopupMenu(), history = CreatePopupMenu();
    const std::string namespaceName = activeDbId_.empty() ? std::string{} : dbEngine_->GetConnectionProfile(activeDbId_).schema;
    const auto commonTemplates = QueryTemplates(schema_, maxRows_, namespaceName);
    AppendMenuW(menu, MF_STRING | (ActiveConnectionIsConnected() ? 0 : MF_GRAYED), 3515, L"Run entire document");
    AppendMenuW(menu, MF_STRING | (schema_.capabilities.explain ? 0 : MF_GRAYED), 3510, L"Explain query plan");
    AppendMenuW(menu, MF_STRING, 3511, L"Save query document...");
    AppendMenuW(menu, MF_STRING | (lastQueryResult_.error.empty() && !lastQueryResult_.columns.empty() ? 0 : MF_GRAYED), 3512, L"Export selected result set...");
    AppendMenuW(menu, MF_STRING, 3513, L"Rename query document...");
    for (size_t index = 0; index < commonTemplates.size(); ++index)
        AppendMenuW(templates, MF_STRING | (commonTemplates[index].writes && (schema_.capabilities.readOnly || !schema_.capabilities.crud) ? MF_GRAYED : 0), 3520 + index, ToWide(commonTemplates[index].name).c_str());
    if (!schema_.tables.empty()) AppendMenuW(templates, MF_SEPARATOR, 0, nullptr);
    for (size_t index = 0; index < schema_.tables.size() && index < 1000; ++index) AppendMenuW(templates, MF_STRING, 3700 + index, ToWide("Query " + schema_.tables[index].name).c_str());
    for (size_t index = 0; index < queryHistory_.size(); ++index) {
        const auto& item = queryHistory_[index]; std::string summary = item.sql; std::replace(summary.begin(), summary.end(), '\n', ' ');
        if (summary.size() > 90) summary = summary.substr(0, 90) + "...";
        AppendMenuW(history, MF_STRING, 3600 + index, ToWide((item.success ? "OK " : "Error ") + item.timestamp + " | " + summary).c_str());
    }
    if (queryHistory_.empty()) AppendMenuW(history, MF_STRING | MF_GRAYED, 0, L"No queries executed");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(templates), L"Query templates"); AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(history), L"Query history");
    AppendMenuW(menu, MF_STRING | (queryHistory_.empty() ? MF_GRAYED : 0), 3514, L"Clear query history");
    RECT bounds{}; GetWindowRect(hBtnExplain_, &bounds);
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, bounds.left, bounds.bottom, 0, hWnd_, nullptr); DestroyMenu(menu);
    if (choice == 3510) RunSqlQuery(true);
    else if (choice == 3515) RunSqlQuery(false, true);
    else if (choice >= 3520 && choice < 3520 + commonTemplates.size()) CodeEditor::SetText(hEditSql_, ToWide(commonTemplates[choice - 3520].text));
    else if (choice == 3511) SaveSqlFile();
    else if (choice == 3512) ExportQueryResult();
    else if (choice == 3513) { auto name = ToWide(documents_[activeDocument_].title); if (EditNativeText(hWnd_, L"Query document name", name, EditorLanguage::PlainText, [](const std::wstring& value) { return value.empty() || value.size() > 80 ? L"Enter a name between 1 and 80 characters." : L""; })) { documents_[activeDocument_].title = ToUtf8(name); RebuildDocumentTabs(); SaveWorkspace(); } }
    else if (choice == 3514 && MessageBoxW(hWnd_, L"Clear saved query history?", L"Query history", MB_YESNO | MB_DEFBUTTON2) == IDYES) { queryHistory_.clear(); SaveWorkspace(); }
    else if (choice >= 3600 && choice < 3600 + queryHistory_.size()) {
        const auto item = queryHistory_[choice - 3600]; NewDocument(); documents_[activeDocument_].databaseId = item.databaseId;
        CodeEditor::SetText(hEditSql_, ToWide(item.sql)); if (!item.databaseId.empty() && item.databaseId != activeDbId_) { activeDbId_ = item.databaseId; activeTable_.clear(); RefreshData(); }
    } else if (choice >= 3700 && choice < 3700 + schema_.tables.size()) {
        const std::string table = schema_.tables[choice - 3700].name;
        const std::string sql = SelectTemplate(table, schema_, maxRows_, dbEngine_->GetConnectionProfile(activeDbId_).schema);
        CodeEditor::SetText(hEditSql_, ToWide(sql));
    }
}
void DbStudioView::StartJob(const std::shared_ptr<Job>& job, std::function<void(Job&)> work) {
    if (IsBusy() || Closing(hWnd_)) return;
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
    const bool queryIsCurrent = complete->kind == Job::Kind::Query && activeDocument_ < documents_.size()
        && documents_[activeDocument_].id == complete->documentId;
    if (complete->kind == Job::Kind::Query) RecordQuery(*complete);
    if (Closing(hWnd_)) { reloadPending_ = false; UpdateEnabledState(); return; }
    if (!complete->error.empty()) {
        if (complete->kind != Job::Kind::Query || queryIsCurrent) {
            SetWindowTextW(hSqlStatus_, ToWide(complete->error).c_str());
            SetWindowTextW(hGridStatus_, ToWide(complete->error).c_str());
        }
        if (complete->kind == Job::Kind::Query) RenderResults();
        else if (complete->kind == Job::Kind::Grid) { currentTableData_ = {}; currentTableData_.error = complete->error; RenderGrid(); }
        if (OnToast && (complete->kind != Job::Kind::Query || queryIsCurrent)) OnToast(complete->error);
    } else if (complete->kind == Job::Kind::Schema) {
        schema_ = std::move(complete->schema);
        PopulateTables();
        if (workbenchTab_ == 1) RefreshDataGrid();
        else if (workbenchTab_ == 2) RefreshSchemaViewer();
        else if (workbenchTab_ == 3) RefreshErDiagram();
        if (!lastQueryResult_.query.empty()) RenderResults();
        else SetWindowTextW(hSqlStatus_, L"Ctrl+Enter runs selected text or the entire editor. Ctrl+Space opens completion.");
    } else if (complete->kind == Job::Kind::Query) {
        RenderResults();
        if (complete->query.error.empty() && complete->databaseId == activeDbId_) RefreshData();
    } else if (complete->kind == Job::Kind::Grid) {
        currentTableData_ = std::move(complete->table);
        gridPage_ = currentTableData_.page;
        RenderGrid();
        if (!lastQueryResult_.query.empty()) RenderResults();
        else SetWindowTextW(hSqlStatus_, L"Ctrl+Enter executes SQL. Ctrl+Space opens completion.");
    } else if (complete->kind == Job::Kind::Mutation) {
        if (complete->success) { SetWindowTextW(hGridStatus_, ToWide(complete->sql.empty() ? "Row saved." : complete->sql).c_str()); RefreshData(); }
        else { SetWindowTextW(hGridStatus_, L"Row could not be saved. Check constraints, required fields, and primary keys.");
            MessageBoxW(hWnd_, L"The row could not be saved. Check required values, unique constraints, and foreign keys. No successful change was reported.", L"Database change failed", MB_OK | MB_ICONERROR); }
    } else if (complete->kind == Job::Kind::Action) {
        if (complete->success) { if (OnToast) OnToast(complete->sql); RefreshData(); }
        else { SetWindowTextW(hSqlStatus_, L"The connection operation did not succeed."); }
    } else if (complete->kind == Job::Kind::Export) {
        if (complete->success) {
            SetWindowTextW(hSqlStatus_, L"Export saved successfully."); SetWindowTextW(hGridStatus_, L"Export saved successfully.");
            if (OnToast) OnToast("Export saved successfully.");
        }
    }
    UpdateEnabledState();
    if (reloadPending_ && !IsBusy()) { reloadPending_ = false; RefreshData(); }
}

bool DbStudioView::ActiveConnectionIsConnected() const {
    return std::any_of(databases_.begin(), databases_.end(), [this](const auto& database) { return database.id == activeDbId_ && database.connected; });
}
bool DbStudioView::CanNavigateDocuments() const {
    return !Closing(hWnd_) && (!job_ || job_->kind == Job::Kind::Query);
}
void DbStudioView::UpdateEnabledState() {
    const bool available = !IsBusy() && !Closing(hWnd_);
    const HWND controls[] = { hListDatabases_, hListTables_, hBtnNewDb_, hBtnFormatSql_, hBtnExplain_,
        hComboTemplates_, hComboGridTable_, hEditGridSearch_, hBtnGridSearch_, hBtnExportCsv_, hBtnExportJson_, hBtnExportSql_, hRowLimit_, hComboPageSize_, hBtnGridRefresh_, hBtnConnections_, hEditObjectFilter_ };
    for (HWND control : controls) EnableWindow(control, available);
    const bool connected = ActiveConnectionIsConnected();
    EnableWindow(hBtnRunSql_, !Closing(hWnd_) && (IsBusy() || connected));
    SetWindowTextW(hBtnRunSql_, IsBusy() ? L"Cancel" : schema_.kind == "sql" ? L"Run SQL" : L"Execute");
    EnableWindow(hBtnSqlExport_, available && lastQueryResult_.error.empty() && !lastQueryResult_.columns.empty());
    EnableWindow(hQueryDocTabs_, CanNavigateDocuments()); EnableWindow(hBtnNewDoc_, CanNavigateDocuments());
    EnableWindow(hBtnCloseDoc_, CanNavigateDocuments() && documents_.size() > 1);
    EnableWindow(hResultSet_, available && lastQueryResult_.resultSets.size() > 1);
    EnableWindow(hBtnGridPrev_, available && gridPage_ > 1);
    EnableWindow(hBtnGridNext_, available && gridPage_ < currentTableData_.totalPages);
    bool editable = false;
    for (const auto& table : schema_.tables) if (table.name == activeTable_) editable = table.editable && table.type != "view" && schema_.capabilities.crud && !schema_.capabilities.readOnly;
    const int selection = ListView_GetNextItem(hListDataGrid_, -1, LVNI_SELECTED);
    EnableWindow(hBtnAddRow_, available && editable && !currentTableData_.columns.empty() && currentTableData_.error.empty());
    const bool identifiable = available && editable && !currentTableData_.primaryKeys.empty() && selection >= 0;
    bool rowEditable = identifiable;
    if (rowEditable && schema_.kind == "keyvalue") {
        rowEditable = selection < static_cast<int>(currentTableData_.rows.size()) && currentTableData_.rows[selection].count("type")
            && currentTableData_.rows[selection].at("type") == "string";
    }
    EnableWindow(hBtnEditRow_, rowEditable);
    EnableWindow(hBtnDeleteRow_, identifiable);
}

void DbStudioView::RefreshData() {
    if (Closing(hWnd_)) { reloadPending_ = false; return; }
    if (IsBusy()) { reloadPending_ = true; return; }
    PopulateDatabases();
    if (activeDbId_.empty()) { schema_ = {}; PopulateTables(); UpdateEnabledState(); return; }
    for(const auto& database:databases_)if(database.id==activeDbId_&&!database.connected){schema_={};activeTable_.clear();PopulateTables();RefreshSchemaViewer();RefreshErDiagram();SetWindowTextW(hSqlStatus_,L"Connection is disconnected. Open Connection tools and choose Connect.");UpdateEnabledState();return;}
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Schema;
    const std::string database = activeDbId_;
    DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database](Job& result) { result.schema = engine->GetSchema(database, &result.cancel); });
}

void DbStudioView::PopulateDatabases() {
    populating_ = true;
    ListView_DeleteAllItems(hListDatabases_);
    databases_ = dbEngine_->ListDatabases();
    int active = -1;
    for (int i = 0; i < static_cast<int>(databases_.size()); ++i) {
        const auto& database = databases_[i];
        const std::wstring name = ToWide(database.name + " (" + database.type + ")") + (database.isSample ? L" (Sample)" : database.connected ? L"" : L" [disconnected]");
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = i; item.pszText = const_cast<LPWSTR>(name.c_str());
        ListView_InsertItem(hListDatabases_, &item);
        if (database.id == activeDbId_) active = i;
    }
    if (active < 0 && autoSelectDatabase_) { for (int index = 0; index < static_cast<int>(databases_.size()); ++index) if (databases_[index].connected) { active = index; activeDbId_ = databases_[index].id; activeTable_.clear(); break; } }
    if (active >= 0) { ListView_SetItemState(hListDatabases_, active, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
    else activeDbId_.clear();
    populating_ = false;
}

void DbStudioView::PopulateTables() {
    populating_ = true;
    TreeView_DeleteAllItems(hListTables_);
    SendMessageW(hComboGridTable_, CB_RESETCONTENT, 0, 0);
    SendMessageW(hComboTemplates_, CB_RESETCONTENT, 0, 0);
    SendMessageW(hComboTemplates_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Templates..."));
    std::vector<std::wstring> completions;
    std::wstring filter; const int length = GetWindowTextLengthW(hEditObjectFilter_);
    filter.resize(length + 1); GetWindowTextW(hEditObjectFilter_, filter.data(), length + 1); filter.resize(length);
    std::transform(filter.begin(), filter.end(), filter.begin(), [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
    int active = -1; HTREEITEM activeItem = nullptr;
    for (int index = 0; index < static_cast<int>(schema_.tables.size()); ++index) {
        const auto& table = schema_.tables[index]; const std::wstring name = ToWide(table.name);
        std::wstring folded = name; std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
        HTREEITEM node = nullptr;
        if (filter.empty() || folded.find(filter) != std::wstring::npos) {
            const std::wstring label = name + (table.rowCount < 0 ? L" (count unavailable)" : L" (" + std::to_wstring(table.rowCount) + L")");
            TVINSERTSTRUCTW entry{}; entry.hParent = TVI_ROOT; entry.hInsertAfter = TVI_LAST; entry.item.mask = TVIF_TEXT | TVIF_PARAM;
            entry.item.pszText = const_cast<LPWSTR>(label.c_str()); entry.item.lParam = index + 1;
            node = TreeView_InsertItem(hListTables_, &entry);
            for (const auto& column : table.columns) {
                const std::wstring text = (column.pk ? L"[PK] " : L"") + ToWide(column.name) + L" : " + ToWide(column.type);
                entry.hParent = node; entry.item.pszText = const_cast<LPWSTR>(text.c_str()); TreeView_InsertItem(hListTables_, &entry);
            }
        }
        SendMessageW(hComboGridTable_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        const std::wstring label = L"Query " + name; SendMessageW(hComboTemplates_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        completions.push_back(name);
        for (const auto& column : table.columns) { completions.push_back(ToWide(column.name)); completions.push_back(name + L"." + ToWide(column.name)); }
        if (table.name == activeTable_) { active = index; activeItem = node; }
    }
    if (active < 0 && !schema_.tables.empty()) { active = 0; activeTable_ = schema_.tables[0].name; gridPage_ = 1; gridSortCol_.clear(); }
    if (active < 0) { activeTable_.clear(); currentTableData_ = {}; RenderGrid(); }
    SendMessageW(hComboGridTable_, CB_SETCURSEL, active, 0); SendMessageW(hComboTemplates_, CB_SETCURSEL, 0, 0);
    if (activeItem) TreeView_SelectItem(hListTables_, activeItem);
    const bool document = schema_.kind == "document", keyValue = schema_.kind == "keyvalue";
    UpdateQueryLanguage();
    if (keyValue) for (const wchar_t* command : {L"GET", L"SET", L"DEL", L"SCAN", L"HGETALL", L"HSET", L"LPUSH", L"LRANGE", L"SADD", L"SMEMBERS", L"ZRANGE", L"TTL", L"EXPIRE", L"TYPE", L"command", L"args"}) completions.emplace_back(command);
    if (document) for (const wchar_t* word : {L"collection", L"operation", L"find", L"find_one", L"insert_one", L"update_one", L"delete_one", L"aggregate", L"count", L"distinct", L"filter", L"pipeline", L"projection", L"sort", L"limit", L"document", L"update", L"upsert", L"$match", L"$group", L"$set"}) completions.emplace_back(word);
    CodeEditor::SetCompletions(hEditSql_, completions);
    CodeEditor::SetSqlSchema(hEditSql_,schema_.tables,schema_.dialect);
    SetWindowTextW(hTabs_[0], document ? L"JSON Queries" : keyValue ? L"Commands" : L"SQL Console");
    EnableWindow(hTabs_[1], schema_.capabilities.dataGrid); EnableWindow(hTabs_[2], schema_.capabilities.schema); EnableWindow(hTabs_[3], schema_.capabilities.erDiagram);
    populating_ = false;
}
void DbStudioView::RunSqlQuery(bool explain, bool entireDocument) {
    if (Closing(hWnd_)) { RequestCancel(); return; }
    if (IsBusy()) { RequestCancel(); return; }
    if (activeDbId_.empty()) return;
    if (!ActiveConnectionIsConnected()) { SetWindowTextW(hSqlStatus_, L"This connection is disconnected. Choose Connect in Connection tools before executing a query."); return; }
    CaptureDocument();
    std::wstring text = CodeEditor::GetText(hEditSql_);
    DWORD start = 0, end = 0;
    SendMessageW(hEditSql_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (!entireDocument && end > start && end <= text.size()) text = text.substr(start, end - start);
    const std::string sql = ToUtf8(text);
    if (sql.find_first_not_of(" \t\r\n") == std::string::npos) return;
    if (!explain && DestructiveQuery(sql, schema_.kind) && MessageBoxW(hWnd_, L"This query can modify or remove database data or objects. Execute it?",
        L"Confirm query execution", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    auto job = std::make_shared<Job>(); job->kind = Job::Kind::Query;
    job->documentId = documents_[activeDocument_].id; job->databaseId = activeDbId_; job->sql = sql;
    const int maximumRows = maxRows_;
    const std::string database = activeDbId_;
    DbEngine* engine = dbEngine_;
    StartJob(job, [engine, database, sql, explain, maximumRows](Job& result) {
        result.query = explain ? engine->ExplainQuery(database, sql, &result.cancel) : engine->ExecuteQuery(database, sql, maximumRows, &result.cancel);
    });
}

static void FillRows(HWND list, const std::vector<std::string>& columns,
    const std::vector<std::map<std::string, std::string>>& rows, const std::vector<std::string>& primaryKeys = {}, const std::vector<TypedRow>& typedRows = {}) {
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
            std::wstring text = value == rows[r].end() ? L"" : ToWide(value->second);
            if (r < static_cast<int>(typedRows.size())) {
                const auto field = typedRows[r].find(columns[c]);
                if (field != typedRows[r].end()) {
                    if (field->second.type == DbValueType::Null) text = L"[NULL]";
                    else if (field->second.type == DbValueType::Blob) text = L"[BLOB " + std::to_wstring(field->second.text.size()) + L" bytes]";
                }
            }
            if (c == 0) { LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = r; item.pszText = const_cast<LPWSTR>(text.c_str()); ListView_InsertItem(list, &item); }
            else ListView_SetItemText(list, r, c, const_cast<LPWSTR>(text.c_str()));
        }
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void DbStudioView::RenderResults() {
    SendMessageW(hResultSet_, CB_RESETCONTENT, 0, 0);
    if (!lastQueryResult_.error.empty()) {
        FillRows(hListSqlResults_, {"Execution error"}, {{{"Execution error", lastQueryResult_.error}}});
        ListView_SetColumnWidth(hListSqlResults_, 0, Theme::Scale(600));
        SetWindowTextW(hSqlStatus_, ToWide(lastQueryResult_.error).c_str()); UpdateEnabledState(); return;
    }
    const QueryResultSet* selected = nullptr;
    if (!lastQueryResult_.resultSets.empty()) {
        for (int index = 0; index < static_cast<int>(lastQueryResult_.resultSets.size()); ++index) {
            const auto& result = lastQueryResult_.resultSets[index];
            const std::wstring label = L"Result " + std::to_wstring(index + 1) + L" (" + std::to_wstring(result.rows.size()) + L" rows)";
            SendMessageW(hResultSet_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        auto& index = documents_[activeDocument_].selectedResult;
        index = std::max(0, std::min(index, static_cast<int>(lastQueryResult_.resultSets.size()) - 1));
        SendMessageW(hResultSet_, CB_SETCURSEL, index, 0); selected = &lastQueryResult_.resultSets[index];
    } else {
        SendMessageW(hResultSet_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Query results")); SendMessageW(hResultSet_, CB_SETCURSEL, 0, 0);
    }
    FillRows(hListSqlResults_, selected ? selected->columns : lastQueryResult_.columns, selected ? selected->rows : lastQueryResult_.rows,
        {}, selected ? selected->typedRows : lastQueryResult_.typedRows);
    std::wostringstream status;
    status << L"Returned " << (selected ? selected->rows.size() : lastQueryResult_.rows.size()) << L" rows; affected " << lastQueryResult_.rowsAffected
        << L"; " << static_cast<int>(lastQueryResult_.executionTimeMs) << L" ms";
    if (selected ? selected->truncated : lastQueryResult_.truncated) status << L". Truncated at " << maxRows_ << L" rows.";
    if (lastQueryResult_.query.empty()) status.str(L"Ctrl+Enter runs SQL. Ctrl+Space opens completion.");
    SetWindowTextW(hSqlStatus_, status.str().c_str()); UpdateEnabledState();
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
        result.table = engine->GetTableData(database, table, page, pageSize, sort, direction, filter, "", "", &result.cancel);
    });
}

void DbStudioView::RenderGrid() {
    FillRows(hListDataGrid_, currentTableData_.columns, currentTableData_.rows, currentTableData_.primaryKeys, currentTableData_.typedRows);
    std::wstring status;
    if (!currentTableData_.error.empty()) status = ToWide(currentTableData_.error);
    else status = L"Page " + std::to_wstring(currentTableData_.page) + L" of " + std::to_wstring(currentTableData_.totalPages)
        + L" | " + std::to_wstring(currentTableData_.totalRows) + L" matching rows | Click a column to sort; double-click a row to edit.";
    if (currentTableData_.error.empty() && schema_.kind == "keyvalue" && currentTableData_.truncated)
        status += L" Complex values are previewed; export fetches complete values.";
    SetWindowTextW(hGridStatus_, status.c_str());
    UpdateEnabledState();
}

void DbStudioView::EditGridRow(bool add) {
    if(IsBusy()||activeTable_.empty()||schema_.capabilities.readOnly||!schema_.capabilities.crud)return;
    const int selected=ListView_GetNextItem(hListDataGrid_,-1,LVNI_SELECTED);
    if(!add&&(selected<0||selected>=static_cast<int>(currentTableData_.rows.size())||currentTableData_.primaryKeys.empty()))return;
    TypedRow original,primary,changes;
    if(!add){if(selected<static_cast<int>(currentTableData_.typedRows.size()))original=currentTableData_.typedRows[selected];else for(const auto& field:currentTableData_.rows[selected])original[field.first]={DbValueType::Text,field.second};
        for(const auto& key:currentTableData_.primaryKeys){const auto value=original.find(key);if(value==original.end())return;primary[key]=value->second;}}
    if (!add && schema_.kind == "keyvalue" && (!original.count("type") || original.at("type").text != "string")) {
        if (OnToast) OnToast("Use the native command editor to edit a non-string Redis value."); return;
    }
    std::vector<std::string> readOnlyColumns;
    if (schema_.kind == "keyvalue") { readOnlyColumns = {"type", "ttl"}; if (!add) readOnlyColumns.push_back("key"); }
    else if (!add && schema_.kind == "document") readOnlyColumns = currentTableData_.primaryKeys;
    if(!ShowTypedRowEditorDialog(hWnd_,currentTableData_,original,add,changes,readOnlyColumns)||(!add&&changes.empty()))return;
    auto job=std::make_shared<Job>();job->kind=Job::Kind::Mutation;job->sql=add?"Row inserted.":"Row updated.";
    const std::string database=activeDbId_,table=activeTable_;DbEngine* engine=dbEngine_;
    StartJob(job,[engine,database,table,primary,changes,add](Job& result){result.success=add?engine->InsertTypedRow(database,table,changes,&result.cancel):engine->UpdateTypedRow(database,table,primary,changes,&result.cancel);});
}
void DbStudioView::DeleteGridRow() {
    if(IsBusy()||currentTableData_.primaryKeys.empty()||schema_.capabilities.readOnly||!schema_.capabilities.crud)return;
    const int selected=ListView_GetNextItem(hListDataGrid_,-1,LVNI_SELECTED);if(selected<0||selected>=static_cast<int>(currentTableData_.rows.size()))return;
    if(MessageBoxW(hWnd_,L"Delete the selected row permanently?",L"Delete row",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
    TypedRow row,primary;if(selected<static_cast<int>(currentTableData_.typedRows.size()))row=currentTableData_.typedRows[selected];else for(const auto& field:currentTableData_.rows[selected])row[field.first]={DbValueType::Text,field.second};
    for(const auto& key:currentTableData_.primaryKeys){const auto value=row.find(key);if(value==row.end())return;primary[key]=value->second;}
    auto job=std::make_shared<Job>();job->kind=Job::Kind::Mutation;job->sql="Row deleted.";const std::string database=activeDbId_,table=activeTable_;DbEngine* engine=dbEngine_;
    StartJob(job,[engine,database,table,primary](Job& result){result.success=engine->DeleteTypedRow(database,table,primary,&result.cancel);});
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
        const std::string data = engine->ExportTableData(database, table, format, filter, sort, direction, &result.cancel);
        if (result.cancel.load()) { result.error = "Export canceled."; return; }
        if (data.empty()) { result.error = "The database did not return an export. Check that the table exists and is readable."; return; }
        result.error = SaveExportAtomically(result.destination, data, &result.cancel);
        result.success = result.error.empty();
    });
}

void DbStudioView::OpenConnection(bool editing) {
    if(IsBusy()||Closing(hWnd_))return;ConnectionProfile profile;
    if(editing){if(activeDbId_.empty())return;profile=dbEngine_->GetConnectionProfile(activeDbId_);}
    ShowConnectionDialog(hWnd_,dbEngine_,profile,[this](const DatabaseInfo& database,bool connect){if(connect){activeDbId_=database.id;activeTable_.clear();autoSelectDatabase_=true;}RefreshData();if(OnToast)OnToast(connect?"Connection saved and connected.":"Connection settings saved.");});
}
void DbStudioView::ShowConnectionTools() {
    if(IsBusy()||Closing(hWnd_))return;HMENU menu=CreatePopupMenu();const UINT active=activeDbId_.empty()?MF_GRAYED:0;
    AppendMenuW(menu,MF_STRING|active,3556,L"Connect current connection");
    AppendMenuW(menu,MF_STRING|active,3550,L"Edit current connection...");AppendMenuW(menu,MF_STRING|active,3551,L"Disconnect current connection");AppendMenuW(menu,MF_STRING|active,3552,L"Remove connection...");
    AppendMenuW(menu,MF_STRING|active,3553,L"Refresh database metadata");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,3554,L"Create local database / sample preset...");AppendMenuW(menu,MF_STRING,3555,L"Local mock REST API...");
    RECT bounds{};GetWindowRect(hBtnConnections_,&bounds);const UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN,bounds.left,bounds.bottom,0,hWnd_,nullptr);DestroyMenu(menu);
    if(choice==3550)OpenConnection(true);else if(choice==3553)RefreshData();else if(choice==3554){if(OnOpenNewDbDialog)OnOpenNewDbDialog();}else if(choice==3555)ShowMockServer();
    else if(choice==3556){const auto profile=dbEngine_->GetConnectionProfile(activeDbId_);DbEngine* engine=dbEngine_;auto job=std::make_shared<Job>();job->kind=Job::Kind::Action;job->sql="Connection established.";StartJob(job,[engine,profile](Job& result){engine->SaveConnection(profile,true,&result.cancel);result.success=true;});}
    else if(choice==3551||choice==3552){if(choice==3552&&MessageBoxW(hWnd_,L"Remove this connection from the workspace? Database files and remote data are preserved.",L"Remove connection",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;
        const std::string database=activeDbId_;DbEngine* engine=dbEngine_;auto job=std::make_shared<Job>();job->kind=Job::Kind::Action;job->sql=choice==3552?"Connection removed; database data preserved.":"Connection disconnected.";
        if(choice==3552)activeDbId_.clear();activeTable_.clear();autoSelectDatabase_=false;schema_={};PopulateTables();RenderResults();
        StartJob(job,[engine,database,choice](Job& result){if(choice==3552)result.success=engine->RemoveConnection(database);else result.success=engine->DisconnectDatabase(database);});
    }
}
void DbStudioView::ShowMockServer() {
    if(Closing(hWnd_))return;if(!mockServer_)mockServer_=std::make_unique<NativeMockServer>(dbEngine_);ShowMockServerDialog(hWnd_,mockServer_.get(),activeDbId_,activeTable_);
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
    if (activeTable_.empty()) { ListView_DeleteAllItems(hListSchemaCols_); ListView_DeleteAllItems(hListSchemaFks_); ListView_DeleteAllItems(hListSchemaIdx_); CodeEditor::SetText(hEditSchemaDdl_, L""); SetWindowTextW(hSchemaTitle_,L"No database object selected."); return; }
    SetWindowTextW(hSchemaTitle_, ToWide("Object: " + activeTable_).c_str());
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
                const std::wstring defaultValue = ToWide(col.dfltValue); ListView_SetItemText(hListSchemaCols_, i, 4, const_cast<LPWSTR>(defaultValue.c_str()));
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
                const std::wstring update = ToWide(fk.onUpdate), deletion = ToWide(fk.onDelete);
                ListView_SetItemText(hListSchemaFks_, i, 3, const_cast<LPWSTR>(update.c_str())); ListView_SetItemText(hListSchemaFks_, i, 4, const_cast<LPWSTR>(deletion.c_str()));
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
                const std::wstring origin = ToWide(idx.origin); std::string joined;
                for (const auto& column : idx.columns) { if (!joined.empty()) joined += ", "; joined += column; }
                const std::wstring columns = ToWide(joined); ListView_SetItemText(hListSchemaIdx_, i, 2, const_cast<LPWSTR>(origin.c_str())); ListView_SetItemText(hListSchemaIdx_, i, 3, const_cast<LPWSTR>(columns.c_str()));
            }
            break;
        }
    }
}

void DbStudioView::RefreshErDiagram() {
    if (erDiagram_.databaseId != activeDbId_) selectedErTable_.clear();
    erDiagram_.nodes.clear(); erDiagram_.links.clear(); erDiagram_.databaseId = activeDbId_;
    for (const auto& table : schema_.tables) {
        ERNode node; node.id = table.name; node.name = table.name; node.columns = table.columns; node.rowCount = table.rowCount;
        erDiagram_.nodes.push_back(std::move(node));
        for (const auto& key : table.foreignKeys) {
            ERLink link; link.id = table.name + "." + key.fromColumn + ":" + std::to_string(erDiagram_.links.size()); link.source = table.name; link.sourceCol = key.fromColumn;
            link.target = key.targetTable; link.targetCol = key.toColumn; link.onUpdate = key.onUpdate; link.onDelete = key.onDelete;
            erDiagram_.links.push_back(std::move(link));
        }
    }
    if (std::none_of(erDiagram_.nodes.begin(), erDiagram_.nodes.end(), [this](const auto& node) { return node.name == selectedErTable_; })) selectedErTable_.clear();
    hoveredErLink_ = -1; erRoutesDirty_ = true;
    EnsureErPositions(); UpdateErScrollbars(); UpdateErStatus(); InvalidateRect(hWnd_, nullptr, TRUE);
}
void DbStudioView::EnsureErPositions(bool reset) {
    auto& positions = erPositions_[activeDbId_]; if (reset) { positions.clear(); erOffsetX_ = erOffsetY_ = 0; }
    if (!reset && std::all_of(erDiagram_.nodes.begin(), erDiagram_.nodes.end(), [&positions](const auto& node) { return positions.find(node.name) != positions.end(); })) return;
    std::vector<ErLayoutNode> nodes; std::vector<ErLayoutEdge> links;
    for (const auto& node : erDiagram_.nodes) nodes.push_back({node.name, 235, 38 + static_cast<int>(node.columns.size()) * 24});
    for (const auto& link : erDiagram_.links) links.push_back({link.source, link.target});
    const auto arranged = ComputeErLayout(nodes, links);
    // Existing saved/dragged cards keep their locations. New cards must also avoid them.
    std::vector<RECT> occupied;
    for (const auto& node : nodes) {
        const auto position = positions.find(node.id);
        if (position != positions.end()) occupied.push_back({position->second.x, position->second.y, position->second.x + node.width, position->second.y + node.height});
    }
    for (const auto& node : nodes) {
        if (positions.find(node.id) != positions.end()) continue;
        const auto found = arranged.find(node.id); if (found == arranged.end()) continue;
        POINT point{found->second.x, found->second.y};
        for (;;) {
            RECT card{point.x - 24, point.y - 24, point.x + node.width + 24, point.y + node.height + 24};
            int nextY = point.y;
            for (const auto& obstacle : occupied) { RECT intersection{}; if (IntersectRect(&intersection, &card, &obstacle)) nextY = std::max<int>(nextY, obstacle.bottom + 48); }
            if (nextY == point.y) break;
            point.y = nextY;
        }
        positions[node.id] = point; occupied.push_back({point.x, point.y, point.x + node.width, point.y + node.height});
    }
    erRoutesDirty_ = true;
}
void DbStudioView::RebuildErRoutes() {
    if (!erRoutesDirty_) return;
    std::vector<ErRouteCard> cards; std::vector<ErRouteConnection> links;
    const auto& positions = erPositions_[activeDbId_];
    for (const auto& node : erDiagram_.nodes) {
        const auto position = positions.find(node.name); if (position == positions.end()) continue;
        cards.push_back({node.name, {position->second.x, position->second.y, position->second.x + 235, position->second.y + 38 + static_cast<int>(node.columns.size()) * 24}});
    }
    const auto rowOffset = [this](const std::string& table, const std::string& column) {
        for (const auto& node : erDiagram_.nodes) if (node.name == table) {
            for (size_t index = 0; index < node.columns.size(); ++index) if (node.columns[index].name == column) return 44 + static_cast<int>(index) * 24;
        }
        return 15; // Providers can omit the column name from foreign-key metadata.
    };
    for (const auto& link : erDiagram_.links) links.push_back({link.source, link.target, rowOffset(link.source, link.sourceCol), rowOffset(link.target, link.targetCol)});
    erRoutes_ = RouteErConnections(cards, links); erRoutesDirty_ = false;
    erCrossings_.clear();
    struct Segment { ErRoutePoint a, b; size_t route; };
    std::vector<Segment> horizontal, vertical;
    // This optional visual pass is bounded independently of path search.
    constexpr size_t maximumSegments = 4096, maximumChecks = 100000, maximumCrossings = 2048;
    for (size_t index = 0; index < erRoutes_.routes.size() && horizontal.size() + vertical.size() < maximumSegments; ++index) {
        const auto& points = erRoutes_.routes[index].points;
        for (size_t segment = 1; segment < points.size() && horizontal.size() + vertical.size() < maximumSegments; ++segment) {
            auto a = points[segment - 1], b = points[segment];
            if (a.y == b.y) { if (a.x > b.x) std::swap(a, b); horizontal.push_back({a, b, index}); }
            else { if (a.y > b.y) std::swap(a, b); vertical.push_back({a, b, index}); }
        }
    }
    std::sort(horizontal.begin(), horizontal.end(), [](const auto& a, const auto& b) { return a.a.y < b.a.y; });
    size_t checks = 0;
    for (const auto& v : vertical) {
        auto h = std::upper_bound(horizontal.begin(), horizontal.end(), v.a.y + 6, [](int y, const auto& segment) { return y < segment.a.y; });
        for (; h != horizontal.end() && h->a.y < v.b.y - 6 && checks < maximumChecks && erCrossings_.size() < maximumCrossings; ++h, ++checks) {
            // Keep bends, row ports and shared endpoint stubs intact.
            if (h->route != v.route && h->a.x + 6 < v.a.x && v.a.x < h->b.x - 6) erCrossings_.push_back({{v.a.x, h->a.y}, h->route, v.route});
        }
        if (checks >= maximumChecks || erCrossings_.size() >= maximumCrossings) break;
    }
}
void DbStudioView::UpdateErStatus() {
    std::wstring status;
    if (hoveredErLink_ >= 0 && hoveredErLink_ < static_cast<int>(erDiagram_.links.size())) {
        const auto& link = erDiagram_.links[hoveredErLink_]; status = ToWide(link.source + "." + link.sourceCol + " -> " + link.target + "." + link.targetCol);
    } else if (!selectedErTable_.empty()) {
        const auto count = std::count_if(erDiagram_.links.begin(), erDiagram_.links.end(), [this](const auto& link) { return link.source == selectedErTable_ || link.target == selectedErTable_; });
        status = ToWide(selectedErTable_) + L": " + std::to_wstring(count) + L" relationships highlighted. Drag its header to move it.";
    } else status = std::to_wstring(erDiagram_.nodes.size()) + L" objects, " + std::to_wstring(erDiagram_.links.size()) + L" relationships. Click a table to trace its links.";
    const auto unresolved = std::count_if(erRoutes_.routes.begin(), erRoutes_.routes.end(), [](const auto& route) { return !route.routed; });
    if (unresolved) status += L" " + std::to_wstring(unresolved) + (erRoutes_.limited ? L" exceed routing limits." : L" could not be routed; try Auto arrange.");
    SetWindowTextW(hErStatus_, status.c_str());
}
void DbStudioView::UpdateErScrollbars() {
    RebuildErRoutes();
    const int availableHeight = std::max(1, height_ - erCanvasY_ - Theme::Scale(6));
    const int availableWidth = std::max(1, width_ - erCanvasX_ - Theme::Scale(10));
    erExtentW_ = erExtentH_ = 0;
    const auto& positions = erPositions_[activeDbId_];
    for (const auto& node : erDiagram_.nodes) {
        const auto found = positions.find(node.name); if (found == positions.end()) continue;
        erExtentW_ = std::max(erExtentW_, Theme::Scale(found->second.x + 235));
        erExtentH_ = std::max(erExtentH_, Theme::Scale(found->second.y + 38 + static_cast<int>(node.columns.size()) * 24));
    }
    erExtentW_ = std::max(erExtentW_, Theme::Scale(erRoutes_.extentRight + 24));
    erExtentH_ = std::max(erExtentH_, Theme::Scale(erRoutes_.extentBottom + 24));
    const bool diagram = workbenchTab_ == 3;
    ShowScrollBar(hWnd_, SB_HORZ, diagram && erExtentW_ > availableWidth);
    ShowScrollBar(hWnd_, SB_VERT, diagram && erExtentH_ > availableHeight);
    erOffsetX_ = std::max(0, std::min(erOffsetX_, std::max(0, erExtentW_ - availableWidth)));
    erOffsetY_ = std::max(0, std::min(erOffsetY_, std::max(0, erExtentH_ - availableHeight)));
    SCROLLINFO scroll{}; scroll.cbSize = sizeof(scroll); scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMax = std::max(0, erExtentW_ - 1); scroll.nPage = availableWidth; scroll.nPos = erOffsetX_; SetScrollInfo(hWnd_, SB_HORZ, &scroll, TRUE);
    scroll.nMax = std::max(0, erExtentH_ - 1); scroll.nPage = availableHeight; scroll.nPos = erOffsetY_; SetScrollInfo(hWnd_, SB_VERT, &scroll, TRUE);
}
void DbStudioView::Layout() {
    if (width_ <= 0 || height_ <= 0 || layoutInProgress_) return;
    struct LayoutGuard { bool& active; explicit LayoutGuard(bool& value) : active(value) { active = true; } void Release() { active = false; } ~LayoutGuard() { Release(); } } guard(layoutInProgress_);
    const int initialWidth = width_, initialHeight = height_;
    const auto scale = [](int value) { return Theme::Scale(value); };
    TEXTMETRICW metrics{}; HDC dc = GetDC(hWnd_); const auto previous = SelectObject(dc, Theme::GetMainFont());
    GetTextMetricsW(dc, &metrics); SelectObject(dc, previous); ReleaseDC(hWnd_, dc);
    const int gap = scale(6), margin = scale(10), row = std::max(scale(24), static_cast<int>(metrics.tmHeight) + scale(8));
    const int statusHeight = static_cast<int>(metrics.tmHeight) + scale(4);
    sidebarWidth_ = std::min(scale(265), std::max(scale(170), width_ / 4));
    const auto place = [](HWND control, int x, int y, int width, int height) { SetWindowPos(control, nullptr, x, y, std::max(1, width), std::max(1, height), SWP_NOZORDER | SWP_NOACTIVATE); };
    const int databaseHeight = std::min(scale(130), height_ / 4);
    place(hListDatabases_, gap, gap, sidebarWidth_ - gap * 2, databaseHeight);
    place(hBtnNewDb_, gap, databaseHeight + gap * 2, sidebarWidth_ - gap * 2, row);
    place(hBtnConnections_, gap, databaseHeight + gap * 3 + row, sidebarWidth_ - gap * 2, row);
    const int filterY = databaseHeight + row * 2 + gap * 4;
    place(hEditObjectFilter_, gap, filterY, sidebarWidth_ - gap * 2, row);
    const int tablesY = filterY + row + gap;
    place(hListTables_, gap, tablesY, sidebarWidth_ - gap * 2, height_ - tablesY - gap);
    ListView_SetColumnWidth(hListDatabases_, 0, sidebarWidth_ - scale(26));
    const int mainX = sidebarWidth_ + margin, contentW = std::max(1, width_ - mainX - margin), tabWidth = contentW / 4;
    for (int index = 0; index < 4; ++index) place(hTabs_[index], mainX + index * tabWidth, gap, tabWidth - scale(2), row);
    const int contentY = row + gap * 2, contentHeight = height_ - contentY - gap;
    const int documentToolsWidth = row * 2 + scale(45) + scale(65) + gap * 4;
    const int documentWidth = std::max(1, contentW - documentToolsWidth);
    place(hQueryDocTabs_, mainX, contentY, documentWidth, row);
    SendMessageW(hQueryDocTabs_, TCM_SETITEMSIZE, 0, MAKELPARAM(scale(110), row - scale(3)));
    int x = mainX + documentWidth + gap;
    place(hBtnNewDoc_, x, contentY, row, row); x += row + gap;
    place(hBtnCloseDoc_, x, contentY, row, row); x += row + gap;
    place(hRowLimitLabel_, x, contentY + scale(3), scale(45), row); x += scale(45) + gap;
    place(hRowLimit_, x, contentY, scale(65), row);
    const std::pair<HWND,int> sqlControls[] = {{hBtnRunSql_,95}, {hBtnFormatSql_,80}, {hBtnExplain_,120}};
    int toolbarRows = 1, used = 0;
    for (const auto& control : sqlControls) { const int width = std::min(scale(control.second), contentW); if (used && used + width > contentW) { ++toolbarRows; used = 0; } used += width + gap; }
    const int toolbarHeight = toolbarRows * row + (toolbarRows - 1) * gap;
    const int editorY = contentY + row + gap;
    const int resultMinimum = std::max(scale(48), static_cast<int>(metrics.tmHeight) * 3);
    const int editorBudget = contentHeight - row - toolbarHeight - statusHeight - resultMinimum - gap * 4;
    const int editorHeight = std::max(1, std::min(scale(260), std::min(contentHeight / 3, editorBudget)));
    place(hEditSql_, mainX, editorY, contentW, editorHeight);
    x = mainX; int y = editorY + editorHeight + gap;
    for (const auto& control : sqlControls) { const int width = std::min(scale(control.second), contentW); if (x > mainX && x + width > mainX + contentW) { x = mainX; y += row + gap; } place(control.first, x, y, width, row); x += width + gap; }
    const int sqlStatusY = y + row + gap;
    const int resultSelectorWidth = std::min(scale(165), contentW / 2);
    place(hSqlStatus_, mainX, sqlStatusY, contentW - resultSelectorWidth - gap, statusHeight);
    place(hResultSet_, mainX + contentW - resultSelectorWidth, sqlStatusY, resultSelectorWidth, scale(240));
    const int resultY = sqlStatusY + statusHeight + gap;
    place(hListSqlResults_, mainX, resultY, contentW, height_ - resultY - gap);
    x = mainX; y = contentY;
    const std::pair<HWND,int> gridControls[] = {{hComboGridTable_,155}, {hEditGridSearch_,120}, {hBtnGridSearch_,60}, {hBtnGridPrev_,75}, {hBtnGridNext_,75},
        {hBtnAddRow_,80}, {hBtnEditRow_,80}, {hBtnDeleteRow_,90}, {hBtnExportCsv_,85}, {hBtnGridRefresh_,85}, {hComboPageSize_,85}};
    for (const auto& control : gridControls) { const int width = std::min(scale(control.second), contentW); if (x > mainX && x + width > mainX + contentW) { x = mainX; y += row + gap; }
        place(control.first, x, y, width, control.first == hComboGridTable_ || control.first == hComboPageSize_ ? scale(240) : row); x += width + gap; }
    const int gridStatusY = y + row + gap; place(hGridStatus_, mainX, gridStatusY, contentW, statusHeight);
    const int gridY = gridStatusY + statusHeight + gap; place(hListDataGrid_, mainX, gridY, contentW, height_ - gridY - gap);
    place(hSchemaTitle_, mainX, contentY, contentW - scale(130) - gap, row); place(hBtnCopyDdl_, mainX + contentW - scale(130), contentY, scale(130), row);
    const int schemaY = contentY + row + gap, schemaHeight = height_ - schemaY - gap;
    const int halfWidth = (contentW - gap) / 2, halfHeight = (schemaHeight - gap) / 2;
    place(hListSchemaCols_, mainX, schemaY, halfWidth, halfHeight);
    place(hListSchemaFks_, mainX + halfWidth + gap, schemaY, halfWidth, halfHeight / 2);
    place(hListSchemaIdx_, mainX + halfWidth + gap, schemaY + halfHeight / 2 + gap, halfWidth, halfHeight / 2 - gap);
    place(hEditSchemaDdl_, mainX, schemaY + halfHeight + gap, contentW, halfHeight);
    place(hErStatus_, mainX, contentY, contentW - scale(140) - gap, row); place(hBtnResetEr_, mainX + contentW - scale(140), contentY, scale(140), row);
    erCanvasX_ = mainX; erCanvasY_ = contentY + row + gap;
    UpdateWorkbenchTabsVisibility(); guard.Release(); if (width_ != initialWidth || height_ != initialHeight) Layout();
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
    ShowWindow(hComboTemplates_, SW_HIDE);
    ShowWindow(hListSqlResults_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hSqlStatus_, isSql ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnSqlExport_, SW_HIDE);
    for (HWND control : {hQueryDocTabs_, hBtnNewDoc_, hBtnCloseDoc_, hRowLimit_, hRowLimitLabel_, hResultSet_}) ShowWindow(control, isSql ? SW_SHOW : SW_HIDE);

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
    ShowWindow(hBtnExportJson_, SW_HIDE);
    ShowWindow(hBtnExportSql_, SW_HIDE);
    ShowWindow(hBtnGridRefresh_, isGrid ? SW_SHOW : SW_HIDE); ShowWindow(hComboPageSize_, isGrid ? SW_SHOW : SW_HIDE);
    ShowWindow(hListDataGrid_, isGrid ? SW_SHOW : SW_HIDE);

    bool isSchema = (workbenchTab_ == 2);
    ShowWindow(hListSchemaCols_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hListSchemaFks_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hListSchemaIdx_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditSchemaDdl_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hSchemaTitle_, isSchema ? SW_SHOW : SW_HIDE); ShowWindow(hBtnCopyDdl_, isSchema ? SW_SHOW : SW_HIDE);
    ShowWindow(hErStatus_, workbenchTab_ == 3 ? SW_SHOW : SW_HIDE); ShowWindow(hBtnResetEr_, workbenchTab_ == 3 ? SW_SHOW : SW_HIDE);
}

void DbStudioView::Draw(HDC dc) {
    const auto& colors = Theme::Get(); RECT client{0,0,width_,height_}; FillRect(dc,&client,Theme::GetBgPrimaryBrush());
    RECT sidebar{0,0,sidebarWidth_,height_}; FillRect(dc,&sidebar,Theme::GetBgSecondaryBrush());
    HPEN separator = CreatePen(PS_SOLID,1,colors.borderColor); const auto originalPen = SelectObject(dc,separator);
    MoveToEx(dc,sidebarWidth_,0,nullptr); LineTo(dc,sidebarWidth_,height_);
    if (workbenchTab_ == 3) {
        const int saved = SaveDC(dc); IntersectClipRect(dc,erCanvasX_,erCanvasY_,width_,height_);
        erCardRects_.clear(); const auto& positions = erPositions_[activeDbId_];
        for (const auto& node : erDiagram_.nodes) {
            const auto position = positions.find(node.name); if (position == positions.end()) continue;
            const int x = erCanvasX_ + Theme::Scale(position->second.x) - erOffsetX_, y = erCanvasY_ + Theme::Scale(position->second.y) - erOffsetY_;
            erCardRects_[node.name] = {x,y,x+Theme::Scale(235),y+Theme::Scale(38+static_cast<int>(node.columns.size())*24)};
        }
        const auto screenPoint = [this](const ErRoutePoint& point) { return POINT{erCanvasX_ + Theme::Scale(point.x) - erOffsetX_, erCanvasY_ + Theme::Scale(point.y) - erOffsetY_}; };
        const auto emphasized = [this](size_t index) {
            const auto& link = erDiagram_.links[index];
            return static_cast<int>(index) == hoveredErLink_ || (!selectedErTable_.empty() && (link.source == selectedErTable_ || link.target == selectedErTable_));
        };
        const auto routeColor = [&](size_t index) { return (!selectedErTable_.empty() || hoveredErLink_ >= 0) && !emphasized(index) ? colors.textMuted : colors.accent; };
        const auto paintRoute = [&](size_t index, bool markers) {
            const auto& route = erRoutes_.routes[index]; if (!route.routed || route.points.size() < 2) return;
            const bool highlight = emphasized(index);
            const COLORREF color = routeColor(index);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Theme::Scale(highlight ? 2 : 1)), color);
            const auto previousPen = SelectObject(dc, pen);
            if (!markers) {
                std::vector<POINT> points; points.reserve(route.points.size());
                for (const auto& point : route.points) points.push_back(screenPoint(point));
                Polyline(dc, points.data(), static_cast<int>(points.size()));
            } else {
                const POINT source = screenPoint(route.source.point), target = screenPoint(route.target.point);
                const int outward = route.target.side == ErRouteSide::Left ? -1 : 1;
                POINT arrow[3]{{target.x, target.y}, {target.x + outward * Theme::Scale(8), target.y - Theme::Scale(4)}, {target.x + outward * Theme::Scale(8), target.y + Theme::Scale(4)}};
                HBRUSH brush = CreateSolidBrush(color); const auto previousBrush = SelectObject(dc, brush); Polygon(dc, arrow, 3);
                SelectObject(dc, Theme::GetBgSecondaryBrush()); const int radius = Theme::Scale(3);
                Ellipse(dc, source.x - radius, source.y - radius, source.x + radius + 1, source.y + radius + 1);
                SelectObject(dc, previousBrush); DeleteObject(brush);
            }
            SelectObject(dc, previousPen); DeleteObject(pen);
        };
        // Draw the selected relationships last so they remain traceable at crossings.
        for (int pass = 0; pass < 2; ++pass) for (size_t index = 0; index < erRoutes_.routes.size(); ++index) if (emphasized(index) == (pass == 1)) paintRoute(index, false);
        std::map<std::pair<int, int>, std::pair<size_t, bool>> crossingTops;
        const auto priority = [&](size_t index, bool horizontal) { return (static_cast<int>(index) == hoveredErLink_ ? 4 : emphasized(index) ? 2 : 0) + (horizontal ? 1 : 0); };
        for (const auto& crossing : erCrossings_) {
            const auto key = std::make_pair(crossing.point.x, crossing.point.y);
            for (const auto candidate : {std::make_pair(crossing.horizontal, true), std::make_pair(crossing.vertical, false)}) {
                const auto found = crossingTops.find(key);
                if (found == crossingTops.end() || priority(candidate.first, candidate.second) > priority(found->second.first, found->second.second)) crossingTops[key] = candidate;
            }
        }
        if (!crossingTops.empty()) {
            HPEN erasePen = CreatePen(PS_SOLID, 1, colors.bgPrimary); HBRUSH eraseBrush = CreateSolidBrush(colors.bgPrimary);
            const auto previousPen = SelectObject(dc, erasePen), previousBrush = SelectObject(dc, eraseBrush);
            const int radius = Theme::Scale(4);
            for (const auto& crossing : crossingTops) {
                const POINT point = screenPoint({crossing.first.first, crossing.first.second});
                SelectObject(dc, erasePen); Ellipse(dc, point.x - radius, point.y - radius, point.x + radius + 1, point.y + radius + 1);
                HPEN topPen = CreatePen(PS_SOLID, std::max(1, Theme::Scale(emphasized(crossing.second.first) ? 2 : 1)), routeColor(crossing.second.first));
                SelectObject(dc, topPen);
                const bool horizontal = crossing.second.second;
                MoveToEx(dc, point.x - (horizontal ? radius + 1 : 0), point.y - (horizontal ? 0 : radius + 1), nullptr);
                LineTo(dc, point.x + (horizontal ? radius + 2 : 0), point.y + (horizontal ? 0 : radius + 2));
                SelectObject(dc, erasePen); DeleteObject(topPen);
            }
            SelectObject(dc, previousPen); SelectObject(dc, previousBrush); DeleteObject(erasePen); DeleteObject(eraseBrush);
        }
        SetBkMode(dc,TRANSPARENT);
        for (const auto& node : erDiagram_.nodes) {
            const auto found = erCardRects_.find(node.name); if(found==erCardRects_.end())continue; const RECT bounds=found->second;
            HPEN selectedPen = node.name == selectedErTable_ ? CreatePen(PS_SOLID, std::max(1, Theme::Scale(2)), colors.accent) : nullptr;
            if (selectedPen) SelectObject(dc, selectedPen);
            HBRUSH background=CreateSolidBrush(colors.bgSecondary); const auto oldBrush=SelectObject(dc,background); RoundRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,Theme::Scale(6),Theme::Scale(6)); SelectObject(dc,oldBrush); DeleteObject(background);
            RECT header{bounds.left,bounds.top,bounds.right,bounds.top+Theme::Scale(30)}; FillRect(dc,&header,Theme::GetBgTertiaryBrush());
            SelectObject(dc,Theme::GetBoldFont()); SetTextColor(dc,colors.textPrimary); InflateRect(&header,-Theme::Scale(8),0); DrawTextW(dc,ToWide(node.name).c_str(),-1,&header,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            int y=bounds.top+Theme::Scale(32); SelectObject(dc,Theme::GetSmallFont());
            for(const auto& column:node.columns) { RECT line{bounds.left+Theme::Scale(8),y,bounds.right-Theme::Scale(8),y+Theme::Scale(24)}; SetTextColor(dc,column.pk?colors.methodPost:colors.textSecondary);
                DrawTextW(dc,ToWide((column.pk?"[PK] ":"")+column.name+" ("+column.type+")").c_str(),-1,&line,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS); y+=Theme::Scale(24); }
            if (selectedPen) { SelectObject(dc, separator); DeleteObject(selectedPen); }
        }
        for (int pass = 0; pass < 2; ++pass) for (size_t index = 0; index < erRoutes_.routes.size(); ++index) if (emphasized(index) == (pass == 1)) paintRoute(index, true);
        RestoreDC(dc,saved);
    }
    SelectObject(dc,originalPen); DeleteObject(separator);
}
LRESULT CALLBACK DbStudioView::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DbStudioView* pThis = reinterpret_cast<DbStudioView*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    try { switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<DbStudioView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    case WM_PRINTCLIENT:
        if (pThis) pThis->Draw(reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_SIZE:
        if (pThis && pThis->hEditSql_) {
            pThis->width_ = LOWORD(lParam); pThis->height_ = HIWORD(lParam);
            pThis->Layout();
        }
        return 0;
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
        pThis->hoveredErLink_ = -1; pThis->UpdateErScrollbars(); pThis->UpdateErStatus(); InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (pThis && pThis->workbenchTab_ == 3) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * Theme::Scale(72);
            if ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) || pThis->erExtentH_ <= std::max(1, pThis->height_ - pThis->erCanvasY_ - Theme::Scale(6))) pThis->erOffsetX_ -= delta;
            else pThis->erOffsetY_ -= delta;
            pThis->hoveredErLink_ = -1; pThis->UpdateErScrollbars(); pThis->UpdateErStatus(); InvalidateRect(hWnd, nullptr, TRUE); return 0;
        }
        break;
    case WM_TIMER:
        if (pThis && wParam == 71) { pThis->PollJob(); return 0; }
        if (pThis && wParam == 72) { KillTimer(hWnd,72); pThis->SaveWorkspace(); return 0; }
        if (pThis && wParam == 73) { KillTimer(hWnd,73); if(!pThis->IsBusy()){pThis->gridPage_=1;pThis->RefreshDataGrid();} return 0; }
        break;
    case WM_COMMAND: {
        if (!pThis) break;
        if (Closing(hWnd)) return 0;
        const WORD id = LOWORD(wParam), code = HIWORD(wParam);
        try {
            if (id >= 3400 && id <= 3403) pThis->SelectWorkbenchTab(id - 3400);
            else if (id == 3102) pThis->RunSqlQuery();
            else if (id == 3103 && !pThis->IsBusy()) {
                CodeEditor::SetText(pThis->hEditSql_, ToWide(pThis->dbEngine_->FormatQuery(ToUtf8(CodeEditor::GetText(pThis->hEditSql_)), pThis->schema_.kind)));
            } else if (id == 3104 && !pThis->IsBusy()) pThis->ShowQueryTools();
            else if (id == 3105 && code == CBN_SELCHANGE && !pThis->IsBusy()) {
                const int selected = static_cast<int>(SendMessageW(pThis->hComboTemplates_, CB_GETCURSEL, 0, 0)) - 1;
                if (selected >= 0 && selected < static_cast<int>(pThis->schema_.tables.size())) {
                    std::string table = pThis->schema_.tables[selected].name;
                    CodeEditor::SetText(pThis->hEditSql_, ToWide(SelectTemplate(table, pThis->schema_, pThis->maxRows_, pThis->dbEngine_->GetConnectionProfile(pThis->activeDbId_).schema)));
                }
            } else if (id == 3107 && !pThis->IsBusy()) pThis->ExportQueryResult();
            else if (id == 3002 && !pThis->IsBusy()) pThis->OpenConnection();
            else if (id == 3006 && !pThis->IsBusy()) pThis->ShowConnectionTools();
            else if (id == 3005 && code == EN_CHANGE && !pThis->populating_) pThis->PopulateTables();
            else if (id == 3501) pThis->NewDocument();
            else if (id == 3502) pThis->CloseDocument();
            else if (id == 3503 && code == EN_KILLFOCUS) { pThis->CaptureDocument(); SetWindowTextW(pThis->hRowLimit_, std::to_wstring(pThis->maxRows_).c_str()); pThis->SaveWorkspace(); }
            else if (id == 3504 && code == CBN_SELCHANGE && !pThis->IsBusy()) { pThis->documents_[pThis->activeDocument_].selectedResult = static_cast<int>(SendMessageW(pThis->hResultSet_, CB_GETCURSEL, 0, 0)); pThis->RenderResults(); }
            else if (id == 3101 && code == EN_CHANGE && !pThis->changingDocument_ && pThis->activeDocument_ < pThis->documents_.size()) {
                const auto before = pThis->documents_[pThis->activeDocument_].sql; pThis->CaptureDocument();
                if (before != pThis->documents_[pThis->activeDocument_].sql) { pThis->documents_[pThis->activeDocument_].dirty = true; pThis->RebuildDocumentTabs(); SetTimer(hWnd, 72, 700, nullptr); }
            } else if (id == 3202 && code == EN_CHANGE && !pThis->IsBusy()) SetTimer(hWnd, 73, 300, nullptr);
            else if (id == 3213 && code == CBN_SELCHANGE && !pThis->IsBusy()) { static constexpr int pageSizes[] = {10,25,50,100}; const int choice = static_cast<int>(SendMessageW(pThis->hComboPageSize_, CB_GETCURSEL, 0, 0)); if (choice >= 0 && choice < 4) { pThis->gridPageSize_ = pageSizes[choice]; pThis->gridPage_ = 1; pThis->RefreshDataGrid(); } }
            else if (id == 3214 && !pThis->IsBusy()) pThis->RefreshDataGrid();
            else if (id == 3305) { if (!CopyNativeText(hWnd, CodeEditor::GetText(pThis->hEditSchemaDdl_))) throw std::runtime_error("Clipboard is currently unavailable."); if (pThis->OnToast) pThis->OnToast("DDL copied."); }
            else if (id == 3405) { pThis->hoveredErLink_ = -1; pThis->EnsureErPositions(true); pThis->UpdateErScrollbars(); pThis->UpdateErStatus(); pThis->SaveWorkspace(); InvalidateRect(hWnd,nullptr,TRUE); }
            else if (id == 3201 && code == CBN_SELCHANGE && !pThis->IsBusy()) {
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
            else if (id == 3208 && !pThis->IsBusy()) {
                HMENU menu=CreatePopupMenu(); AppendMenuW(menu,MF_STRING,3208,L"CSV"); AppendMenuW(menu,MF_STRING,3209,L"JSON"); AppendMenuW(menu,MF_STRING|(pThis->schema_.capabilities.sqlExport?0:MF_GRAYED),3210,L"SQL INSERT statements");
                RECT bounds{};GetWindowRect(pThis->hBtnExportCsv_,&bounds);const UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN,bounds.left,bounds.bottom,0,hWnd,nullptr);DestroyMenu(menu);if(choice)pThis->ExportTable(choice==3208?"csv":choice==3209?"json":"sql");
            }
            else if (id == 3209) pThis->ExportTable("json");
            else if (id == 3210) pThis->ExportTable("sql");
        } catch (const std::exception& error) {
            MessageBoxW(hWnd, ToWide(error.what()).c_str(), L"Database operation failed", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_DRAWITEM: {
        if (!pThis) break;
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlID != 3500 || item->itemID >= pThis->documents_.size()) break;
        const auto& colors = Theme::Get();
        const bool selected = item->itemID == pThis->activeDocument_;
        FillRect(item->hDC, &item->rcItem, selected ? Theme::GetBgTertiaryBrush() : Theme::GetBgSecondaryBrush());
        SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, selected ? colors.textPrimary : colors.textSecondary);
        const auto previous = SelectObject(item->hDC, selected ? Theme::GetBoldFont() : Theme::GetMainFont());
        RECT text = item->rcItem; InflateRect(&text, -Theme::Scale(8), 0);
        const auto& document = pThis->documents_[item->itemID];
        DrawTextW(item->hDC, ToWide(document.title + (document.dirty ? " *" : "")).c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (selected) { RECT accent=item->rcItem; accent.top=accent.bottom-Theme::Scale(2); HBRUSH brush=CreateSolidBrush(colors.accent); FillRect(item->hDC,&accent,brush);DeleteObject(brush); }
        if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC,&text);
        SelectObject(item->hDC,previous); return TRUE;
    }
    case WM_NOTIFY: {
        if (!pThis || Closing(hWnd)) return 0;
        const auto* notification = reinterpret_cast<NMHDR*>(lParam);
        if (notification->idFrom == 3500 && notification->code == TCN_SELCHANGE) {
            const int index=TabCtrl_GetCurSel(pThis->hQueryDocTabs_); if(index>=0)pThis->SelectDocument(static_cast<size_t>(index));
        } else if (notification->idFrom == 3003 && (notification->code == TVN_SELCHANGEDW || notification->code == TVN_SELCHANGEDA) && !pThis->populating_ && !pThis->IsBusy()) {
            const auto* selection=reinterpret_cast<NMTREEVIEWW*>(lParam); const int index=static_cast<int>(selection->itemNew.lParam)-1;
            if(index>=0 && index<static_cast<int>(pThis->schema_.tables.size())) {
                pThis->activeTable_=pThis->schema_.tables[index].name; pThis->gridPage_=1; pThis->gridSortCol_.clear();
                SendMessageW(pThis->hComboGridTable_,CB_SETCURSEL,index,0);
                if(pThis->workbenchTab_==2)pThis->RefreshSchemaViewer();
                else if(pThis->workbenchTab_!=3)pThis->SelectWorkbenchTab(1);
            }
        } else if (notification->idFrom == 3211) {
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
        } else if (notification->idFrom == 3001 && notification->code == LVN_ITEMCHANGED && !pThis->populating_ && !pThis->IsBusy()) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
            if (!(change->uNewState & LVIS_SELECTED) || (change->uOldState & LVIS_SELECTED)) return 0;
            if (change->iItem >= 0 && change->iItem < static_cast<int>(pThis->databases_.size())) {
                pThis->CaptureDocument(); pThis->autoSelectDatabase_=true;
                pThis->activeDbId_ = pThis->databases_[change->iItem].id;
                if (pThis->activeDocument_ < pThis->documents_.size()) pThis->documents_[pThis->activeDocument_].databaseId=pThis->activeDbId_;
                pThis->activeTable_.clear(); pThis->gridPage_ = 1; pThis->gridSortCol_.clear(); pThis->RefreshData(); pThis->SaveWorkspace();
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if(pThis && pThis->workbenchTab_==3 && !pThis->IsBusy()) {
            const POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            if(point.x>=pThis->erCanvasX_ && point.y>=pThis->erCanvasY_) {
                pThis->hoveredErLink_ = -1; pThis->selectedErTable_.clear();
                for(auto card=pThis->erCardRects_.rbegin();card!=pThis->erCardRects_.rend();++card) {
                    if (!PtInRect(&card->second, point)) continue;
                    pThis->selectedErTable_ = card->first;
                    if (point.y < card->second.top + Theme::Scale(30)) { pThis->draggedTable_=card->first;pThis->dragStart_=point;pThis->dragPosition_=pThis->erPositions_[pThis->activeDbId_][card->first];SetCapture(hWnd); }
                    break;
                }
                pThis->UpdateErStatus(); InvalidateRect(hWnd, nullptr, FALSE); return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if(pThis && !pThis->draggedTable_.empty() && GetCapture()==hWnd) {
            const int scale=std::max(1,Theme::Scale(100));
            POINT& position=pThis->erPositions_[pThis->activeDbId_][pThis->draggedTable_];
            position.x=std::max<int>(8,pThis->dragPosition_.x+MulDiv(GET_X_LPARAM(lParam)-pThis->dragStart_.x,100,scale));
            position.y=std::max<int>(8,pThis->dragPosition_.y+MulDiv(GET_Y_LPARAM(lParam)-pThis->dragStart_.y,100,scale));
            pThis->erRoutesDirty_ = true; pThis->UpdateErScrollbars(); pThis->UpdateErStatus(); InvalidateRect(hWnd,nullptr,FALSE);return 0;
        }
        if (pThis && pThis->workbenchTab_ == 3) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hovered = -1;
            bool overCard = false; for (const auto& card : pThis->erCardRects_) if (PtInRect(&card.second, point)) { overCard = true; break; }
            if (!overCard && point.x >= pThis->erCanvasX_ && point.y >= pThis->erCanvasY_) {
                const int x = MulDiv(point.x - pThis->erCanvasX_ + pThis->erOffsetX_, 100, std::max(1, Theme::Scale(100)));
                const int y = MulDiv(point.y - pThis->erCanvasY_ + pThis->erOffsetY_, 100, std::max(1, Theme::Scale(100)));
                int distance = 7;
                for (size_t index = 0; index < pThis->erRoutes_.routes.size(); ++index) {
                    const auto& route = pThis->erRoutes_.routes[index];
                    for (size_t segment = 1; segment < route.points.size(); ++segment) {
                        const auto& a = route.points[segment-1]; const auto& b = route.points[segment];
                        int candidate = 1000000;
                        if (a.x == b.x && y >= std::min(a.y, b.y) && y <= std::max(a.y, b.y)) candidate = std::abs(x - a.x);
                        if (a.y == b.y && x >= std::min(a.x, b.x) && x <= std::max(a.x, b.x)) candidate = std::abs(y - a.y);
                        if (candidate < distance) { distance = candidate; hovered = static_cast<int>(index); }
                    }
                }
            }
            if (pThis->hoveredErLink_ != hovered) { pThis->hoveredErLink_ = hovered; pThis->UpdateErStatus(); InvalidateRect(hWnd, nullptr, FALSE); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hWnd, 0}; TrackMouseEvent(&track);
        }
        break;
    case WM_MOUSELEAVE:
        if (pThis && pThis->hoveredErLink_ != -1) { pThis->hoveredErLink_ = -1; pThis->UpdateErStatus(); InvalidateRect(hWnd, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP:
        if(pThis && !pThis->draggedTable_.empty()){pThis->draggedTable_.clear();ReleaseCapture();pThis->SaveWorkspace();return 0;}
        break;
    case WM_CAPTURECHANGED:
        if(pThis && !pThis->draggedTable_.empty()){pThis->draggedTable_.clear();pThis->SaveWorkspace();} return 0;
    case WM_DESTROY:
        KillTimer(hWnd, 71); KillTimer(hWnd,72); KillTimer(hWnd,73);
        if (pThis) { pThis->SaveWorkspace(); pThis->RequestCancel(); }
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
    } catch (const std::exception& error) {
        if (pThis && IsWindow(pThis->hSqlStatus_)) SetWindowTextW(pThis->hSqlStatus_, ToWide(error.what()).c_str());
        return 0;
    } catch (...) { return 0; }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool DbStudioView::RunSelfTests(std::wstring& failure) {
    const auto check = [&failure](bool condition, const wchar_t* label) { if (!condition) failure += std::wstring(label) + L"; "; return condition; };
    bool passed = RunModalSelfTests(failure);
    DatabaseSchema templateSchema; templateSchema.dialect = "postgresql";
    passed &= check(SelectTemplate("order\"items", templateSchema, 1000, "sales\"reports")
        == "SELECT * FROM \"sales\"\"reports\".\"order\"\"items\" LIMIT 25;", L"query template qualifies and escapes a configured PostgreSQL schema");
    templateSchema.dialect = "mssql";
    passed &= check(SelectTemplate("order]items", templateSchema, 10, "sales]reports")
        == "SELECT TOP 10 * FROM [sales]]reports].[order]]items];", L"query template uses escaped SQL Server schema and row limit");
    passed &= check(!DestructiveQuery("SELECT 'DELETE FROM example'; -- DROP example", "sql")
        && DestructiveQuery("DELETE FROM example;", "sql"), L"query confirmation separates executable SQL from literals and comments");
    passed &= check(DestructiveQuery(R"({"collection":"items","operation":"delete_one","filter":{}})", "document")
        && DestructiveQuery(R"({"collection":"items","operation":"aggregate","pipeline":[{"$merge":"archived"}]})", "document")
        && !DestructiveQuery(R"({"collection":"items","operation":"find","filter":{"message":"delete_one"}})", "document"),
        L"Mongo write and aggregate queries preserve the original confirmation behavior");
    passed &= check(DestructiveQuery(" del example", "keyvalue")
        && DestructiveQuery(R"({"command":"FLUSHDB","args":[]})", "keyvalue")
        && !DestructiveQuery("GET delete_example", "keyvalue"), L"Redis destructive commands are recognized in command and JSON inputs");
    templateSchema = {}; templateSchema.kind = "document";
    const auto mongoTemplates = QueryTemplates(templateSchema, 20, "");
    passed &= check(mongoTemplates.size() == 4 && nlohmann::json::parse(mongoTemplates[1].text)["pipeline"][0]["$limit"] == 20
        && nlohmann::json::parse(mongoTemplates[2].text)["operation"] == "count"
        && nlohmann::json::parse(mongoTemplates[3].text)["operation"] == "insert_one" && mongoTemplates[3].writes,
        L"Mongo templates preserve aggregate, count and native insert contracts");
    templateSchema.kind = "keyvalue";
    const auto redisTemplates = QueryTemplates(templateSchema, 1000, "");
    passed &= check(redisTemplates.size() == 3 && redisTemplates[1].text == "INFO" && redisTemplates[2].text == "DBSIZE",
        L"Redis templates preserve scan, info and size workflows");
    std::filesystem::path workspace;
    HWND parent = nullptr;
    bool ownsWorkspace = false;
    const int originalScale = Theme::GetTextScale();
    const UINT originalDpi = static_cast<UINT>(MulDiv(Theme::Scale(96), 100, originalScale));
    struct RestoreTheme {
        UINT dpi; int scale;
        ~RestoreTheme() { Theme::SetDpi(dpi); Theme::SetTextScale(scale); }
    } restoreTheme{ originalDpi, originalScale };
    try {
        wchar_t temporary[32768]{};
        if (!GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary)) throw std::runtime_error("Cannot locate temporary directory");
        workspace = std::filesystem::path(temporary) / (L"DataForge-db-ui-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(workspace)) throw std::runtime_error("Cannot create isolated test directory");
        ownsWorkspace = true;
        DbEngine engine(ToUtf8(workspace.wstring()), false);
        engine.CreateDatabase("UI acceptance", "blank");
        passed &= RunConnectionModalSelfTests(&engine, failure);
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
            view.LoadSql("-- paragraph comment\nSELECT 55 AS n;\n-- second paragraph\nSELECT 66 AS n;"); view.Execute();
            passed &= check(waitForCompletion() && view.lastQueryResult_.resultSets.size() == 2
                && !view.lastQueryResult_.resultSets[0].rows.empty() && view.lastQueryResult_.resultSets[0].rows.front().at("n") == "55"
                && !view.lastQueryResult_.resultSets[1].rows.empty() && view.lastQueryResult_.resultSets[1].rows.front().at("n") == "66",
                L"native editor paragraph comments terminate before following SQL statements");
            view.LoadSql("SELECT 11 AS n; SELECT 22 AS n;"); view.Execute();
            passed &= check(waitForCompletion() && view.lastQueryResult_.resultSets.size() == 2, L"query batch retains two native result sets");
            SendMessageW(view.hEditSql_, EM_SETSEL, 0, 15); view.Execute();
            passed &= check(waitForCompletion() && view.lastQueryResult_.resultSets.size() == 1 && !view.lastQueryResult_.rows.empty() && view.lastQueryResult_.rows[0].at("n") == "11",
                L"Execute runs the selected statement");
            view.RunSqlQuery(false, true);
            passed &= check(waitForCompletion() && view.lastQueryResult_.resultSets.size() == 2,
                L"Run entire document executes both statements despite an active selection");
            SendMessageW(view.hEditSql_, EM_SETSEL, 0, 0);
            SendMessageW(view.hResultSet_,CB_SETCURSEL,1,0);SendMessageW(view.hWnd_,WM_COMMAND,MAKEWPARAM(3504,CBN_SELCHANGE),reinterpret_cast<LPARAM>(view.hResultSet_));
            wchar_t resultCell[64]{};ListView_GetItemText(view.hListSqlResults_,0,0,resultCell,64);
            passed &= check(std::wstring(resultCell)==L"22",L"result-set selector displays the selected statement");
            const auto firstDocument=view.documents_[view.activeDocument_].id;
            view.NewDocument();view.LoadSql("SELECT 33 AS n;");view.Execute();
            passed &= check(waitForCompletion() && view.documents_.size()==2,L"independent native query tab executes");
            view.CycleDocument(true);passed &= check(waitForCompletion() && view.documents_[view.activeDocument_].id==firstDocument && CodeEditor::GetText(view.hEditSql_)==L"SELECT 11 AS n; SELECT 22 AS n;",L"query tabs restore independent editor text");
            ListView_GetItemText(view.hListSqlResults_,0,0,resultCell,64);
            passed &= check(std::wstring(resultCell)==L"22",L"query tabs restore independent result selection");
            view.CycleDocument(false);view.CloseDocument(false);
            const std::string originalDatabase = view.activeDbId_;
            const auto secondDatabase = engine.CreateDatabase("Second document database", "blank");
            view.NewDocument();
            view.activeDbId_ = secondDatabase.id; view.documents_[view.activeDocument_].databaseId = secondDatabase.id;
            view.activeTable_.clear(); view.RefreshData();
            passed &= check(waitForCompletion(), L"second query document loads its own database");
            view.LoadSql("SELECT 44 AS n;");
            view.CloseDocument(false);
            passed &= check(waitForCompletion() && view.activeDbId_ == originalDatabase
                && view.documents_[view.activeDocument_].databaseId == originalDatabase
                && CodeEditor::GetText(view.hEditSql_) == L"SELECT 11 AS n; SELECT 22 AS n;",
                L"closing a query tab restores the surviving document database and text");
            SetWindowTextW(view.hRowLimit_,L"1");view.LoadSql("SELECT 1 AS n UNION ALL SELECT 2;");view.Execute();
            passed &= check(waitForCompletion() && view.lastQueryResult_.rows.size()==1 && view.lastQueryResult_.truncated,L"row-limit control bounds displayed query results");
            SetWindowTextW(view.hRowLimit_,L"1000");view.CaptureDocument();
            passed &= check(!view.queryHistory_.empty() && view.queryHistory_.front().sql=="SELECT 1 AS n UNION ALL SELECT 2;",L"executed query history keeps the original document text");
            view.SelectWorkbenchTab(1);
            passed &= check(waitForCompletion() && view.currentTableData_.rows.size() == 1
                && ListView_GetItemCount(view.hListDataGrid_) == 1, L"paginated native grid renders real rows");
            view.LoadSql("WITH RECURSIVE counter(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM counter WHERE n<100000000) SELECT sum(n) FROM counter;");
            view.Execute(); view.RequestCancel();
            passed &= check(waitForCompletion() && !view.lastQueryResult_.error.empty(), L"active SQL cancellation reaches SQLite");
            passed &= check(!view.worker_.joinable(), L"completion joins the worker before destroying the view");
            view.LoadSql("WITH RECURSIVE counter(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM counter WHERE n<500000) SELECT sum(n) AS total FROM counter;");
            view.Execute(); const auto runningDocument = view.documents_[view.activeDocument_].id;
            view.NewDocument(); view.LoadSql("SELECT 77 AS draft;");
            passed &= check(view.IsBusy() && view.documents_.size() == 2 && view.documents_[view.activeDocument_].id != runningDocument
                && IsWindowEnabled(view.hQueryDocTabs_) && IsWindowEnabled(view.hBtnNewDoc_), L"query execution preserves new-document navigation and editing");
            view.CycleDocument(true); passed &= check(view.documents_[view.activeDocument_].id == runningDocument, L"query tabs switch while their owned query runs");
            view.CycleDocument(false);
            passed &= check(waitForCompletion() && CodeEditor::GetText(view.hEditSql_) == L"SELECT 77 AS draft;"
                && view.lastQueryResult_.rows.empty(), L"late query completion preserves another document draft and results");
            const auto completedDocument = std::find_if(view.documents_.begin(), view.documents_.end(), [&](const auto& document) { return document.id == runningDocument; });
            passed &= check(completedDocument != view.documents_.end() && completedDocument->result.error.empty()
                && !completedDocument->result.rows.empty() && completedDocument->result.rows.front().at("total") == "125000250000",
                L"background query result belongs to its launch document");
            view.activeDbId_ = secondDatabase.id; view.documents_[view.activeDocument_].databaseId = secondDatabase.id;
            view.activeTable_.clear(); view.RefreshData();
            passed &= check(waitForCompletion(), L"another query document loads its own database before the navigation regression");
            view.CycleDocument(true); passed &= check(waitForCompletion() && view.activeDbId_ == originalDatabase,
                L"query navigation restores the launch database before execution");
            view.Execute(); view.CycleDocument(false);
            passed &= check(view.IsBusy() && view.job_->kind == Job::Kind::Query && view.reloadPending_
                && view.activeDbId_ == secondDatabase.id,
                L"switching databases through query tabs defers schema work until the owned query finishes");
            passed &= check(waitForCompletion() && view.activeDbId_ == secondDatabase.id && view.schema_.tables.empty()
                && view.lastQueryResult_.rows.empty() && CodeEditor::GetText(view.hEditSql_) == L"SELECT 77 AS draft;",
                L"deferred schema refresh belongs to the selected document database without replacing its draft");
            view.LoadSql("WITH RECURSIVE counter(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM counter WHERE n<100000000) SELECT sum(n) AS abandoned FROM counter;");
            view.Execute(); view.CycleDocument(true); view.CycleDocument(false); view.CloseDocument(false);
            passed &= check(waitForCompletion() && view.documents_.size() == 1 && view.documents_[view.activeDocument_].id == runningDocument
                && view.lastQueryResult_.error.empty() && !view.lastQueryResult_.rows.empty()
                && view.lastQueryResult_.rows.front().at("total") == "125000250000",
                L"closing a running query document cancels it and discards its late result");
            std::string tallSchema = "CREATE TABLE zz_ui_tall(id INTEGER PRIMARY KEY";
            for (int column = 0; column < 32; ++column) tallSchema += ",column_" + std::to_string(column) + " TEXT";
            tallSchema += "); CREATE TABLE er_child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES ui_rows(id));";
            view.LoadSql(tallSchema); view.Execute();
            passed &= check(waitForCompletion() && view.lastQueryResult_.error.empty(), L"large schema loads for ER scroll acceptance");
            // At 150% text and 144 DPI, a 1366x920 window leaves about this
            // physical workspace after the frame and enlarged app header.
            Theme::SetDpi(144); Theme::SetTextScale(150);
            view.ApplyTheme(); view.Resize(0, 0, 1318, 620);
            const auto visibleChildrenFit = [&view]() {
                RECT client{}; GetClientRect(view.hWnd_, &client);
                for (HWND child = GetWindow(view.hWnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                    if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
                    RECT bounds{}; GetWindowRect(child, &bounds);
                    MapWindowPoints(nullptr, view.hWnd_, reinterpret_cast<POINT*>(&bounds), 2);
                    if (bounds.left < 0 || bounds.top < 0 || bounds.right > client.right + 1 || bounds.bottom > client.bottom + 1) return false;
                }
                return true;
            };
            for (int tab = 0; tab < 4; ++tab) {
                view.SelectWorkbenchTab(tab);
                passed &= check(waitForCompletion(), L"enlarged database tab finishes loading");
                passed &= check(visibleChildrenFit(), L"all database tabs stay in bounds at 144 DPI and 150% text");
                if (tab == 3) {
                    SCROLLINFO scroll{}; scroll.cbSize = sizeof(scroll); scroll.fMask = SIF_RANGE | SIF_PAGE;
                    passed &= check(GetScrollInfo(view.hWnd_, SB_VERT, &scroll) && scroll.nMax > static_cast<int>(scroll.nPage),
                        L"large ER schema exposes native scrolling");
                }
            }
            view.SelectWorkbenchTab(3);
            passed &= check(view.erDiagram_.links.size() == 1 && view.erRoutes_.routes.size() == 1 && view.erRoutes_.routes.front().routed,
                L"native ER view routes a real introspected foreign key");
            if (!view.erRoutes_.routes.empty() && view.erRoutes_.routes.front().routed) {
                const auto& route = view.erRoutes_.routes.front();
                passed &= check(route.source.point.y == view.erPositions_[view.activeDbId_]["er_child"].y + 68
                    && route.target.point.y == view.erPositions_[view.activeDbId_]["ui_rows"].y + 44,
                    L"ER connector endpoints align with foreign-key and referenced column rows");
                const auto before = route.points;
                view.UpdateErScrollbars();
                passed &= check(!view.erRoutesDirty_ && view.erRoutes_.routes.front().points == before,
                    L"ER scrollbar updates preserve the logical routing cache");
                const int outward = route.source.side == ErRouteSide::Left ? -1 : 1;
                const int hx = view.erCanvasX_ + Theme::Scale(route.source.point.x + outward * 6) - view.erOffsetX_;
                const int hy = view.erCanvasY_ + Theme::Scale(route.source.point.y) - view.erOffsetY_;
                SendMessageW(view.hWnd_, WM_MOUSEMOVE, 0, MAKELPARAM(hx, hy));
                wchar_t status[256]{}; GetWindowTextW(view.hErStatus_, status, 256);
                passed &= check(view.hoveredErLink_ == 0 && std::wstring(status).find(L"er_child.parent_id -> ui_rows.id") != std::wstring::npos,
                    L"hovering an ER connection reveals its exact relationship without canvas labels");
                SendMessageW(view.hWnd_, WM_MOUSELEAVE, 0, 0);
            }
            HDC canvas=GetDC(view.hWnd_);view.Draw(canvas);ReleaseDC(view.hWnd_,canvas);
            const auto card=view.erCardRects_.find("ui_rows");
            if(card!=view.erCardRects_.end()) {
                const auto original=view.erPositions_[view.activeDbId_]["ui_rows"];
                const int x=card->second.left+Theme::Scale(15),y=card->second.top+Theme::Scale(15);
                SendMessageW(view.hWnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, card->second.top + Theme::Scale(44)));
                passed &= check(view.selectedErTable_ == "ui_rows" && GetCapture() != view.hWnd_, L"ER table body selection highlights links without starting a drag");
                SendMessageW(view.hWnd_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
                SendMessageW(view.hWnd_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+Theme::Scale(50),y+Theme::Scale(20)));
                SendMessageW(view.hWnd_,WM_LBUTTONUP,0,MAKELPARAM(x+Theme::Scale(50),y+Theme::Scale(20)));
                const auto moved=view.erPositions_[view.activeDbId_]["ui_rows"];
                passed &= check(moved.x==original.x+50 && moved.y==original.y+20,L"ER card dragging stores DPI-independent positions");
                passed &= check(!view.erRoutesDirty_ && !view.erRoutes_.routes.empty() && view.erRoutes_.routes.front().routed
                    && view.erRoutes_.routes.front().target.point.y == moved.y + 44, L"dragging an ER table rebuilds its column-level connector");
                view.RefreshErDiagram();
                passed &= check(view.erPositions_[view.activeDbId_]["ui_rows"].x == moved.x && view.erPositions_[view.activeDbId_]["ui_rows"].y == moved.y,
                    L"schema refresh preserves manually positioned ER cards");
            } else passed &= check(false,L"ER card hit-test geometry");
            view.SaveWorkspace();
            std::ifstream protectedFile(std::filesystem::u8path(engine.WorkspaceDirectory())/"query-workspace.bin",std::ios::binary);
            std::string protectedBytes((std::istreambuf_iterator<char>(protectedFile)),std::istreambuf_iterator<char>());
            passed &= check(protectedBytes.find("SELECT")==std::string::npos && !protectedBytes.empty(),L"query workspace persists encrypted text and history");
            { DbStudioView restored(&engine);restored.LoadWorkspace();
            passed &= check(restored.documents_.size()==view.documents_.size() && restored.documents_[restored.activeDocument_].sql==view.documents_[view.activeDocument_].sql && restored.queryHistory_.size()==view.queryHistory_.size(),L"protected query documents and history survive reload");
            passed &= check(restored.erPositions_[view.activeDbId_]["ui_rows"].x==view.erPositions_[view.activeDbId_]["ui_rows"].x,L"ER layout survives workspace reload"); }
            SendMessageW(view.hWnd_, WM_COMMAND, 3405, 0);
            bool cardsClear = true;
            for (size_t a = 0; a < view.erDiagram_.nodes.size(); ++a) {
                const auto& nodeA = view.erDiagram_.nodes[a]; const auto& pointA = view.erPositions_[view.activeDbId_][nodeA.name];
                const RECT boundsA{pointA.x, pointA.y, pointA.x + 235, pointA.y + 38 + static_cast<int>(nodeA.columns.size()) * 24};
                for (size_t b = a + 1; b < view.erDiagram_.nodes.size(); ++b) {
                    const auto& nodeB = view.erDiagram_.nodes[b]; const auto& pointB = view.erPositions_[view.activeDbId_][nodeB.name];
                    const RECT boundsB{pointB.x, pointB.y, pointB.x + 235, pointB.y + 38 + static_cast<int>(nodeB.columns.size()) * 24}; RECT intersection{};
                    if (IntersectRect(&intersection, &boundsA, &boundsB)) cardsClear = false;
                }
            }
            passed &= check(cardsClear && view.erOffsetX_ == 0 && view.erOffsetY_ == 0 && !view.erRoutes_.routes.empty() && view.erRoutes_.routes.front().routed,
                L"Auto arrange separates variable-height cards and reroutes their relationships");
            RECT erWindow{}, erClient{}; GetWindowRect(view.hWnd_, &erWindow); GetClientRect(view.hWnd_, &erClient);
            const int erFrameHeight = erWindow.bottom - erWindow.top - erClient.bottom;
            view.Resize(0, 0, 1318, view.erExtentH_ + view.erCanvasY_ + Theme::Scale(6) + erFrameHeight);
            SendMessageW(view.hWnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
            passed &= check(view.erOffsetX_ > 0 && view.erOffsetY_ == 0, L"ER wheel uses horizontal scrolling when the actual vertical canvas fits exactly");
            view.Resize(0, 0, 1318, 620); view.erOffsetX_ = view.erOffsetY_ = 0; view.UpdateErScrollbars();
            view.SelectWorkbenchTab(2);
            passed &= check(ListView_GetItemCount(view.hListSchemaCols_) == 2
                && CodeEditor::GetText(view.hEditSchemaDdl_).find(L"ui_rows") != std::wstring::npos,
                L"native schema tab renders introspected columns and DDL");
            const auto schemaTemplates = QueryTemplates(view.schema_, 1000, "");
            const auto createTemplate = std::find_if(schemaTemplates.begin(), schemaTemplates.end(), [](const auto& item) { return item.name == "CREATE TABLE"; });
            passed &= check(createTemplate != schemaTemplates.end(), L"SQL query tools expose the original create-table template");
            if (createTemplate != schemaTemplates.end()) {
                view.LoadSql(createTemplate->text); view.Execute();
                passed &= check(waitForCompletion() && view.lastQueryResult_.error.empty()
                    && std::any_of(view.schema_.tables.begin(), view.schema_.tables.end(), [](const auto& table) { return table.name == "new_table"; }),
                    L"generated SQLite create-table template executes through the native worker");
            }
            const auto parentTable = std::find_if(view.schema_.tables.begin(), view.schema_.tables.end(), [](const auto& table) { return table.name == "ui_rows"; });
            if (parentTable != view.schema_.tables.end()) {
                DatabaseSchema linked = view.schema_; TableMeta child; child.name = "child_rows";
                ForeignKeyMeta foreign; foreign.fromColumn = "parent_id"; foreign.targetTable = "ui_rows"; foreign.toColumn = "id"; child.foreignKeys.push_back(foreign); linked.tables.push_back(child);
                const auto templates = QueryTemplates(linked, 1000, "main");
                const auto join = std::find_if(templates.begin(), templates.end(), [](const auto& item) { return item.name == "JOIN"; });
                passed &= check(join != templates.end() && join->text.find("JOIN \"main\".\"ui_rows\"") != std::string::npos
                    && join->text.find("a.\"parent_id\" = b.\"id\"") != std::string::npos,
                    L"SQL join template follows a real foreign-key target with schema qualification");
            }
            engine.DisconnectDatabase(view.activeDbId_); view.RefreshData();
            view.LoadSql("SELECT 1;"); view.Execute();
            passed &= check(!view.IsBusy() && !IsWindowEnabled(view.hBtnRunSql_), L"keyboard execution cannot bypass a disconnected connection");
            engine.SaveConnection(engine.GetConnectionProfile(view.activeDbId_), true); view.RefreshData();
            passed &= check(waitForCompletion(), L"explicit reconnect restores the database workbench");
            view.LoadSql("SELECT 1;"); view.Execute();
            SetPropW(parent, L"DataForge.Closing", reinterpret_cast<HANDLE>(1));
            passed &= check(waitForCompletion() && !view.IsBusy() && !view.reloadPending_,
                L"closing drains a completed query without scheduling schema work");
            view.RefreshData(); view.Execute();
            passed &= check(!view.IsBusy(), L"closing rejects new database work");
            RemovePropW(parent, L"DataForge.Closing");
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
