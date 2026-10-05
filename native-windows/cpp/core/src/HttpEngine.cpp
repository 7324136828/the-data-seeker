#include "HttpEngine.h"
#include "VariableResolver.h"
#include "Assertions.h"
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <objbase.h>
#include <chrono>
#include <ctime>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <algorithm>
#include <stdexcept>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace native_app {
namespace {
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Request contains invalid UTF-8 text.");
    std::wstring result(length, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};
// A per-request watcher interrupts blocking WinHTTP calls on cancellation.
// Only one owner closes the request, and the watcher joins before parents die.
class RequestHandle {
public:
    explicit RequestHandle(HINTERNET handle, const std::atomic_bool* cancellation) : value_(handle) {
        if (handle && cancellation) watcher_ = std::thread([this, cancellation] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!finished_) {
                if (cancellation->load()) {
                    if (value_) { WinHttpCloseHandle(value_); value_ = nullptr; }
                    break;
                }
                changed_.wait_for(lock, std::chrono::milliseconds(20));
            }
        });
    }
    ~RequestHandle() {
        { std::lock_guard<std::mutex> lock(mutex_); finished_ = true; }
        changed_.notify_all();
        if (watcher_.joinable()) watcher_.join();
        if (value_) WinHttpCloseHandle(value_);
    }
private:
    HINTERNET value_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool finished_ = false;
    std::thread watcher_;
};
std::string DecodeBase64(const std::string& value) {
    DWORD length = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr))
        throw std::runtime_error("Multipart file contains invalid Base64 data.");
    std::string output(length, 0);
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(output.data()), &length, nullptr, nullptr))
        throw std::runtime_error("Multipart file contains invalid Base64 data.");
    return output;
}
std::string MultipartName(const std::string& value) {
    if (value.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Multipart names cannot contain line breaks.");
    std::string result;
    for (char c : value) { if (c == '\\' || c == '"') result += '\\'; result += c; }
    return result;
}
}

