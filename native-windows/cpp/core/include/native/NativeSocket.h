#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
namespace native_app {
// A synchronous native transport whose DNS, connect, reads and writes poll cancellation.
// Each adapter owns its socket; no caller closes handles in another thread.
class NativeSocket {
public:
    NativeSocket();
    ~NativeSocket();
    NativeSocket(const NativeSocket&) = delete;
    void Connect(const std::string& host, unsigned short port, bool tls,
        const std::string& certificateFile, int timeoutSeconds,
        const std::atomic_bool* cancellation);
    void Adopt(std::uintptr_t socket, int timeoutSeconds, const std::atomic_bool* cancellation);
    std::size_t Read(void* destination, std::size_t capacity);
    void Write(const void* source, std::size_t size);
    void Close() noexcept;
    bool Alive() const noexcept;
    void SetCancellation(const std::atomic_bool* cancellation) noexcept;
    void SetTimeout(int timeoutSeconds) noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
void InitializeNativeSockets();
}
