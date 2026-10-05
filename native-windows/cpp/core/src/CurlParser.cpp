#include "CurlParser.h"
#include "VariableResolver.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace native_app {
namespace {
std::vector<std::string> Tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool single = false, quoted = false, started = false;
    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (!single && (c == '\\' || c == '^' || c == '`') && i + 1 < input.size()) {
            size_t newline = i + 1;
            if (input[newline] == '\r' && newline + 1 < input.size()) ++newline;
            if (input[newline] == '\n') { i = newline; continue; }
        }
        if (c == '\\' && !single) {
            if (i + 1 == input.size()) throw std::runtime_error("Incomplete escape in cURL command.");
            const auto next = input[i + 1];
            // Inside double quotes, preserve Windows path separators and JSON escapes.
            if (!quoted || next == '\\' || next == '"' || next == '$' || next == '`') { current += next; ++i; }
            else current += c;
            started = true;
        } else if (c == '\'' && !quoted) { single = !single; started = true; }
        else if (c == '"' && !single) { quoted = !quoted; started = true; }
        else if ((c == ' ' || c == '\t' || c == '\r' || c == '\n') && !single && !quoted) {
            if (started) { tokens.push_back(current); current.clear(); started = false; }
        } else { current += c; started = true; }
    }
    if (single || quoted) throw std::runtime_error("Unterminated quote in cURL command.");
    if (started) tokens.push_back(current);
    return tokens;
}
void Trim(std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    text = first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t") - first + 1);
}
void AppendQuery(std::string& url, const std::string& query) {
    if (query.empty()) return;
    const auto fragment = url.find('#');
    const auto end = fragment == std::string::npos ? url.size() : fragment;
    const bool hasQuery = url.substr(0, end).find('?') != std::string::npos;
    const bool separator = end && url[end - 1] != '?' && url[end - 1] != '&';
    url.insert(end, (hasQuery ? (separator ? "&" : "") : "?") + query);
}
}

