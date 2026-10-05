#pragma once

#include "Types.h"
#include <string>

namespace native_app {

class OpenApiParser {
public:
    static Collection Parse(const std::string& specJson);
};

} // namespace native_app
