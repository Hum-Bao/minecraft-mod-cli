#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace modrinth_cli::files {

struct FileHashRecord {
        std::filesystem::path path;
        std::string hash;
};

class FileHasher {
    public:
        static std::string getSha512(const std::filesystem::path& file_path);
};

class FileScanner {
    public:
        static std::vector<FileHashRecord> collectModFileHashes(
            const std::filesystem::path& mods_path);
};

}  // namespace modrinth_cli::files
