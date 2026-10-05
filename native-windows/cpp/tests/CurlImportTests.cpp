#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "CurlParser.h"
#include "VariableResolver.h"
#include "HttpEngine.h"
#include <thread>
#include <string>
#include <stdexcept>
#include <algorithm>

using namespace native_app;
namespace {
void RequireCurl(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void RejectCurl(F action, const char* message) {
    bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; }
    RequireCurl(rejected, message);
}
class HttpCapture {
    SOCKET listener_ = INVALID_SOCKET;
    std::thread worker_;
    unsigned short port_ = 0;
    std::string captured_, failure_;
public:
    explicit HttpCapture(bool redirect = false) {
        WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Cannot start HTTP import fixture.");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener_, 1)) {
            if (listener_ != INVALID_SOCKET) closesocket(listener_); WSACleanup(); throw std::runtime_error("Cannot bind HTTP import fixture.");
        }
        int size = sizeof(address); getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size); port_ = ntohs(address.sin_port);
        worker_ = std::thread([this, redirect] {
            const auto client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            const DWORD timeout = 4000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            try {
                size_t total = std::string::npos;
                while (captured_.size() < 1024 * 1024 && (total == std::string::npos || captured_.size() < total)) {
                    char buffer[4096]; const int count = recv(client, buffer, sizeof(buffer), 0);
                    if (count <= 0) throw std::runtime_error("HTTP import fixture read failed.");
                    captured_.append(buffer, count);
                    const auto end = captured_.find("\r\n\r\n");
                    if (end != std::string::npos && total == std::string::npos) {
                        auto header = captured_.substr(0, end); std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
                        const auto length = header.find("\r\ncontent-length:");
                        total = end + 4 + (length == std::string::npos ? 0 : static_cast<size_t>(std::stoull(header.substr(length + 17))));
                    }
                }
                if (captured_.size() != total) throw std::runtime_error("HTTP import fixture exceeded bounds.");
                const std::string response = redirect ? "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/should-not-follow\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
                size_t sent = 0; while (sent < response.size()) { const int count = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0); if (count <= 0) throw std::runtime_error("HTTP import fixture write failed."); sent += count; }
            } catch (const std::exception& ex) { failure_ = ex.what(); }
            closesocket(client);
        });
    }
    ~HttpCapture() { closesocket(listener_); if (worker_.joinable()) worker_.join(); WSACleanup(); }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/capture"; }
    std::string Captured() { if (worker_.joinable()) worker_.join(); if (!failure_.empty()) throw std::runtime_error(failure_); return captured_; }
};
}

