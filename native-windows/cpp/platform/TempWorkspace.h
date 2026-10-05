#pragma once

#include <string>
#include <filesystem>

namespace native_app {

class TempWorkspace {
public:
    explicit TempWorkspace(const std::string& prefix = "job_");
    ~TempWorkspace();

    // Disable copy
    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    // Enable move
    TempWorkspace(TempWorkspace&& other) noexcept;
    TempWorkspace& operator=(TempWorkspace&& other) noexcept;

    const std::filesystem::path& GetPath() const { return path_; }
    std::filesystem::path GetSubpath(const std::string& relative) const;
    void Cleanup();

private:
    std::filesystem::path path_;
    std::string ownerId_;
    bool autoCleanup_ = true;
};

} // namespace native_app
