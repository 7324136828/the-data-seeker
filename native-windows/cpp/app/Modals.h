#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <functional>
#include "Types.h"
#include "StoreManager.h"

namespace native_app {

class DbEngine;
class NativeMockServer;
bool RunModalSelfTests(std::wstring& failure);
bool RunConnectionModalSelfTests(DbEngine* engine, std::wstring& failure);
void ShowConnectionDialog(HWND parent, DbEngine* engine, const ConnectionProfile& profile,
    const std::function<void(const DatabaseInfo&, bool)>& onSaved);
void ShowMockServerDialog(HWND parent, NativeMockServer* server, const std::string& databaseId, const std::string& table);
bool ShowTypedRowEditorDialog(HWND parent, const TableDataResult& table, const TypedRow& original,
    bool insert, TypedRow& changes, const std::vector<std::string>& readOnlyColumns = {});

// Environment editor modal dialog
void ShowEnvironmentsDialog(HWND hParent, StoreManager* store, std::function<void()> onUpdate);

// Globals editor modal dialog
void ShowGlobalsDialog(HWND hParent, StoreManager* store, std::function<void()> onUpdate);

// cURL import modal dialog
void ShowCurlImportDialog(HWND hParent, std::function<void(const ApiRequest&)> onImport);

// OpenAPI import modal dialog
void ShowOpenApiImportDialog(HWND hParent, StoreManager* store, std::function<void()> onImport);

// Code generator modal dialog (cURL, Python, Fetch, Axios)
void ShowCodeGenDialog(HWND hParent, const ApiRequest& req);

// New database creation wizard (Blank, E-Commerce, Dev Studio presets)
void ShowNewDatabaseDialog(HWND hParent, std::function<void(const std::wstring& dbPath, int samplePreset)> onCreate);

// Documentation dialog
void ShowDocsDialog(HWND hParent);

// Insert omits untouched fields so SQLite defaults remain effective; updates
// return changed fields only to preserve unchanged NULL/default values.
bool ShowRowEditorDialog(HWND parent, const TableDataResult& table,
    const std::map<std::string, std::string>& original, bool insert,
    std::map<std::string, std::string>& changes);
} // namespace native_app
