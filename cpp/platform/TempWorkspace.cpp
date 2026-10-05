#include "TempWorkspace.h"
#include <windows.h>
#include <system_error>

namespace native_app {

namespace fs = std::filesystem;

TempWorkspace::TempWorkspace(const std::string& prefix) {
    wchar_t tempPath[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH, tempPath);

    fs::path baseTemp(tempPath);
    fs::path vendorDir = baseTemp / L"DataForgeStudio" / L"jobs";

    std::string uniqueId = prefix + std::to_string(GetTickCount64()) + "_" + std::to_string(GetCurrentProcessId());
    path_ = vendorDir / uniqueId;

    fs::create_directories(path_);
}

TempWorkspace::~TempWorkspace() {
    if (autoCleanup_) {
        Cleanup();
    }
}

TempWorkspace::TempWorkspace(TempWorkspace&& other) noexcept
    : path_(std::move(other.path_)), autoCleanup_(other.autoCleanup_) {
    other.autoCleanup_ = false;
}

TempWorkspace& TempWorkspace::operator=(TempWorkspace&& other) noexcept {
    if (this != &other) {
        if (autoCleanup_) Cleanup();
        path_ = std::move(other.path_);
        autoCleanup_ = other.autoCleanup_;
        other.autoCleanup_ = false;
    }
    return *this;
}

std::filesystem::path TempWorkspace::GetSubpath(const std::string& relative) const {
    fs::path p = path_ / relative;
    // Normalize and ensure it remains inside workspace
    p = fs::weakly_canonical(p);
    fs::path root = fs::weakly_canonical(path_);
    auto mismatch = std::mismatch(root.begin(), root.end(), p.begin());
    if (mismatch.first != root.end()) {
        throw std::runtime_error("Path traversal detected outside temporary workspace.");
    }
    return p;
}

void TempWorkspace::Cleanup() {
    if (!path_.empty() && fs::exists(path_)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
}

} // namespace native_app