ApiRequest CurlParser::Parse(const std::string& curlCommand) {
    const auto raw = Tokenize(curlCommand);
    if (raw.empty() || (_stricmp(raw[0].c_str(), "curl") && _stricmp(raw[0].c_str(), "curl.exe")))
        throw std::runtime_error("Command must start with curl.");
    std::vector<std::string> tokens;
    for (size_t i = 1; i < raw.size(); ++i) {
        const auto& token = raw[i];
        const auto equals = token.find('=');
        if (token.rfind("--", 0) == 0 && equals != std::string::npos) {
            tokens.push_back(token.substr(0, equals)); tokens.push_back(token.substr(equals + 1));
        } else if (token.size() > 2 && token[0] == '-' && std::string("XHduFAbeo").find(token[1]) != std::string::npos) {
            tokens.push_back(token.substr(0, 2)); tokens.push_back(token.substr(2));
        } else tokens.push_back(token);
    }
    ApiRequest req;
    req.options.followRedirects = false; // cURL follows redirects only with -L.
    req.tests.push_back({"status_code", "200", "", "", "Status code is 200", true});
    std::string explicitMethod, url;
    std::vector<std::string> data;
    bool useGet = false, useHead = false, json = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto token = tokens[i];
        auto next = [&]() {
            if (i + 1 >= tokens.size()) throw std::runtime_error("Missing value for option: " + token);
            return tokens[++i];
        };
        if (token == "-X" || token == "--request") explicitMethod = next();
        else if (token == "-H" || token == "--header") {
            const auto header = next(); const auto colon = header.find(':');
            if (colon == std::string::npos) throw std::runtime_error("cURL headers must use Name: value.");
            auto key = header.substr(0, colon), value = header.substr(colon + 1); Trim(key); Trim(value);
            if (key.empty()) throw std::runtime_error("cURL header name cannot be empty.");
            req.headers.push_back({key, value, true, ""});
        } else if (token == "-d" || token == "--data" || token == "--data-raw" || token == "--data-binary" || token == "--data-ascii" || token == "--data-urlencode" || token == "--json") {
            auto value = next();
            if (token != "--data-raw" && !value.empty() && value[0] == '@')
                throw std::runtime_error("cURL body files are not read during import. Attach the file in the body editor.");
            if (token == "--data-urlencode") {
                const auto equals = value.find('=');
                if (equals == std::string::npos) {
                    if (value.find('@') != std::string::npos) throw std::runtime_error("Attach cURL URL-encoded files in the body editor.");
                    value = VariableResolver::UrlEncode(value);
                } else value = value.substr(0, equals + 1) + VariableResolver::UrlEncode(value.substr(equals + 1));
            }
            data.push_back(value); if (token == "--json") json = true;
        } else if (token == "-F" || token == "--form" || token == "--form-string") {
            const auto field = next(); const auto equals = field.find('=');
            if (equals == std::string::npos || equals == 0) throw std::runtime_error("cURL form fields must use name=value.");
            FormItem item; item.key = field.substr(0, equals); item.value = field.substr(equals + 1);
            if (token != "--form-string" && !item.value.empty()) {
                if (item.value[0] == '<') throw std::runtime_error("Attach cURL form files in the body editor.");
                const auto modifier = item.value.find(';');
                const auto source = item.value.substr(0, modifier);
                if (!source.empty() && source[0] == '@') {
                    const auto path = source.substr(1);
                    if (path.empty() || path.find(',') != std::string::npos) throw std::runtime_error("Import one named file per cURL form field.");
                    item.type = "file";
                    const auto slash = path.find_last_of("/\\"); item.filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
                    if (item.filename.empty()) throw std::runtime_error("cURL form file requires a filename.");
                    item.value.clear(); // Preserve a descriptor for reattachment; never open the referenced path.
                } else item.value = source;
                if (modifier != std::string::npos) {
                    std::stringstream modifiers(field.substr(equals + 1 + modifier + 1)); std::string value;
                    while (std::getline(modifiers, value, ';')) {
                        if (value.rfind("type=", 0) == 0) item.contentType = value.substr(5);
                        else if (value.rfind("filename=", 0) == 0 && item.type == "file") item.filename = value.substr(9);
                        else throw std::runtime_error("Unsupported cURL form modifier: " + value);
                    }
                }
                if (item.type == "file" && item.filename.empty()) throw std::runtime_error("cURL form file requires a filename.");
            }
            req.body.type = BodyType::FormData; req.body.formItems.push_back(item);
        } else if (token == "-u" || token == "--user") {
            const auto credentials = next(); const auto colon = credentials.find(':');
            req.auth.type = AuthType::Basic; req.auth.username = credentials.substr(0, colon);
            req.auth.password = colon == std::string::npos ? "" : credentials.substr(colon + 1);
        } else if (token == "--url") {
            if (!url.empty()) throw std::runtime_error("Import one cURL URL at a time."); url = next();
        } else if (token == "-A" || token == "--user-agent") req.headers.push_back({"User-Agent", next(), true, ""});
        else if (token == "-b" || token == "--cookie") {
            const auto cookies = next();
            if (cookies.find('=') == std::string::npos) throw std::runtime_error("Cookie files are not read during cURL import. Paste cookie values instead.");
            req.headers.push_back({"Cookie", cookies, true, ""});
        } else if (token == "-e" || token == "--referer") req.headers.push_back({"Referer", next(), true, ""});
        else if (token == "-L" || token == "--location") req.options.followRedirects = true;
        else if (token == "-k" || token == "--insecure") req.options.verifyTls = false;
        else if (token == "-G" || token == "--get") useGet = true;
        else if (token == "-I" || token == "--head") useHead = true;
        else if (token == "--max-time" || token == "--connect-timeout") {
            const auto value = next(); size_t used = 0;
            try { req.options.timeoutSec = std::stod(value, &used); }
            catch (...) { throw std::runtime_error("Invalid cURL timeout."); }
            if (used != value.size() || !std::isfinite(req.options.timeoutSec) || req.options.timeoutSec <= 0 || req.options.timeoutSec > 3600)
                throw std::runtime_error("cURL timeout must be greater than zero and at most 3600 seconds.");
        } else if (token == "-o" || token == "--output") { next(); }
        else if (token == "-s" || token == "-S" || token == "-sS" || token == "-v" || token == "-i" || token == "--silent" || token == "--show-error" || token == "--verbose" || token == "--include" || token == "--compressed" || token == "--globoff" || token == "--http1.1" || token == "--http2") {}
        else if (!token.empty() && token[0] == '-') throw std::runtime_error("Unsupported cURL option: " + token);
        else { if (!url.empty()) throw std::runtime_error("Import one cURL URL at a time."); url = token; }
    }
    if (url.empty()) throw std::runtime_error("cURL command did not contain a URL.");
    if (!data.empty() && !req.body.formItems.empty()) throw std::runtime_error("cURL data and multipart body options cannot be combined.");
    if (useGet && !req.body.formItems.empty()) throw std::runtime_error("cURL --get cannot combine with multipart form options.");
    if (!explicitMethod.empty()) {
        req.method = HttpMethodFromString(explicitMethod);
        if (_stricmp(HttpMethodToString(req.method).c_str(), explicitMethod.c_str())) throw std::runtime_error("Unsupported cURL request method: " + explicitMethod);
    } else req.method = useHead ? HttpMethod::HEAD : useGet ? HttpMethod::GET : !data.empty() || !req.body.formItems.empty() ? HttpMethod::POST : HttpMethod::GET;
    std::string joined;
    for (size_t i = 0; i < data.size(); ++i) { if (i) joined += json ? "" : "&"; joined += data[i]; }
    auto addHeader = [&](const char* key, const char* value) {
        if (std::none_of(req.headers.begin(), req.headers.end(), [&](const auto& header) { return _stricmp(header.key.c_str(), key) == 0; })) req.headers.push_back({key, value, true, ""});
    };
    if (useGet) AppendQuery(url, joined);
    else if (!data.empty()) {
        const bool isJson = json || joined.rfind("{", 0) == 0 || joined.rfind("[", 0) == 0;
        req.body.type = isJson ? BodyType::Json : BodyType::Raw; req.body.content = joined;
        addHeader("Content-Type", isJson ? "application/json" : "application/x-www-form-urlencoded");
        if (json) addHeader("Accept", "application/json");
    }
    req.url = url;
    for (auto it = req.headers.begin(); it != req.headers.end();) {
        if (req.auth.type == AuthType::None && !_stricmp(it->key.c_str(), "Authorization") && !_strnicmp(it->value.c_str(), "Bearer ", 7)) {
            req.auth.type = AuthType::Bearer; req.auth.token = it->value.substr(7); it = req.headers.erase(it);
        } else ++it;
    }
    const auto fragment = url.find('#'), query = url.find('?');
    if (query != std::string::npos && (fragment == std::string::npos || query < fragment)) {
        std::stringstream pairs(url.substr(query + 1, (fragment == std::string::npos ? url.size() : fragment) - query - 1));
        std::string pair;
        while (std::getline(pairs, pair, '&')) {
            if (pair.empty()) continue;
            const auto equals = pair.find('=');
            const auto key = VariableResolver::UrlDecode(pair.substr(0, equals));
            const auto value = equals == std::string::npos ? "" : VariableResolver::UrlDecode(pair.substr(equals + 1));
            if (!key.empty()) req.params.push_back({key, value, true, ""});
        }
    }
    return req;
}
} // namespace native_app
