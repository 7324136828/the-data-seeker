#include "TempWorkspace.h"
#include <windows.h>
#include <objbase.h>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <cctype>

#pragma comment(lib, "ole32.lib")
namespace native_app {
namespace fs = std::filesystem;
namespace {
bool ReparsePoint(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
void ValidateComponent(const std::string& value) {
    if (value == "." || value == ".." || value.find_first_of(":*?\"<>|") != std::string::npos || (!value.empty() && (value.back() == ' ' || value.back() == '.')))
        throw std::runtime_error("Unsafe temporary workspace path.");
    std::string base = value.substr(0, value.find('.'));
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || (base.size() == 4 && (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") && base[3] >= '1' && base[3] <= '9'))
        throw std::runtime_error("Reserved temporary workspace filename.");
}
}
TempWorkspace::TempWorkspace(const std::string& prefix) {
    if (prefix.size() > 64 || std::any_of(prefix.begin(), prefix.end(), [](unsigned char c) { return !isalnum(c) && c != '_' && c != '-'; }))
        throw std::runtime_error("Temporary workspace prefix must contain only letters, numbers, underscores, and hyphens.");
    wchar_t temporary[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (!length || length >= std::size(temporary)) throw std::runtime_error("Windows temporary directory is unavailable.");
    const fs::path parent = fs::path(temporary) / L"DataForgeStudio" / L"jobs";
    fs::create_directories(parent);
    if (ReparsePoint(parent) || ReparsePoint(parent.parent_path())) throw std::runtime_error("Temporary workspace parent cannot be a link.");
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot generate a temporary workspace identifier.");
    wchar_t identifier[40]{}; StringFromGUID2(guid, identifier, 40);
    const std::wstring wideId(identifier);
    for (wchar_t c : wideId) ownerId_ += static_cast<char>(c); // GUID characters are ASCII.
    path_ = parent / fs::u8path(prefix + ownerId_);
    if (!fs::create_directory(path_)) throw std::runtime_error("Temporary workspace collision.");
    std::ofstream marker(path_ / L".workspace-owner", std::ios::binary);
    marker << ownerId_;
    marker.close();
    if (!marker) { std::error_code error; fs::remove(path_, error); throw std::runtime_error("Cannot initialize temporary workspace."); }
}
TempWorkspace::~TempWorkspace() {
    if (autoCleanup_) { try { Cleanup(); } catch (...) {} }
}
TempWorkspace::TempWorkspace(TempWorkspace&& other) noexcept
    : path_(std::move(other.path_)), ownerId_(std::move(other.ownerId_)), autoCleanup_(other.autoCleanup_) { other.autoCleanup_ = false; }
TempWorkspace& TempWorkspace::operator=(TempWorkspace&& other) noexcept {
    if (this != &other) {
        if (autoCleanup_) { try { Cleanup(); } catch (...) {} }
        path_ = std::move(other.path_); ownerId_ = std::move(other.ownerId_); autoCleanup_ = other.autoCleanup_; other.autoCleanup_ = false;
    }
    return *this;
}
fs::path TempWorkspace::GetSubpath(const std::string& relative) const {
    const fs::path child = fs::u8path(relative);
    if (child.empty() || child.is_absolute() || child.has_root_name() || child.has_root_directory()) throw std::runtime_error("Expected a relative workspace filename.");
    if (ReparsePoint(path_)) throw std::runtime_error("Temporary workspace was replaced with a link.");
    fs::path current = path_;
    for (const auto& component : child) {
        ValidateComponent(component.u8string());
        current /= component;
        if (ReparsePoint(current)) throw std::runtime_error("Temporary workspace paths cannot traverse links.");
    }
    return current;
}
void TempWorkspace::Cleanup() {
    if (path_.empty() || !fs::exists(path_)) return;
    if (ownerId_.empty() || ReparsePoint(path_) || ReparsePoint(path_.parent_path()) || ReparsePoint(path_ / L".workspace-owner"))
        throw std::runtime_error("Temporary cleanup refused: workspace ownership is invalid.");
    std::ifstream marker(path_ / L".workspace-owner", std::ios::binary);
    const std::string recorded((std::istreambuf_iterator<char>(marker)), {});
    marker.close();
    if (recorded != ownerId_) throw std::runtime_error("Temporary cleanup refused: workspace ownership changed.");
    for (const auto& entry : fs::recursive_directory_iterator(path_))
        if (ReparsePoint(entry.path())) throw std::runtime_error("Temporary cleanup refused: workspace contains a link.");
    std::error_code error;
    fs::remove_all(path_, error);
    if (error) throw std::runtime_error("Temporary workspace could not be cleaned up.");
    path_.clear(); ownerId_.clear();
}
} // namespace native_app
