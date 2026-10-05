#include "ProcessRunner.h"
#include <windows.h>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <chrono>
#include <thread>

namespace native_app {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void Close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Native process arguments contain invalid UTF-8.");
    std::wstring result(length, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}
// Windows argv escaping: double every backslash before a closing quote, and
// double backslashes plus one before an embedded quote.
std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++backslashes; continue; }
        result.append(backslashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result += L'\\';
        result += c; backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    return result + L"\"";
}
}
ProcessResult ProcessRunner::Run(const std::string& applicationPath, const std::vector<std::string>& arguments,
    const std::string& workingDir, uint32_t timeoutMs) {
    ProcessResult result;
    try {
        namespace fs = std::filesystem;
        const auto application = fs::u8path(applicationPath);
        if (!application.is_absolute() || !fs::is_regular_file(application)) throw std::runtime_error("Native helper executable is missing or its path is not absolute.");
        if (!timeoutMs) throw std::runtime_error("Native helper timeout must be greater than zero.");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        Handle outputRead, outputWrite, input, job;
        if (!CreatePipe(&outputRead.value, &outputWrite.value, &attributes, 0) || !SetHandleInformation(outputRead.value, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("Cannot initialize native helper output.");
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot initialize native helper input.");
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> storage(attributeBytes);
        auto* attributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) throw std::runtime_error("Cannot initialize native helper handle list.");
        struct AttributeOwner { PPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeOwner() { DeleteProcThreadAttributeList(value); } } attributeOwner{attributeList};
        HANDLE inherited[] = {outputWrite.value, input.value};
        if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
            throw std::runtime_error("Cannot restrict native helper handle inheritance.");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributeList;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdOutput = outputWrite.value;
        startup.StartupInfo.hStdError = outputWrite.value; startup.StartupInfo.hStdInput = input.value;
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Cannot contain native helper process lifetime.");
        std::wstring command = Quote(application.wstring());
        for (const auto& arg : arguments) command += L" " + Quote(Wide(arg));
        const auto directory = Wide(workingDir);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            directory.empty() ? application.parent_path().c_str() : directory.c_str(), &startup.StartupInfo, &process))
            throw std::runtime_error("Cannot start native helper (Windows error " + std::to_string(GetLastError()) + ").");
        Handle processHandle, threadHandle; processHandle.value = process.hProcess; threadHandle.value = process.hThread;
        if (!AssignProcessToJobObject(job.value, process.hProcess)) {
            TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000);
            throw std::runtime_error("Cannot assign native helper to its lifetime container.");
        }
        outputWrite.Close(); input.Close();
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) throw std::runtime_error("Cannot resume native helper.");
        threadHandle.Close();
        const auto start = std::chrono::steady_clock::now();
        bool truncated = false;
        for (;;) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >= timeoutMs) {
                TerminateJobObject(job.value, 1); WaitForSingleObject(process.hProcess, 5000);
                result.stdErr = "Native helper timed out and was stopped.";
                return result;
            }
            DWORD available = 0;
            if (PeekNamedPipe(outputRead.value, nullptr, 0, nullptr, &available, nullptr) && available) {
                char buffer[4096]; DWORD received = 0;
                if (ReadFile(outputRead.value, buffer, static_cast<DWORD>(sizeof(buffer)), &received, nullptr) && received) {
                    if (result.stdOut.size() + received <= 4 * 1024 * 1024) result.stdOut.append(buffer, received);
                    else truncated = true;
                    continue;
                }
            }
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >= timeoutMs) {
                TerminateJobObject(job.value, 1); WaitForSingleObject(process.hProcess, 5000);
                result.stdErr = "Native helper timed out and was stopped.";
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process.hProcess, &exitCode)) throw std::runtime_error("Cannot read native helper exit status.");
        result.exitCode = static_cast<int>(exitCode); result.success = exitCode == 0;
        if (truncated) result.stdErr = "Native helper output exceeded 4 MiB and was truncated.";
    } catch (const std::exception& error) { result.stdErr = error.what(); }
    return result;
}
} // namespace native_app
