#include "CurseForgeAPI.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>

#include "httplib.h"

namespace {
constexpr int HTTPS_PORT = 443;
constexpr int HTTP_STATUS_OK = 200;
constexpr int HTTP_STATUS_NOT_FOUND = 404;
constexpr std::array<char, sizeof("api.curseforge.com")> API_HOST{"api.curseforge.com"};
constexpr std::array<char, sizeof("Hum-Bao/modrinth-updater-cli")> USER_AGENT{
    "Hum-Bao/modrinth-updater-cli"};
constexpr std::array<const char*, 3> kCaBundleCandidates = {"/etc/ssl/certs/ca-certificates.crt",
                                                            "/etc/pki/tls/certs/ca-bundle.crt",
                                                            "/etc/ssl/ca-bundle.pem"};
bool g_warned_curseforge_forbidden = false;
bool g_warned_curseforge_rate_limited = false;

std::filesystem::path getConfigDir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        home = ".";
    }
    return std::filesystem::path(home) / ".modrinth-cli";
}

std::filesystem::path getApiKeyFile() {
    return getConfigDir() / "api-key";
}

std::string toLowerCopy(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return input;
}

std::string normalizeForMatch(std::string input) {
    input = toLowerCopy(std::move(input));
    input.erase(std::remove_if(input.begin(), input.end(),
                               [](unsigned char ch) { return !std::isalnum(ch); }),
                input.end());
    return input;
}

std::string urlEncode(const std::string& input) {
    constexpr std::array<char, sizeof("0123456789ABCDEF")> hex_digits{"0123456789ABCDEF"};

    std::string encoded;
    encoded.reserve(input.size() * 3);
    for (unsigned char ch : input) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex_digits[(ch >> 4) & 0x0F]);
            encoded.push_back(hex_digits[ch & 0x0F]);
        }
    }

    return encoded;
}

std::optional<std::filesystem::path> resolveCaBundlePath() {
    const char* env_path = std::getenv("SSL_CERT_FILE");
    if (env_path != nullptr && *env_path != '\0') {
        std::filesystem::path candidate(env_path);
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    for (const char* candidate : kCaBundleCandidates) {
        if (std::filesystem::exists(candidate)) {
            return std::filesystem::path(candidate);
        }
    }

    return std::nullopt;
}

int scoreSearchCandidate(const std::string& normalized_query, const std::string& candidate_name,
                         const std::string& candidate_slug) {
    const std::string normalized_name = normalizeForMatch(candidate_name);
    const std::string normalized_slug = normalizeForMatch(candidate_slug);

    if (normalized_name == normalized_query || normalized_slug == normalized_query) {
        return 100;
    }

    if (normalized_name.rfind(normalized_query, 0) == 0 ||
        normalized_slug.rfind(normalized_query, 0) == 0) {
        return 80;
    }

    if (normalized_name.find(normalized_query) != std::string::npos ||
        normalized_slug.find(normalized_query) != std::string::npos) {
        return 60;
    }

    return 0;
}

std::string resolveHashAlgorithmName(const nlohmann::json& algo_json) {
    if (algo_json.is_string()) {
        return toLowerCopy(algo_json.get<std::string>());
    }

    if (!algo_json.is_number_integer()) {
        return "";
    }

    const int algo = algo_json.get<int>();
    switch (algo) {
        case 1:
            return "sha1";
        case 2:
            return "md5";
        case 3:
            return "sha256";
        case 4:
            return "sha512";
        default:
            return std::to_string(algo);
    }
}

std::optional<std::string> sanitizeApiKeyValue(const char* raw_value) {
    if (raw_value == nullptr) {
        return std::nullopt;
    }

    std::string value(raw_value);
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    if (start >= end) {
        return std::nullopt;
    }

    value = value.substr(start, end - start);

    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }

    return value.empty() ? std::nullopt : std::optional<std::string>(value);
}

// Extract mod name from filename (remove version numbers and extension)
// e.g., "BetterAdvancements-Forge-1.20.1-0.4.2.10.jar" -> "BetterAdvancements"
std::string extractModName(const std::string& filename) {
    std::string lower_filename = toLowerCopy(filename);

    // Remove extension
    size_t ext_pos = lower_filename.rfind(".jar");
    if (ext_pos != std::string::npos) {
        lower_filename = lower_filename.substr(0, ext_pos);
    }

    // Try to extract the base name before any version-like pattern
    // Pattern: ModName-variant-gameversion-modversion
    // We'll take everything up to the first dash followed by a game version (1.x.x) or variant
    // For simplicity, just take the first component
    size_t dash_pos = lower_filename.find('-');
    if (dash_pos != std::string::npos) {
        return lower_filename.substr(0, dash_pos);
    }

    return lower_filename;
}

// Configure SSL certificates for the HTTP client
void configureClientCertificates(httplib::SSLClient& client, std::string* warning_message) {
    const auto ca_bundle = resolveCaBundlePath();
    if (ca_bundle) {
        client.set_ca_cert_path(ca_bundle->string().c_str());
        return;
    }

    if (warning_message != nullptr) {
        *warning_message = "system CA bundle not found; HTTPS verification failed.";
    }
}

}  // namespace

