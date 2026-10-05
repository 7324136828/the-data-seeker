#pragma once

#include <string>
#include <vector>
#include <map>

namespace native_app {

class ArchiveService {
public:
    // Create a zip file containing given virtual files (path in zip -> file content)
    static bool CreateZip(const std::string& destinationZipPath, const std::map<std::string, std::string>& files);
};

} // namespace native_app
