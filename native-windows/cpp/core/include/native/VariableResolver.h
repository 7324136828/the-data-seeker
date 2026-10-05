#pragma once

#include "Types.h"
#include <string>
#include <map>
#include <vector>

namespace native_app {

class VariableResolver {
public:
    static std::string Substitute(const std::string& text, const std::map<std::string, std::string>& variables);
    static std::vector<std::string> FindVariables(const std::string& text);
    static std::string UrlEncode(const std::string& value);
    static std::string UrlDecode(const std::string& value);
    static ApiRequest PrepareRequest(const ApiRequest& input, const std::map<std::string, std::string>& variables, bool strict = false);
    static std::string RedactSensitive(const std::string& key, const std::string& value);
    static bool IsSensitiveKey(const std::string& key);
};

} // namespace native_app
