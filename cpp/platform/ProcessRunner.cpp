#include "ProcessRunner.h"
#include <windows.h>
#include <vector>

namespace native_app {

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &wstr[0], sizeNeeded);
    return wstr;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

ProcessResult ProcessRunner::Run(const std::string& appPath, const std::vector<std::string>& arguments, const std::string& workingDir) {
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdOutRead = nullptr;
    HANDLE hStdOutWrite = nullptr;
    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) return result;
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdOutWrite;

    PROCESS_INFORMATION pi{};

    std::wstring cmdLine = L"\"" + Utf8ToWide(appPath) + L"\"";
    for (const auto& arg : arguments) {
        cmdLine += L" \"" + Utf8ToWide(arg) + L"\"";
    }

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(0);

    std::wstring wWorkDir = Utf8ToWide(workingDir);
    LPCWSTR lpWorkDir = workingDir.empty() ? nullptr : wWorkDir.c_str();

    BOOL created = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        lpWorkDir,
        &si,
        &pi
    );

    CloseHandle(hStdOutWrite);

    if (!created) {
        CloseHandle(hStdOutRead);
        return result;
    }

    // Drain pipe
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = 0;
        result.stdOut.append(buffer, bytesRead);
    }
    CloseHandle(hStdOutRead);

    WaitForSingleObject(pi.hProcess, 30000); // 30 sec timeout
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    result.success = (result.exitCode == 0);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

} // namespace native_app
