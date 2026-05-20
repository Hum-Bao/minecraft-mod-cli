#ifndef MODRINTH_API_H
#define MODRINTH_API_H

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "CurseForgeAPI.h"
#include "FileInfo.h"
#include "ModDependency.h"
#include "ModVersionInfo.h"
#include "httplib.h"

namespace modrinth_cli::files {
struct FileHashRecord;
}  // namespace modrinth_cli::files

struct ProjectSideInfo {
        std::string client_side;
        std::string server_side;
};

class ModrinthAPI {
    public:
        static std::unordered_map<std::string, ModVersionInfo> fetchCurrentVersions(
            const std::vector<std::string>& file_hashes, size_t* response_bytes = nullptr,
            bool enable_curseforge_fallback = true);

        // Overload that accepts FileHashRecord for CurseForge fallback support
        static std::unordered_map<std::string, ModVersionInfo> fetchCurrentVersionsWithFallback(
            const std::vector<modrinth_cli::files::FileHashRecord>& file_records,
            size_t* response_bytes = nullptr);

        static std::unordered_map<std::string, ModVersionInfo> fetchLatestUpdates(
            const std::vector<std::string>& file_hashes, const std::string& game_version,
            const std::string& loader, size_t* response_bytes = nullptr);

        static std::optional<ModVersionInfo> fetchVersionById(const std::string& version_id);

        static std::unordered_map<std::string, std::string> fetchProjectTitles(
            const std::vector<std::string>& project_ids);

        static std::unordered_map<std::string, ProjectSideInfo> fetchProjectSideInfo(
            const std::vector<std::string>& project_ids);

        static bool hasCompatibleProjectVersion(const std::string& project_id,
                                                const std::string& game_version,
                                                const std::string& loader);

        static bool downloadFile(const std::string& url, const std::filesystem::path& destination,
                                 std::string* error_message = nullptr);

    private:
        static std::optional<std::string> getNullableString(const nlohmann::json& obj,
                                                            const char* key);
        static std::optional<bool> getNullableBool(const nlohmann::json& obj, const char* key);
        static std::optional<int64_t> getNullableInt64(const nlohmann::json& obj, const char* key);
        static ModDependency parseDependency(const nlohmann::json& dep_json);
        static FileInfo parseFileInfo(const nlohmann::json& file_json);
        static ModVersionInfo parseVersion(const nlohmann::json& version_json);
        static std::unordered_map<std::string, ModVersionInfo> parseHashToVersionMap(
            const std::string& response_body);
        static std::pair<httplib::Headers, std::string> buildVersionLookupRequest(
            const std::vector<std::string>& file_hashes);
        static std::pair<httplib::Headers, std::string> buildUpdateRequest(
            const std::vector<std::string>& file_hashes, const std::string& game_version,
            const std::string& loader);
        static std::unordered_map<std::string, ModVersionInfo> postVersionMapRequest(
            const std::string& endpoint_path, const httplib::Headers& headers,
            const std::string& body, size_t* response_bytes = nullptr);
        static bool configureClientCertificates(httplib::SSLClient& client,
                                                std::string* warning_message = nullptr);
        static std::pair<std::string, std::string> splitUrlHostAndPath(const std::string& url,
                                                                       int* port, bool* is_https);
        static std::pair<httplib::Headers, std::string> buildCommonHeadersAndBody(
            const std::vector<std::string>& file_hashes);
};
#endif