std::optional<std::string> CurseForgeAPI::getApiKey() {
    // Support common env var names used for CurseForge Core API keys.
    for (const char* env_name : {"CURSEFORGE_API_KEY", "CFCORE_API_KEY", "CURSEFORGE_TOKEN"}) {
        const std::optional<std::string> key = sanitizeApiKeyValue(std::getenv(env_name));
        if (key) {
            return key;
        }
    }

    // Fall back to config file at ~/.modrinth-cli/api-key
    const auto api_key_file = getApiKeyFile();
    if (std::filesystem::exists(api_key_file)) {
        try {
            std::ifstream file(api_key_file);
            std::string key_content;
            if (std::getline(file, key_content)) {
                const auto key = sanitizeApiKeyValue(key_content.c_str());
                if (key) {
                    return key;
                }
            }
        } catch (const std::exception& e) {
            // Silently ignore file read errors
        }
    }

    return std::nullopt;
}

std::optional<ModVersionInfo> CurseForgeAPI::searchModByFilename(const std::string& filename,
                                                                 size_t* response_bytes) {
    const auto api_key = getApiKey();
    if (!api_key) {
        return std::nullopt;
    }

    const std::string mod_name = extractModName(filename);
    if (mod_name.empty()) {
        return std::nullopt;
    }

    return performModSearch(mod_name, *api_key, response_bytes);
}

