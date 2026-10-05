#pragma once

#include "Types.h"
#include <string>

namespace native_app {

class CodeGen {
public:
    static std::string Generate(const ApiRequest& req, const std::string& lang);
};

} // namespace native_app
