#include "VariableResolver.h"
#include <regex>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace native_app {

bool VariableResolver::IsSensitiveKey(const std::string& key) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return (lower.find("password") != std::string::npos ||
            lower.find("passwd") != std::string::npos ||
            lower.find("token") != std::string::npos ||
            lower.find("secret") != std::string::npos ||
            lower.find("api_key") != std::string::npos ||
            lower.find("apikey") != std::string::npos ||
            lower.find("authorization") != std::string::npos ||
            lower.find("cookie") != std::string::npos);
}

std::string VariableResolver::RedactSensitive(const std::string& key, const std::string& value) {
    if (value.empty()) return value;
    if (value.rfind("{{", 0) == 0 && value.find("}}") != std::string::npos) return value;
    if (value.rfind("Bearer {{", 0) == 0) return value;
    if (IsSensitiveKey(key)) return "[REDACTED]";
    return value;
}

std::string VariableResolver::UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::uppercase << static_cast<int>(c);
        }
    }
    return escaped.str();
}

std::string VariableResolver::UrlDecode(const std::string& value) {
    std::string res;
    for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] == '%' && i + 2 < value.length()) {
            std::string hexStr = value.substr(i + 1, 2);
            char ch = static_cast<char>(std::strtol(hexStr.c_str(), nullptr, 16));
            res += ch;
            i += 2;
        } else if (value[i] == '+') {
            res += ' ';
        } else {
            res += value[i];
        }
    }
    return res;
}

std::vector<std::string> VariableResolver::FindVariables(const std::string& text) {
    std::vector<std::string> vars;
    std::regex varRegex(R"(\{\{([^}]+)\}\})");
    auto words_begin = std::sregex_iterator(text.begin(), text.end(), varRegex);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        std::string varName = match[1].str();
        size_t first = varName.find_first_not_of(" \t\r\n");
        size_t last = varName.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && last != std::string::npos) {
            varName = varName.substr(first, (last - first + 1));
        }
        if (std::find(vars.begin(), vars.end(), varName) == vars.end()) {
            vars.push_back(varName);
        }
    }
    return vars;
}

std::string VariableResolver::Substitute(const std::string& text, const std::map<std::string, std::string>& variables) {
    if (text.empty()) return text;
    std::string current = text;

    for (int pass = 0; pass < 10; ++pass) {
        std::string next;
        size_t pos = 0;
        bool changed = false;

        while (pos < current.length()) {
            size_t start = current.find("{{", pos);
            if (start == std::string::npos) {
                next += current.substr(pos);
                break;
            }
            size_t end = current.find("}}", start + 2);
            if (end == std::string::npos) {
                next += current.substr(pos);
                break;
            }

            next += current.substr(pos, start - pos);
            std::string key = current.substr(start + 2, end - (start + 2));
            size_t first = key.find_first_not_of(" \t\r\n");
            size_t last = key.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos) {
                key = key.substr(first, (last - first + 1));
            }

            auto it = variables.find(key);
            if (it != variables.end()) {
                next += it->second;
                changed = true;
            } else {
                next += current.substr(start, end + 2 - start);
            }
            pos = end + 2;
        }

        if (!changed || next == current) {
            break;
        }
        current = next;
    }
    return current;
}

ApiRequest VariableResolver::PrepareRequest(const ApiRequest& input, const std::map<std::string, std::string>& variables, bool strict) {
    ApiRequest req = input;
    std::map<std::string, std::string> vars = variables;

    // Add path parameters to variables
    for (const auto& p : req.pathParams) {
        if (p.enabled && !p.key.empty()) {
            std::string val = Substitute(p.value, vars);
            vars[p.key] = val;
        }
    }

    // Substitute URL
    std::string url = Substitute(req.url, vars);

    // Replace :segment path parameters in URL
    for (const auto& p : req.pathParams) {
        if (p.enabled && !p.key.empty()) {
            std::string target = ":" + p.key;
            std::string replacement = UrlEncode(Substitute(p.value, vars));
            size_t pos = 0;
            while ((pos = url.find(target, pos)) != std::string::npos) {
                url.replace(pos, target.length(), replacement);
                pos += replacement.length();
            }
        }
    }

    // Build query parameters
    std::string queryString;
    for (const auto& p : req.params) {
        if (p.enabled && !p.key.empty()) {
            std::string key = UrlEncode(Substitute(p.key, vars));
            std::string val = UrlEncode(Substitute(p.value, vars));
            if (!queryString.empty()) queryString += "&";
            queryString += key + "=" + val;
        }
    }

    if (!queryString.empty()) {
        if (url.find('?') == std::string::npos) {
            url += "?" + queryString;
        } else {
            url += "&" + queryString;
        }
    }
    req.url = url;

    // Substitute headers
    for (auto& h : req.headers) {
        if (h.enabled) {
            h.key = Substitute(h.key, vars);
            h.value = Substitute(h.value, vars);
        }
    }

    // Auth handling
    if (req.auth.type == AuthType::Bearer) {
        std::string token = Substitute(req.auth.token, vars);
        req.headers.push_back({"Authorization", "Bearer " + token, true, ""});
    } else if (req.auth.type == AuthType::Basic) {
        // Base64 encode username:password
        std::string user = Substitute(req.auth.username, vars);
        std::string pass = Substitute(req.auth.password, vars);
        std::string creds = user + ":" + pass;
        // Basic Base64
        static const char b64table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        int val = 0, valb = -6;
        for (unsigned char c : creds) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                encoded.push_back(b64table[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) encoded.push_back(b64table[((val << 8) >> (valb + 8)) & 0x3F]);
        while (encoded.size() % 4) encoded.push_back('=');

        req.headers.push_back({"Authorization", "Basic " + encoded, true, ""});
    } else if (req.auth.type == AuthType::ApiKey) {
        std::string key = Substitute(req.auth.key, vars);
        std::string val = Substitute(req.auth.value, vars);
        if (!key.empty()) {
            if (req.auth.in == "query") {
                std::string q = UrlEncode(key) + "=" + UrlEncode(val);
                if (req.url.find('?') == std::string::npos) req.url += "?" + q;
                else req.url += "&" + q;
            } else {
                req.headers.push_back({key, val, true, ""});
            }
        }
    }

    // Body
    if (req.body.type == BodyType::Json || req.body.type == BodyType::Raw) {
        req.body.content = Substitute(req.body.content, vars);
        if (req.body.type == BodyType::Json) {
            bool hasContentType = false;
            for (const auto& h : req.headers) {
                if (_stricmp(h.key.c_str(), "Content-Type") == 0) {
                    hasContentType = true;
                    break;
                }
            }
            if (!hasContentType) {
                req.headers.push_back({"Content-Type", "application/json", true, ""});
            }
        }
    } else if (req.body.type == BodyType::UrlEncoded || req.body.type == BodyType::FormData) {
        for (auto& item : req.body.formItems) {
            if (item.enabled) {
                item.key = Substitute(item.key, vars);
                item.value = Substitute(item.value, vars);
            }
        }
    }

    if (strict) {
        auto unres = FindVariables(req.url);
        if (!unres.empty()) {
            throw std::runtime_error("Unresolved variable in URL: " + unres[0]);
        }
    }

    return req;
}

} // namespace native_app
