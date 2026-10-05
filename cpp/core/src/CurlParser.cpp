#include "CurlParser.h"
#include <sstream>
#include <vector>
#include <regex>
#include <algorithm>

namespace native_app {

static std::vector<std::string> Tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;
    bool escape = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (escape) {
            current += c;
            escape = false;
            continue;
        }

        if (c == '\\' && !inSingle) {
            escape = true;
            continue;
        }

        if (c == '\'' && !inDouble) {
            inSingle = !inSingle;
            continue;
        }

        if (c == '"' && !inSingle) {
            inDouble = !inDouble;
            continue;
        }

        if ((c == ' ' || c == '\t' || c == '\r' || c == '\n') && !inSingle && !inDouble) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

ApiRequest CurlParser::Parse(const std::string& curlCommand) {
    // Normalize line continuations
    std::string clean = curlCommand;
    clean = std::regex_replace(clean, std::regex(R"([\^`\\]\r?\n)"), " ");

    std::vector<std::string> rawTokens = Tokenize(clean);
    if (rawTokens.empty() || (_stricmp(rawTokens[0].c_str(), "curl") != 0 && _stricmp(rawTokens[0].c_str(), "curl.exe") != 0)) {
        throw std::runtime_error("Command must start with curl.");
    }

    // Expand joined flags like --header=val or -Hval
    std::vector<std::string> tokens;
    for (size_t i = 1; i < rawTokens.size(); ++i) {
        const std::string& t = rawTokens[i];
        if (t.rfind("--", 0) == 0 && t.find('=') != std::string::npos) {
            size_t eq = t.find('=');
            tokens.push_back(t.substr(0, eq));
            tokens.push_back(t.substr(eq + 1));
        } else if (t.length() > 2 && (t.rfind("-X", 0) == 0 || t.rfind("-H", 0) == 0 || t.rfind("-d", 0) == 0 ||
                                      t.rfind("-u", 0) == 0 || t.rfind("-F", 0) == 0 || t.rfind("-A", 0) == 0 || t.rfind("-b", 0) == 0)) {
            tokens.push_back(t.substr(0, 2));
            tokens.push_back(t.substr(2));
        } else {
            tokens.push_back(t);
        }
    }

    ApiRequest req;
    req.tests.push_back({"status_code", "200", "", "", "Status code is 200", true});
    std::string explicitMethod;
    std::string url;
    std::vector<std::string> dataTokens;
    bool useGet = false;
    bool useHead = false;
    bool isJson = false;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        auto getNextVal = [&]() -> std::string {
            if (i + 1 >= tokens.size()) {
                throw std::runtime_error("Missing value for option: " + token);
            }
            return tokens[++i];
        };

        if (token == "-X" || token == "--request") {
            explicitMethod = getNextVal();
        } else if (token == "-H" || token == "--header") {
            std::string h = getNextVal();
            size_t colon = h.find(':');
            if (colon != std::string::npos) {
                std::string k = h.substr(0, colon);
                std::string v = h.substr(colon + 1);
                while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(k.begin());
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                req.headers.push_back({k, v, true, ""});
            }
        } else if (token == "-d" || token == "--data" || token == "--data-raw" || token == "--data-binary" || token == "--json") {
            std::string d = getNextVal();
            dataTokens.push_back(d);
            if (token == "--json") isJson = true;
        } else if (token == "-F" || token == "--form") {
            std::string f = getNextVal();
            size_t eq = f.find('=');
            if (eq != std::string::npos) {
                std::string k = f.substr(0, eq);
                std::string v = f.substr(eq + 1);
                req.body.type = BodyType::FormData;
                req.body.formItems.push_back({k, v, true, "text", "", "", ""});
            }
        } else if (token == "-u" || token == "--user") {
            std::string u = getNextVal();
            size_t colon = u.find(':');
            req.auth.type = AuthType::Basic;
            req.auth.username = (colon != std::string::npos) ? u.substr(0, colon) : u;
            req.auth.password = (colon != std::string::npos) ? u.substr(colon + 1) : "";
        } else if (token == "--url") {
            url = getNextVal();
        } else if (token == "-A" || token == "--user-agent") {
            req.headers.push_back({"User-Agent", getNextVal(), true, ""});
        } else if (token == "-b" || token == "--cookie") {
            req.headers.push_back({"Cookie", getNextVal(), true, ""});
        } else if (token == "-e" || token == "--referer") {
            req.headers.push_back({"Referer", getNextVal(), true, ""});
        } else if (token == "-L" || token == "--location") {
            req.options.followRedirects = true;
        } else if (token == "-k" || token == "--insecure") {
            req.options.verifyTls = false;
        } else if (token == "-G" || token == "--get") {
            useGet = true;
        } else if (token == "-I" || token == "--head") {
            useHead = true;
        } else if (token == "--max-time" || token == "--connect-timeout") {
            try { req.options.timeoutSec = std::stod(getNextVal()); } catch (...) {}
        } else if (token.rfind("-", 0) == 0) {
            // Ignored flags like -s, -S, -v, etc.
        } else if (url.empty()) {
            url = token;
        }
    }

    if (url.empty()) {
        throw std::runtime_error("cURL command did not contain a URL.");
    }
    req.url = url;

    // Check Authorization header for Bearer / Basic
    for (auto it = req.headers.begin(); it != req.headers.end(); ) {
        if (_stricmp(it->key.c_str(), "Authorization") == 0) {
            if (req.auth.type == AuthType::None) {
                if (_strnicmp(it->value.c_str(), "Bearer ", 7) == 0) {
                    req.auth.type = AuthType::Bearer;
                    req.auth.token = it->value.substr(7);
                    it = req.headers.erase(it);
                    continue;
                }
            }
        }
        ++it;
    }

    // Determine method
    if (!explicitMethod.empty()) {
        req.method = HttpMethodFromString(explicitMethod);
    } else if (useHead) {
        req.method = HttpMethod::HEAD;
    } else if (useGet) {
        req.method = HttpMethod::GET;
    } else if (!dataTokens.empty() || !req.body.formItems.empty()) {
        req.method = HttpMethod::POST;
    } else {
        req.method = HttpMethod::GET;
    }

    // Body handling
    if (!dataTokens.empty()) {
        std::string joined;
        for (size_t i = 0; i < dataTokens.size(); ++i) {
            if (i > 0) joined += isJson ? "" : "&";
            joined += dataTokens[i];
        }
        if (isJson || (joined.rfind("{", 0) == 0 || joined.rfind("[", 0) == 0)) {
            req.body.type = BodyType::Json;
            bool hasCt = false;
            for (const auto& h : req.headers) {
                if (_stricmp(h.key.c_str(), "Content-Type") == 0) hasCt = true;
            }
            if (!hasCt) req.headers.push_back({"Content-Type", "application/json", true, ""});
        } else {
            req.body.type = BodyType::Raw;
        }
        req.body.content = joined;
    }

    // Extract query parameters from URL into params list
    size_t qPos = req.url.find('?');
    if (qPos != std::string::npos) {
        std::string qStr = req.url.substr(qPos + 1);
        std::stringstream ss(qStr);
        std::string pair;
        while (std::getline(ss, pair, '&')) {
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                req.params.push_back({pair.substr(0, eq), pair.substr(eq + 1), true, ""});
            } else if (!pair.empty()) {
                req.params.push_back({pair, "", true, ""});
            }
        }
    }

    return req;
}

} // namespace native_app
