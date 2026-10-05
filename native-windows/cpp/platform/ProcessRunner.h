#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace native_app {

struct ProcessResult {
    int exitCode = -1;
    std::string stdOut;
    std::string stdErr;
    bool success = false;
};

class ProcessRunner {
public:
    static ProcessResult Run(const std::string& applicationPath, const std::vector<std::string>& arguments, const std::string& workingDir = "", uint32_t timeoutMs = 30000);
};

} // namespace native_app
