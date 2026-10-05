#include "ArchiveService.h"
#include <fstream>
#include <vector>
#include <cstdint>
#include <array>
#include <filesystem>
#include <limits>
#include <windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

namespace native_app {

static uint32_t CalculateCRC32(const std::string& data) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
            }
            values[i] = c;
        }
        return values;
    }();

    uint32_t crc = 0xFFFFFFFFL;
    for (unsigned char b : data) {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFL;
}

static void Write16(std::ofstream& out, uint16_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

static void Write32(std::ofstream& out, uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

bool ArchiveService::CreateZip(const std::string& destinationZipPath, const std::map<std::string, std::string>& files) {
    namespace fs = std::filesystem;
    if (files.size() > 65535) return false;
    uint64_t projected = 22;
    for (const auto& file : files) {
        const auto name = fs::u8path(file.first);
        if (file.first.empty() || file.first.size() > 65535 || file.first.find_first_of("\\:") != std::string::npos || name.is_absolute() || name.has_root_name()) return false;
        for (const auto& component : name) if (component == ".." || component == ".") return false;
        projected += file.second.size() + 76 + file.first.size() * 2;
        if (projected > std::numeric_limits<uint32_t>::max()) return false;
    }
    const auto destination = fs::u8path(destinationZipPath);
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) return false;
    wchar_t identifier[40]{}; StringFromGUID2(guid, identifier, 40);
    const auto temporary = fs::path(destination.wstring() + identifier + L".pending");
    struct TemporaryOwner { fs::path path; ~TemporaryOwner() { std::error_code ec; fs::remove(path, ec); } } owner{temporary};
    std::ofstream zip(temporary, std::ios::binary);
    if (!zip.is_open()) return false;

    struct CentralDirEntry {
        std::string filename;
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t localHeaderOffset = 0;
    };

    std::vector<CentralDirEntry> centralDir;

    for (const auto& pair : files) {
        CentralDirEntry entry;
        entry.filename = pair.first;
        entry.size = static_cast<uint32_t>(pair.second.size());
        entry.crc = CalculateCRC32(pair.second);
        entry.localHeaderOffset = static_cast<uint32_t>(zip.tellp());

        // Local file header signature = 0x04034b50
        Write32(zip, 0x04034b50);
        Write16(zip, 20); // version needed
        Write16(zip, 0x0800);  // UTF-8 filename flag
        Write16(zip, 0);  // compression method = store
        Write16(zip, 0);  // mod time
        Write16(zip, 0x0021);  // 1980-01-01
        Write32(zip, entry.crc);
        Write32(zip, entry.size); // compressed size
        Write32(zip, entry.size); // uncompressed size
        Write16(zip, static_cast<uint16_t>(entry.filename.size()));
        Write16(zip, 0); // extra length

        zip.write(entry.filename.data(), entry.filename.size());
        zip.write(pair.second.data(), pair.second.size());

        centralDir.push_back(entry);
    }

    uint32_t centralDirOffset = static_cast<uint32_t>(zip.tellp());

    // Write central directory
    for (const auto& entry : centralDir) {
        // Central directory signature = 0x02014b50
        Write32(zip, 0x02014b50);
        Write16(zip, 20); // version made by
        Write16(zip, 20); // version needed
        Write16(zip, 0);  // flags
        Write16(zip, 0);  // compression method
        Write16(zip, 0);  // mod time
        Write16(zip, 0);  // mod date
        Write32(zip, entry.crc);
        Write32(zip, entry.size);
        Write32(zip, entry.size);
        Write16(zip, static_cast<uint16_t>(entry.filename.size()));
        Write16(zip, 0); // extra length
        Write16(zip, 0); // comment length
        Write16(zip, 0); // disk start
        Write16(zip, 0); // internal attr
        Write32(zip, 0); // external attr
        Write32(zip, entry.localHeaderOffset);

        zip.write(entry.filename.data(), entry.filename.size());
    }

    uint32_t centralDirEnd = static_cast<uint32_t>(zip.tellp());
    uint32_t centralDirSize = centralDirEnd - centralDirOffset;

    // End of central directory record signature = 0x06054b50
    Write32(zip, 0x06054b50);
    Write16(zip, 0); // disk number
    Write16(zip, 0); // start disk
    Write16(zip, static_cast<uint16_t>(centralDir.size())); // entries on this disk
    Write16(zip, static_cast<uint16_t>(centralDir.size())); // total entries
    Write32(zip, centralDirSize);
    Write32(zip, centralDirOffset);
    Write16(zip, 0); // comment length

    zip.flush();
    if (!zip) return false;
    zip.close();
    if (!zip) return false;
    return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

} // namespace native_app