ApiResponse HttpEngine::Execute(const ApiRequest& input, const std::map<std::string, std::string>& variables,
    bool strictVariables, const std::atomic_bool* cancelled) {
    ApiResponse response;
    const auto start = std::chrono::steady_clock::now();
    const auto now = std::time(nullptr);
    std::tm utc{}; gmtime_s(&utc, &now);
    char timestamp[64]{}; std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    response.executedAt = timestamp;
    response.method = HttpMethodToString(input.method);
    auto checkCancellation = [&] { if (cancelled && cancelled->load()) throw std::runtime_error("Request cancelled."); };
    auto check = [&](BOOL success, const char* stage) {
        if (!success) {
            checkCancellation();
            throw std::runtime_error(std::string(stage) + " (Windows error " + std::to_string(GetLastError()) + ").");
        }
    };
    try {
        checkCancellation();
        const auto req = VariableResolver::PrepareRequest(input, variables, strictVariables);
        response.url = req.url;
        if (!std::isfinite(req.options.timeoutSec) || req.options.timeoutSec <= 0 || req.options.timeoutSec > 3600)
            throw std::runtime_error("Timeout must be greater than zero and at most 3600 seconds.");
        const std::wstring url = Wide(req.url);
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        check(WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts), "Invalid HTTP URL");
        if ((parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) || parts.dwHostNameLength == 0)
            throw std::runtime_error("Use an HTTP or HTTPS URL with a host name.");
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
        if (parts.dwExtraInfoLength) path += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const auto fragment = path.find(L'#'); if (fragment != std::wstring::npos) path.resize(fragment);
        InternetHandle session(WinHttpOpen(L"DataForgeStudio/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.value) throw std::runtime_error("Cannot initialize Windows HTTP networking.");
        const int timeout = static_cast<int>(req.options.timeoutSec * 1000);
        check(WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout), "Cannot set HTTP timeout");
        InternetHandle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
        if (!connection.value) throw std::runtime_error("Cannot create HTTP connection.");
        const auto method = Wide(response.method);
        HINTERNET request = WinHttpOpenRequest(connection.value, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request) throw std::runtime_error("Cannot create HTTP request.");
        RequestHandle ownedRequest(request, cancelled);
        auto addHeader = [&](const std::string& key, const std::string& value) {
            if (key.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
                throw std::runtime_error("HTTP header names and values cannot contain line breaks.");
            const auto header = Wide(key + ": " + value);
            checkCancellation();
            check(WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(header.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE), "Invalid HTTP header");
        };
        if (!req.options.followRedirects) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            check(WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)), "Cannot set redirect policy");
        }
        if (!req.options.verifyTls && parts.nScheme == INTERNET_SCHEME_HTTPS) {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            check(WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags)), "Cannot set TLS policy");
        }
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
        for (const auto& header : req.headers) if (header.enabled && !header.key.empty()) addHeader(header.key, header.value);
        std::string payload;
        if (req.body.type == BodyType::Raw || req.body.type == BodyType::Json) payload = req.body.content;
        else if (req.body.type == BodyType::UrlEncoded) {
            for (const auto& item : req.body.formItems) {
                if (!item.enabled || item.key.empty()) continue;
                if (!payload.empty()) payload += '&';
                payload += VariableResolver::UrlEncode(item.key) + "=" + VariableResolver::UrlEncode(item.value);
            }
            addHeader("Content-Type", "application/x-www-form-urlencoded");
        } else if (req.body.type == BodyType::FormData) {
            GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot create multipart boundary.");
            const std::string boundary = "DataForge" + std::to_string(guid.Data1) + std::to_string(guid.Data2) + std::to_string(GetTickCount64());
            for (const auto& item : req.body.formItems) {
                if (!item.enabled || item.key.empty()) continue;
                payload += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + MultipartName(item.key) + "\"";
                if (item.type == "file") {
                    payload += "; filename=\"" + MultipartName(item.filename.empty() ? "upload" : item.filename) + "\"\r\n";
                    if (item.contentType.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid multipart content type.");
                    payload += "Content-Type: " + item.contentType + "\r\n\r\n";
                    payload += item.contentBase64.empty() ? item.value : DecodeBase64(item.contentBase64);
                } else payload += "\r\n\r\n" + item.value;
                payload += "\r\n";
            }
            payload += "--" + boundary + "--\r\n";
            addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        }
        constexpr size_t maximumBytes = 64 * 1024 * 1024;
        if (payload.size() > maximumBytes) throw std::runtime_error("Request body exceeds the 64 MiB limit.");
        checkCancellation();
        check(WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, payload.empty() ? WINHTTP_NO_REQUEST_DATA : payload.data(),
            static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0), "HTTP send failed");
        checkCancellation();
        check(WinHttpReceiveResponse(request, nullptr), "HTTP receive failed");
        DWORD status = 0, length = sizeof(status);
        check(WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &length, nullptr), "Cannot read HTTP status");
        response.statusCode = static_cast<int>(status);
        wchar_t statusText[256]{}; length = sizeof(statusText);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_TEXT, nullptr, statusText, &length, nullptr)) response.statusText = Utf8(statusText);
        length = 0;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, WINHTTP_NO_OUTPUT_BUFFER, &length, nullptr);
        if (length > 0 && length <= 1024 * 1024) {
            std::wstring headers((length + sizeof(wchar_t) - 1) / sizeof(wchar_t), 0);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, headers.data(), &length, nullptr)) {
                std::wistringstream lines(headers);
                std::wstring line; std::getline(lines, line);
                while (std::getline(lines, line)) {
                    const auto colon = line.find(':'); if (colon == std::wstring::npos) continue;
                    const auto key = Utf8(line.substr(0, colon));
                    auto value = Utf8(line.substr(colon + 1));
                    const auto first = value.find_first_not_of(" \t"); value = first == std::string::npos ? "" : value.substr(first);
                    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\0')) value.pop_back();
                    response.headers.push_back({key, value, true, ""});
                    if (_stricmp(key.c_str(), "Content-Type") == 0) {
                        response.contentType = value;
                        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
                        response.isJson = value.find("json") != std::string::npos;
                    }
                }
            }
        }
        char buffer[65536]; DWORD received = 0;
        for (;;) {
            checkCancellation();
            check(WinHttpReadData(request, buffer, sizeof(buffer), &received), "HTTP response read failed");
            if (!received) break;
            if (response.body.size() + received > maximumBytes) throw std::runtime_error("Response exceeds the 64 MiB limit.");
            response.body.append(buffer, received);
        }
        response.sizeBytes = response.body.size();
        response.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        response.testResults = AssertionsEvaluator::Evaluate(req.tests, response.statusCode, response.latencyMs, response.headers, response.body);
    } catch (const std::exception& error) {
        response.statusCode = 0; response.statusText = "Error"; response.error = error.what(); response.body = response.error;
        response.sizeBytes = 0;
        response.latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    return response;
}
} // namespace native_app
