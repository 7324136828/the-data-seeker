#pragma once

#include <string>
#include <vector>

namespace native_app {

struct ProcessResult {
    int exitCode = -1;
    std::string stdOut;
    std::string stdErr;
    bool success = false;
};

class ProcessRunner {
public:
    static ProcessResult Run(const std::string& applicationPath, const std::vector<std::string>& arguments, const std::string& workingDir = "");
};

} // namespace native_app