void TestNativeCurlImportRoundTrips() {
    const auto parsed = CurlParser::Parse("curl 'https://example.invalid/a%20b?tag=one&tag=two&empty=&flag&space=a+b&plus=%2B&utf8=%E2%98%83#result?ignored=yes'");
    RequireCurl(parsed.params.size() == 7 && parsed.params[0].value == "one" && parsed.params[1].value == "two" && parsed.params[2].value.empty() && parsed.params[4].value == "a b" && parsed.params[5].value == "+", "cURL query decoding lost duplicate/blank/plus semantics.");
    const auto prepared = VariableResolver::PrepareRequest(parsed, {});
    RequireCurl(prepared.url == "https://example.invalid/a%20b?tag=one&tag=two&empty=&flag=&space=a%20b&plus=%2B&utf8=%E2%98%83#result?ignored=yes", "cURL query was duplicated, re-encoded, or absorbed its fragment.");
    RequireCurl(!parsed.options.followRedirects && CurlParser::Parse("curl -L https://example.invalid").options.followRedirects, "cURL redirect defaults differ from curl.");
    RequireCurl(CurlParser::Parse("curl 'https://example.invalid/#fragment?not=query'").params.empty(), "Fragment was imported as query parameters.");
    ApiRequest overridden;
    overridden.url = "https://example.invalid/?keep=a+b&tag=old&tag=older&drop=x&empty=&plus=%2B&flag#result";
    overridden.params = {{"tag", "new", true}, {"tag", "", true}, {"drop", "", false}, {"{{key}}", "a&b", true}};
    RequireCurl(VariableResolver::PrepareRequest(overridden, {{"key", "added"}}, true).url == "https://example.invalid/?keep=a%20b&empty=&plus=%2B&flag=&tag=new&tag=&added=a%26b#result", "Editable query overrides lost URL pairs, disabled removal, duplicate order, or variables.");
    ApiRequest variableQuery; variableQuery.url = "https://example.invalid/?q={{value}}#result";
    RequireCurl(VariableResolver::PrepareRequest(variableQuery, {{"value", "a & b"}}, true).url == "https://example.invalid/?q=a%20%26%20b#result", "A raw URL query variable created extra parameters.");
    RejectCurl([&] { VariableResolver::PrepareRequest(variableQuery, {}, true); }, "Strict URL query validation hid an unresolved variable in URL encoding.");
    const auto get = CurlParser::Parse("curl -G 'https://example.invalid/?existing=1#result' --data-urlencode 'search=a+b & c' -d 'repeat=1&repeat=2&blank='");
    RequireCurl(get.method == HttpMethod::GET && get.body.type == BodyType::None && VariableResolver::PrepareRequest(get, {}).url == "https://example.invalid/?existing=1&search=a%2Bb%20%26%20c&repeat=1&repeat=2&blank=#result", "cURL --get data was retained in the body or incorrectly encoded.");
    const auto empty = CurlParser::Parse("curl https://example.invalid -d ''");
    RequireCurl(empty.method == HttpMethod::POST && empty.body.type == BodyType::Raw && empty.body.content.empty(), "Empty quoted cURL payload disappeared.");
    const auto form = CurlParser::Parse("curl https://example.invalid --form-string 'text=@literal;type=text/plain' -F 'upload=@C:\\private\\example.bin;type=application/octet-stream;filename=reattach.bin'");
    RequireCurl(form.body.type == BodyType::FormData && form.body.formItems.size() == 2 && form.body.formItems[0].type == "text" && form.body.formItems[0].value == "@literal;type=text/plain" && form.body.formItems[1].type == "file" && form.body.formItems[1].filename == "reattach.bin" && form.body.formItems[1].value.empty() && form.body.formItems[1].contentBase64.empty(), "Multipart import read a file or changed a literal/form descriptor.");
    for (const auto& command : {"curl 'https://example.invalid", "curl https://example.invalid --max-time", "curl https://example.invalid --max-time nonsense", "curl https://example.invalid --unknown", "curl https://example.invalid -d @private.json", "curl https://example.invalid -F 'body=<private.txt'", "curl https://example.invalid -H malformed"}) RejectCurl([&] { CurlParser::Parse(command); }, "Malformed/unsupported cURL input was silently accepted.");
    {
        HttpCapture capture;
        const auto request = CurlParser::Parse("curl -X GET '" + capture.Url() + "?q=a+b&q=%2B&empty=#ignored' --data-raw 'GET body' --max-time 3");
        const auto response = HttpEngine{}.Execute(request);
        RequireCurl(response.error.empty() && response.statusCode == 200, "Imported cURL could not dispatch through the native HTTP engine.");
        const auto wire = capture.Captured();
        RequireCurl(wire.rfind("GET /capture?q=a%20b&q=%2B&empty= HTTP/1.1\r\n", 0) == 0 && wire.substr(wire.find("\r\n\r\n") + 4) == "GET body", "Native dispatch changed the imported query/fragment or dropped its GET body.");
    }
    {
        HttpCapture capture;
        auto request = CurlParser::Parse("curl '" + capture.Url() + "' --form-string 'text=@literal' -F 'upload=@reattach.bin' --max-time 3");
        request.body.formItems[1].contentBase64 = "QQBC";
        const auto response = HttpEngine{}.Execute(request);
        RequireCurl(response.error.empty() && response.statusCode == 200, "Reattached imported multipart request failed native dispatch.");
        const auto wire = capture.Captured();
        RequireCurl(wire.find("name=\"text\"\r\n\r\n@literal") != std::string::npos && wire.find("filename=\"reattach.bin\"") != std::string::npos && wire.find(std::string("A\0B", 3)) != std::string::npos, "Native multipart dispatch lost literal text, the filename, or binary bytes.");
    }
    {
        HttpCapture capture(true);
        const auto response = HttpEngine{}.Execute(CurlParser::Parse("curl '" + capture.Url() + "' --max-time 3"));
        RequireCurl(response.error.empty() && response.statusCode == 302, "Imported cURL followed a redirect without -L."); capture.Captured();
    }
}
