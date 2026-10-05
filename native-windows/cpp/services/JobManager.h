#pragma once

#include "Types.h"
#include <functional>
#include <future>
#include <atomic>
#include <memory>

namespace native_app {

class CancellationToken {
public:
    CancellationToken() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}
    void Cancel() { cancelled_->store(true); }
    bool IsCancelled() const { return cancelled_->load(); }
    void Reset() { cancelled_->store(false); }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

class JobManager {
public:
    JobManager() = default;
    ~JobManager() = default;

    template <typename Func>
    auto RunAsync(Func&& f) -> std::future<decltype(f())> {
        return std::async(std::launch::async, std::forward<Func>(f));
    }
};

} // namespace native_app
