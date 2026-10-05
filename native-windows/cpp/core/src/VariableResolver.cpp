#include "VariableResolver.h"
#include <regex>
#include "ImportDocument.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <cstdint>
#include <set>

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
    static const std::regex reference(R"((Bearer )?\{\{[A-Za-z0-9_. -]+\}\})");
    if (std::regex_match(value, reference)) return value;
    if (IsSensitiveKey(key)) return "[REDACTED]";
    return value;
}

std::string VariableResolver::UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
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
        if (value[i] == '%' && i + 2 < value.length() && isxdigit(static_cast<unsigned char>(value[i + 1])) && isxdigit(static_cast<unsigned char>(value[i + 2]))) {
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
    const auto iterationData=RequestIterationData(req);
    for(auto item=iterationData.begin();item!=iterationData.end();++item)vars[item.key()]=item.value().is_string()?item.value().get<std::string>():item.value().dump();
    std::set<std::string> localNames;
    for (const auto& v : req.variables) if (v.enabled && !v.key.empty()) {if(!localNames.insert(v.key).second)throw std::runtime_error("Enabled request variable names must be unique.");vars[v.key] = v.value;}
    auto appendQuery = [](std::string& url, const std::string& query) {
        const auto fragment = url.find('#');
        const auto insert = fragment == std::string::npos ? url.size() : fragment;
        const bool hasQuery = url.substr(0, insert).find('?') != std::string::npos;
        const bool needsSeparator = insert && url[insert - 1] != '?' && url[insert - 1] != '&';
        url.insert(insert, (hasQuery ? (needsSeparator ? "&" : "") : "?") + query);
    };

    // Add path parameters to variables
    for (const auto& p : req.pathParams) {
        if (p.enabled && !p.key.empty()) {
            std::string val = Substitute(p.value, vars);
            vars[p.key] = val;
        }
    }

    // Encode path variables only for the URL; bodies and headers keep values.
    auto urlVariables = vars;
    for (const auto& p : req.pathParams) if (p.enabled && !p.key.empty()) urlVariables[p.key] = UrlEncode(Substitute(p.value, vars));
    // Split the raw query before substitution: '&' inside a variable value is
    // data for that pair, rather than a separator for an accidental new pair.
    const auto rawFragmentStart = req.url.find('#');
    const auto rawQueryStart = req.url.find('?');
    const bool hasRawQuery = rawQueryStart != std::string::npos && (rawFragmentStart == std::string::npos || rawQueryStart < rawFragmentStart);
    const auto rawQuery = hasRawQuery ? req.url.substr(rawQueryStart + 1, (rawFragmentStart == std::string::npos ? req.url.size() : rawFragmentStart) - rawQueryStart - 1) : "";
    std::string url = Substitute(req.url.substr(0, hasRawQuery ? rawQueryStart : rawFragmentStart), urlVariables);
    if (rawFragmentStart != std::string::npos) url += Substitute(req.url.substr(rawFragmentStart), urlVariables);

    // Replace :segment path parameters in URL
    for (const auto& p : req.pathParams) {
        if (p.enabled && !p.key.empty()) {
            for (const auto& target : std::vector<std::string>{":" + p.key, "{" + p.key + "}"}) {
            std::string replacement = UrlEncode(Substitute(p.value, vars));
            size_t pos = 0;
            while ((pos = url.find(target, pos)) != std::string::npos) {
                const auto end = pos + target.size();
                const auto queryStart = url.find_first_of("?#");
                const auto authority = url.find("://");
                const auto pathStart = authority == std::string::npos ? 0 : url.find('/', authority + 3);
                const bool withinPath = pathStart != std::string::npos && pos >= pathStart && (queryStart == std::string::npos || end <= queryStart);
                const bool segment = target.front() == '{' ? withinPath : pos && url[pos - 1] == '/' && (end == url.size() || url[end] == '/' || url[end] == '?' || url[end] == '#');
                if (segment && withinPath) {
                    url.replace(pos, target.length(), replacement);
                    pos += replacement.length();
                } else pos += target.length();
            }
            }
        }
    }

    // Editable parameter rows override the same URL keys, including disabled rows.
    // Keep unrelated URL pairs and repeated rows in their original order.
    std::set<std::string> queryKeys;
    for (const auto& p : req.params) if (!p.key.empty()) queryKeys.insert(Substitute(p.key, vars));
    const auto fragmentStart = url.find('#');
    const std::string fragment = fragmentStart == std::string::npos ? "" : url.substr(fragmentStart);
    std::string base = url.substr(0, fragmentStart);
    const auto queryStart = base.find('?');
    std::string queryString;
    auto addPair = [&](const std::string& key, const std::string& value) {
        if (strict) {
            const auto unresolvedKey = FindVariables(key), unresolvedValue = FindVariables(value);
            if (!unresolvedKey.empty() || !unresolvedValue.empty()) throw std::runtime_error("Unresolved variable in URL query: " + (unresolvedKey.empty() ? unresolvedValue[0] : unresolvedKey[0]));
        }
        if (!queryString.empty()) queryString += "&";
        queryString += UrlEncode(key) + "=" + UrlEncode(value);
    };
    auto addUrlQuery = [&](const std::string& query) {
        std::stringstream pairs(query);
        std::string pair;
        while (std::getline(pairs, pair, '&')) {
            if (pair.empty()) continue;
            const auto equals = pair.find('=');
            const auto key = Substitute(UrlDecode(pair.substr(0, equals)), vars);
            const auto value = equals == std::string::npos ? "" : Substitute(UrlDecode(pair.substr(equals + 1)), vars);
            if (!queryKeys.count(key)) addPair(key, value);
        }
    };
    if (queryStart != std::string::npos) {
        const auto query = base.substr(queryStart + 1);
        base.resize(queryStart);
        addUrlQuery(query);
    }
    addUrlQuery(rawQuery);
    for (const auto& p : req.params) {
        if (p.enabled && !p.key.empty()) {
            addPair(Substitute(p.key, vars), Substitute(p.value, vars));
        }
    }
    req.url = base + (queryString.empty() ? "" : "?" + queryString) + fragment;

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
        uint32_t val = 0;
        int valb = -6;
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
                appendQuery(req.url, q);
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
                if (h.enabled && _stricmp(h.key.c_str(), "Content-Type") == 0) {
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
        auto validate = [](const std::string& value, const char* context) {
            auto unres = FindVariables(value);
            if (!unres.empty()) throw std::runtime_error(std::string("Unresolved variable in ") + context + ": " + unres[0]);
        };
        validate(req.url, "URL");
        // URL encoding must not conceal unresolved variable references.
        for (const auto& parameter : input.params) if (parameter.enabled) { validate(Substitute(parameter.key, vars), "query parameter"); validate(Substitute(parameter.value, vars), "query parameter"); }
        for (const auto& parameter : input.pathParams) if (parameter.enabled) validate(Substitute(parameter.value, vars), "path parameter");
        for (const auto& h : req.headers) if (h.enabled) { validate(h.key, "header"); validate(h.value, "header"); }
        validate(req.body.content, "body");
        for (const auto& item : req.body.formItems) if (item.enabled) { validate(item.key, "form field"); validate(item.value, "form field"); }
    }

    return req;
}

} // namespace native_app
