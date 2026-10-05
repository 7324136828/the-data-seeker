#pragma once

#include "Types.h"
#include <string>
#include <map>

namespace native_app {

class HttpEngine {
public:
    HttpEngine() = default;
    ~HttpEngine() = default;

    ApiResponse Execute(
        const ApiRequest& request,
        const std::map<std::string, std::string>& variables = {},
        bool strictVariables = false
    );
};

} // namespace native_app
