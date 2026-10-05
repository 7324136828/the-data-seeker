#include "HttpEngine.h"
#include "VariableResolver.h"
#include "Assertions.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <sstream>
#include <vector>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

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

ApiResponse HttpEngine::Execute(
    const ApiRequest& inputRequest,
    const std::map<std::string, std::string>& variables,
    bool strictVariables
) {
    ApiResponse response;
    auto startTime = std::chrono::high_resolution_clock::now();

    ApiRequest req;
    try {
        req = VariableResolver::PrepareRequest(inputRequest, variables, strictVariables);
    } catch (const std::exception& ex) {
        response.statusCode = 0;
        response.statusText = "Error";
        response.error = ex.what();
        response.body = ex.what();
        return response;
    }

    response.url = req.url;
    response.method = HttpMethodToString(req.method);

    // Get current time ISO-8601
    std::time_t now = std::time(nullptr);
    char timeBuf[64];
    std::tm gmTm{};
    gmtime_s(&gmTm, &now);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", &gmTm);
    response.executedAt = timeBuf;

    // Crack URL
    std::wstring wUrl = Utf8ToWide(req.url);
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = static_cast<DWORD>(-1);
    urlComp.dwHostNameLength = static_cast<DWORD>(-1);
    urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
    urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.length()), 0, &urlComp)) {
        response.statusCode = 0;
        response.statusText = "Error";
        response.error = "Invalid URL format.";
        response.body = response.error;
        return response;
    }

    std::wstring hostName(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring urlPath;
    if (urlComp.dwUrlPathLength > 0) {
        urlPath = std::wstring(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    } else {
        urlPath = L"/";
    }
    if (urlComp.dwExtraInfoLength > 0) {
        urlPath += std::wstring(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
    }

    bool isHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = urlComp.nPort;

    HINTERNET hSession = WinHttpOpen(
        L"DataForgeStudio/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession) {
        response.statusCode = 0;
        response.statusText = "Error";
        response.error = "Failed to initialize WinHTTP session.";
        response.body = response.error;
        return response;
    }

    // Set timeouts
    int timeoutMs = static_cast<int>(req.options.timeoutSec * 1000);
    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, hostName.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        response.statusCode = 0;
        response.statusText = "Error";
        response.error = "Failed to connect to host: " + WideToUtf8(hostName);
        response.body = response.error;
        return response;
    }

    DWORD dwFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    std::wstring wMethod = Utf8ToWide(response.method);

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        wMethod.c_str(),
        urlPath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        dwFlags
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.statusCode = 0;
        response.statusText = "Error";
        response.error = "Failed to create HTTP request.";
        response.body = response.error;
        return response;
    }

    // Redirect options
    if (!req.options.followRedirects) {
        DWORD redirectOption = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectOption, sizeof(redirectOption));
    }

    // TLS verification options
    if (!req.options.verifyTls && isHttps) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    // Add headers
    for (const auto& h : req.headers) {
        if (h.enabled && !h.key.empty()) {
            std::string headerLine = h.key + ": " + h.value;
            std::wstring wHeader = Utf8ToWide(headerLine);
            WinHttpAddRequestHeaders(hRequest, wHeader.c_str(), static_cast<DWORD>(wHeader.length()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
    }

    // Prepare body payload
    std::string bodyPayload;
    if (req.body.type == BodyType::Json || req.body.type == BodyType::Raw) {
        bodyPayload = req.body.content;
    } else if (req.body.type == BodyType::UrlEncoded) {
        std::string formStr;
        for (const auto& item : req.body.formItems) {
            if (item.enabled && !item.key.empty()) {
                if (!formStr.empty()) formStr += "&";
                formStr += VariableResolver::UrlEncode(item.key) + "=" + VariableResolver::UrlEncode(item.value);
            }
        }
        bodyPayload = formStr;
        std::wstring ctHdr = L"Content-Type: application/x-www-form-urlencoded";
        WinHttpAddRequestHeaders(hRequest, ctHdr.c_str(), static_cast<DWORD>(ctHdr.length()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    } else if (req.body.type == BodyType::FormData) {
        std::string boundary = "----DataForgeBoundary" + std::to_string(GetTickCount64());
        std::ostringstream ss;
        for (const auto& item : req.body.formItems) {
            if (!item.enabled || item.key.empty()) continue;
            ss << "--" << boundary << "\r\n";
            if (item.type == "file") {
                ss << "Content-Disposition: form-data; name=\"" << item.key << "\"; filename=\""
                   << (item.filename.empty() ? "upload" : item.filename) << "\"\r\n";
                ss << "Content-Type: " << item.contentType << "\r\n\r\n";
                // If base64 decoded content is provided, decode or use value
                ss << item.value << "\r\n";
            } else {
                ss << "Content-Disposition: form-data; name=\"" << item.key << "\"\r\n\r\n";
                ss << item.value << "\r\n";
            }
        }
        ss << "--" << boundary << "--\r\n";
        bodyPayload = ss.str();
        std::string ctHdr = "Content-Type: multipart/form-data; boundary=" + boundary;
        std::wstring wCt = Utf8ToWide(ctHdr);
        WinHttpAddRequestHeaders(hRequest, wCt.c_str(), static_cast<DWORD>(wCt.length()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    LPVOID pData = bodyPayload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(bodyPayload.data());
    DWORD dwDataLen = static_cast<DWORD>(bodyPayload.size());

    BOOL bResults = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        pData,
        dwDataLen,
        dwDataLen,
        0
    );

    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, nullptr);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    response.latencyMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    if (!bResults) {
        DWORD dwErr = GetLastError();
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "WinHTTP request failed with error code: %lu", dwErr);
        response.statusCode = 0;
        response.statusText = "Error";
        response.error = errBuf;
        response.body = response.error;

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return response;
    }

    // Read status code
    DWORD statusCode = 0;
    DWORD dwSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &dwSize, nullptr)) {
        response.statusCode = static_cast<int>(statusCode);
    }

    // Read status text
    wchar_t statusTextBuf[256]{};
    dwSize = sizeof(statusTextBuf);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_TEXT, nullptr, statusTextBuf, &dwSize, nullptr)) {
        response.statusText = WideToUtf8(statusTextBuf);
    }

    // Read response headers
    dwSize = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, WINHTTP_NO_OUTPUT_BUFFER, &dwSize, nullptr);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && dwSize > 0) {
        std::vector<wchar_t> headerBuf(dwSize / sizeof(wchar_t) + 1, 0);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, headerBuf.data(), &dwSize, nullptr)) {
            std::wstring rawHeaders(headerBuf.data());
            std::wstringstream wss(rawHeaders);
            std::wstring line;
            while (std::getline(wss, line)) {
                if (line.empty() || line == L"\r") continue;
                size_t colon = line.find(L':');
                if (colon != std::wstring::npos) {
                    std::string key = WideToUtf8(line.substr(0, colon));
                    std::string val = WideToUtf8(line.substr(colon + 1));
                    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
                    while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
                    response.headers.push_back({key, val, true, ""});

                    if (_stricmp(key.c_str(), "Content-Type") == 0) {
                        response.contentType = val;
                        if (val.find("json") != std::string::npos) {
                            response.isJson = true;
                        }
                    }
                }
            }
        }
    }

    // Read response body
    std::string responseBody;
    DWORD dwDownloaded = 0;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;

        std::vector<char> buffer(dwSize + 1, 0);
        if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
            responseBody.append(buffer.data(), dwDownloaded);
        }
    } while (dwSize > 0);

    response.body = responseBody;
    response.sizeBytes = responseBody.size();

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // Evaluate tests
    response.testResults = AssertionsEvaluator::Evaluate(
        req.tests,
        response.statusCode,
        response.latencyMs,
        response.headers,
        response.body
    );

    return response;
}

} // namespace native_app
