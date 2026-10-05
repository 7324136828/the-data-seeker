#pragma once
#include <memory>
namespace native_app {
class DbEngine;
class NativeMockServer {
public:
    explicit NativeMockServer(DbEngine* engine);
    ~NativeMockServer();
    NativeMockServer(const NativeMockServer&) = delete;
    unsigned short Start(unsigned short port = 8001);
    void Stop() noexcept;
    bool IsRunning() const noexcept;
    unsigned short Port() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
