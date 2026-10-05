#pragma once

#include "Types.h"
#include <string>

namespace native_app {

class CurlParser {
public:
    static ApiRequest Parse(const std::string& curlCommand);
};

} // namespace native_app