std::optional<ModVersionInfo> CurseForgeAPI::performModSearch(const std::string& mod_name,
                                                              const std::string& api_key,
                                                              size_t* response_bytes) {
    try {
        httplib::SSLClient client(API_HOST.data(), HTTPS_PORT);
        client.set_follow_location(true);

        // Configure SSL certificates
        std::string warning_message;
        configureClientCertificates(client, &warning_message);
        if (!warning_message.empty()) {
            std::cerr << "Warning: " << warning_message << '\n';
        }

        // Match the documented header shape used by curl examples.
        httplib::Headers headers = {{"User-Agent", USER_AGENT.data()},
                                    {"Accept", "application/json"},
                                    {"x-api-key", api_key}};

        // Search for the mod using the search endpoint.
        // gameId=432 targets Minecraft and classId=6 narrows results to Minecraft mods.
        const std::string search_path =
            "/v1/mods/search?gameId=432&classId=6&searchFilter=" + urlEncode(mod_name) +
            "&pageSize=10&sortField=2&sortOrder=desc";

        const auto res = client.Get(search_path, headers);
        if (response_bytes != nullptr && res) {
            *response_bytes += res->body.size();
        }
        if (!res) {
            std::cerr << "Warning: CurseForge request failed: " << httplib::to_string(res.error())
                      << '\n';
            return std::nullopt;
        }

        if (res->status == HTTP_STATUS_NOT_FOUND) {
            return std::nullopt;
        }

        if (res->status != HTTP_STATUS_OK) {
            if (res->status == 403) {
                if (!g_warned_curseforge_forbidden) {
                    std::cerr << "Warning: CurseForge returned HTTP 403 (forbidden). "
                              << "The API key may be invalid, expired, or missing required "
                                 "permissions.\n";
                    g_warned_curseforge_forbidden = true;
                }
            } else if (res->status == 429) {
                if (!g_warned_curseforge_rate_limited) {
                    std::cerr << "Warning: CurseForge returned HTTP 429 (rate limited). "
                              << "Try again later.\n";
                    g_warned_curseforge_rate_limited = true;
                }
            } else {
                std::cerr << "Warning: CurseForge request failed with HTTP " << res->status << '\n';
            }
            return std::nullopt;
        }

        nlohmann::json response_json = nlohmann::json::parse(res->body);

        // Check if we got results
        if (!response_json.contains("data") || !response_json["data"].is_array() ||
            response_json["data"].empty()) {
            return std::nullopt;
        }

        const std::string normalized_query = normalizeForMatch(mod_name);
        const nlohmann::json* best_result = nullptr;
        int best_score = -1;
        for (const auto& candidate : response_json["data"]) {
            if (!candidate.is_object()) {
                continue;
            }

            const std::string candidate_name =
                candidate.contains("name") && candidate["name"].is_string()
                    ? candidate["name"].get<std::string>()
                    : "";
            const std::string candidate_slug =
                candidate.contains("slug") && candidate["slug"].is_string()
                    ? candidate["slug"].get<std::string>()
                    : "";
            const int score =
                scoreSearchCandidate(normalized_query, candidate_name, candidate_slug);
            if (score > best_score) {
                best_score = score;
                best_result = &candidate;
            }
        }

        if (best_result == nullptr) {
            return std::nullopt;
        }

        const nlohmann::json& first_result = *best_result;

        // Extract project info
        if (!first_result.contains("id") || !first_result.contains("latestFilesIndexes") ||
            first_result["latestFilesIndexes"].empty()) {
            return std::nullopt;
        }

        const int64_t project_id = first_result["id"].get<int64_t>();
        const std::string project_id_str = std::to_string(project_id);
        const std::string project_name =
            first_result.contains("name") && first_result["name"].is_string()
                ? first_result["name"].get<std::string>()
                : project_id_str;

        // Get the latest file index
        const nlohmann::json& latest_index = first_result["latestFilesIndexes"][0];
        const int64_t version_id =
            latest_index.contains("fileId") ? latest_index["fileId"].get<int64_t>() : 0;

        if (version_id == 0) {
            return std::nullopt;
        }

        const std::string version_id_str = std::to_string(version_id);

        // Fetch the actual file details
        const std::string file_path = "/v1/mods/" + project_id_str + "/files/" + version_id_str;
        const auto file_res = client.Get(file_path, headers);
        if (response_bytes != nullptr && file_res) {
            *response_bytes += file_res->body.size();
        }

        if (!file_res || file_res->status != HTTP_STATUS_OK) {
            return std::nullopt;
        }

        nlohmann::json file_json = nlohmann::json::parse(file_res->body);
        if (!file_json.contains("data") || !file_json["data"].is_object()) {
            return std::nullopt;
        }

        const nlohmann::json& file_data = file_json["data"];

        // Extract file information
        std::vector<FileInfo> files;
        if (file_data.contains("fileName") && file_data.contains("downloadUrl")) {
            FileInfo file(file_data["downloadUrl"].get<std::string>(),
                          file_data["fileName"].get<std::string>(), true,
                          file_data.contains("fileLength")
                              ? std::optional<int64_t>(file_data["fileLength"].get<int64_t>())
                              : std::nullopt,
                          std::nullopt);

            // Add hashes if available
            if (file_data.contains("hashes") && file_data["hashes"].is_array()) {
                for (const auto& hash_obj : file_data["hashes"]) {
                    if (hash_obj.contains("value") && hash_obj.contains("algo")) {
                        const std::string algo = resolveHashAlgorithmName(hash_obj["algo"]);
                        if (!algo.empty()) {
                            file.addHash(algo, hash_obj["value"].get<std::string>());
                        }
                    }
                }
            }

            files.push_back(file);
        }

        if (files.empty()) {
            return std::nullopt;
        }

        // Extract version information
        const std::string version_number = file_data.contains("displayName")
                                               ? file_data["displayName"].get<std::string>()
                                               : version_id_str;
        const std::string date_published =
            file_data.contains("fileDate") ? file_data["fileDate"].get<std::string>() : "";
        const int64_t downloads =
            file_data.contains("downloadCount") ? file_data["downloadCount"].get<int64_t>() : 0;

        // Build game versions list
        std::vector<std::string> game_versions;
        if (file_data.contains("gameVersions") && file_data["gameVersions"].is_array()) {
            game_versions = file_data["gameVersions"].get<std::vector<std::string>>();
        }

        // Build loaders list
        std::vector<std::string> loaders;
        if (file_data.contains("sortableGameVersions") &&
            file_data["sortableGameVersions"].is_array()) {
            for (const auto& sgv : file_data["sortableGameVersions"]) {
                if (sgv.contains("gameVersionName")) {
                    loaders.push_back(sgv["gameVersionName"].get<std::string>());
                }
            }
        }

        // Create and return ModVersionInfo object
        // Note: Mark source as CurseForge via project_id prefix for tracking
        ModVersionInfo version(
            std::optional<std::string>(project_name),    // name
            std::optional<std::string>(version_number),  // version_number
            std::nullopt,                                // changelog
            {},             // dependencies (empty, as CurseForge doesn't expose in this context)
            game_versions,  // game_versions
            std::nullopt,   // version_type
            loaders,        // loaders
            std::nullopt,   // featured
            std::optional<std::string>("listed"),  // status
            std::nullopt,                          // requested_status
            version_id_str,                        // id
            "curseforge-" + project_id_str,        // project_id (prefixed to distinguish source)
            "",                                    // author_id
            date_published,                        // date_published
            downloads,                             // downloads
            std::nullopt,                          // changelog_url
            files                                  // files
        );

        return version;
    } catch (const std::exception& ex) {
        std::cerr << "Warning: CurseForge search failed: " << ex.what() << '\n';
        return std::nullopt;
    }
}

bool CurseForgeAPI::saveApiKey(const std::string& api_key) {
    try {
        const auto config_dir = getConfigDir();
        if (!std::filesystem::exists(config_dir)) {
            std::filesystem::create_directories(config_dir);
        }

        const auto api_key_file = getApiKeyFile();
        std::ofstream file(api_key_file);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot write to " << api_key_file << '\n';
            return false;
        }

        file << api_key << '\n';
        file.close();

        // Set restrictive permissions (600 = rw-------)
        std::filesystem::permissions(
            api_key_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);

        return true;
    } catch (const std::exception& ex) {
        std::cerr << "Error: Failed to save API key: " << ex.what() << '\n';
        return false;
    }
}
