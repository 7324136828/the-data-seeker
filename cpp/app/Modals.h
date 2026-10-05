#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <functional>
#include "Types.h"
#include "StoreManager.h"

namespace native_app {

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

} // namespace native_app
