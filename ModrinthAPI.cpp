#include "ModrinthAPI.h"
#include "FileScanner.h"
#include "ModDependency.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <unordered_set>

namespace {
constexpr int HTTPS_PORT = 443;
constexpr int HTTP_PORT = 80;
constexpr int HTTP_STATUS_OK = 200;
constexpr size_t PROJECT_LOOKUP_CHUNK_SIZE = 80;
constexpr std::array<char, sizeof("api.modrinth.com")> API_HOST{"api.modrinth.com"};
constexpr std::array<char, sizeof("sha512")> ALGORITHM{"sha512"};
constexpr std::array<char, sizeof("Hum-Bao/modrinth-updater-cli")> USER_AGENT{
    "Hum-Bao/modrinth-updater-cli"};

constexpr std::array<const char*, 3> kCaBundleCandidates = {"/etc/ssl/certs/ca-certificates.crt",
                                                            "/etc/pki/tls/certs/ca-bundle.crt",
                                                            "/etc/ssl/ca-bundle.pem"};

bool setError(std::string* error_message, const std::string& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
    return false;
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
}  // namespace

std::unordered_map<std::string, ModVersionInfo> ModrinthAPI::fetchCurrentVersions(
    const std::vector<std::string>& file_hashes, size_t* response_bytes,
    bool enable_curseforge_fallback) {
    const auto [headers, body] = buildVersionLookupRequest(file_hashes);
    auto result = postVersionMapRequest("/v2/version_files", headers, body, response_bytes);

    // If CurseForge fallback is enabled and we got results, we're done
    // (We can't do fallback here without filenames; this is just the basic lookup)
    return result;
}

std::unordered_map<std::string, ModVersionInfo> ModrinthAPI::fetchCurrentVersionsWithFallback(
    const std::vector<modrinth_cli::files::FileHashRecord>& file_records, size_t* response_bytes) {
    // First, try Modrinth lookup with all hashes
    std::vector<std::string> hashes;
    hashes.reserve(file_records.size());
    for (const auto& record : file_records) {
        hashes.push_back(record.hash);
    }

    const auto [headers, body] = buildVersionLookupRequest(hashes);
    auto result = postVersionMapRequest("/v2/version_files", headers, body, response_bytes);

    // For any unresolved hashes, attempt CurseForge fallback by filename
    std::unordered_set<std::string> resolved_hashes;
    for (const auto& [hash, version] : result) {
        resolved_hashes.insert(hash);
    }

    for (const auto& record : file_records) {
        if (resolved_hashes.find(record.hash) == resolved_hashes.end()) {
            // Hash not found on Modrinth, try CurseForge
            const std::string filename = record.path.filename().string();
            const auto curseforge_result = CurseForgeAPI::searchModByFilename(filename);

            if (curseforge_result) {
                result.emplace(record.hash, *curseforge_result);
                resolved_hashes.insert(record.hash);
            }
        }
    }

    return result;
}

std::unordered_map<std::string, ModVersionInfo> ModrinthAPI::fetchLatestUpdates(
    const std::vector<std::string>& file_hashes, const std::string& game_version,
    const std::string& loader, size_t* response_bytes) {
    const auto [headers, body] = buildUpdateRequest(file_hashes, game_version, loader);
    return postVersionMapRequest("/v2/version_files/update", headers, body, response_bytes);
}

std::optional<ModVersionInfo> ModrinthAPI::fetchVersionById(const std::string& version_id) {
    if (version_id.empty()) {
        return std::nullopt;
    }

    httplib::SSLClient client(API_HOST.data(), HTTPS_PORT);
    client.set_follow_location(true);

    std::string warning_message;
    configureClientCertificates(client, &warning_message);
    if (!warning_message.empty()) {
        std::cerr << "Warning: " << warning_message << '\n';
    }

    const httplib::Headers headers = {{"User-Agent", USER_AGENT.data()}};
    const auto res = client.Get("/v2/version/" + version_id, headers);
    if (!res || res->status != HTTP_STATUS_OK) {
        return std::nullopt;
    }

    try {
        const nlohmann::json version_json = nlohmann::json::parse(res->body);
        if (!version_json.is_object()) {
            return std::nullopt;
        }

        return parseVersion(version_json);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::unordered_map<std::string, std::string> ModrinthAPI::fetchProjectTitles(
    const std::vector<std::string>& project_ids) {
    std::unordered_map<std::string, std::string> titles;
    if (project_ids.empty()) {
        return titles;
    }

    httplib::SSLClient client(API_HOST.data(), HTTPS_PORT);
    client.set_follow_location(true);

    std::string warning_message;
    configureClientCertificates(client, &warning_message);
    if (!warning_message.empty()) {
        std::cerr << "Warning: " << warning_message << '\n';
    }

    const httplib::Headers headers = {{"User-Agent", USER_AGENT.data()}};

    for (size_t index = 0; index < project_ids.size(); index += PROJECT_LOOKUP_CHUNK_SIZE) {
        const size_t end = std::min(project_ids.size(), index + PROJECT_LOOKUP_CHUNK_SIZE);
        const auto chunk_begin =
            project_ids.begin() + static_cast<std::vector<std::string>::difference_type>(index);
        const auto chunk_end =
            project_ids.begin() + static_cast<std::vector<std::string>::difference_type>(end);
        const std::vector<std::string> chunk(chunk_begin, chunk_end);

        const std::string encoded_ids = urlEncode(nlohmann::json(chunk).dump());
        const std::string path = "/v2/projects?ids=" + encoded_ids;

        const auto res = client.Get(path, headers);
        if (!res || res->status != HTTP_STATUS_OK) {
            continue;
        }

        nlohmann::json projects_json;
        try {
            projects_json = nlohmann::json::parse(res->body);
        } catch (const std::exception&) {
            continue;
        }

        if (!projects_json.is_array()) {
            continue;
        }

        for (const auto& project_json : projects_json) {
            if (!project_json.is_object() || !project_json.contains("id") ||
                !project_json.at("id").is_string()) {
                continue;
            }

            const std::string id = project_json.at("id").get<std::string>();
            std::string title = id;

            if (project_json.contains("title") && project_json.at("title").is_string()) {
                title = project_json.at("title").get<std::string>();
            } else if (project_json.contains("slug") && project_json.at("slug").is_string()) {
                title = project_json.at("slug").get<std::string>();
            }

            titles.insert_or_assign(id, std::move(title));
        }
    }

    return titles;
}

std::unordered_map<std::string, ProjectSideInfo> ModrinthAPI::fetchProjectSideInfo(
    const std::vector<std::string>& project_ids) {
    std::unordered_map<std::string, ProjectSideInfo> side_info;
    if (project_ids.empty()) {
        return side_info;
    }

    httplib::SSLClient client(API_HOST.data(), HTTPS_PORT);
    client.set_follow_location(true);

    std::string warning_message;
    configureClientCertificates(client, &warning_message);
    if (!warning_message.empty()) {
        std::cerr << "Warning: " << warning_message << '\n';
    }

    const httplib::Headers headers = {{"User-Agent", USER_AGENT.data()}};

    for (size_t index = 0; index < project_ids.size(); index += PROJECT_LOOKUP_CHUNK_SIZE) {
        const size_t end = std::min(project_ids.size(), index + PROJECT_LOOKUP_CHUNK_SIZE);
        const auto chunk_begin =
            project_ids.begin() + static_cast<std::vector<std::string>::difference_type>(index);
        const auto chunk_end =
            project_ids.begin() + static_cast<std::vector<std::string>::difference_type>(end);
        const std::vector<std::string> chunk(chunk_begin, chunk_end);

        const std::string encoded_ids = urlEncode(nlohmann::json(chunk).dump());
        const std::string path = "/v2/projects?ids=" + encoded_ids;

        const auto res = client.Get(path, headers);
        if (!res || res->status != HTTP_STATUS_OK) {
            continue;
        }

        nlohmann::json projects_json;
        try {
            projects_json = nlohmann::json::parse(res->body);
        } catch (const std::exception&) {
            continue;
        }

        if (!projects_json.is_array()) {
            continue;
        }

        for (const auto& project_json : projects_json) {
            if (!project_json.is_object() || !project_json.contains("id") ||
                !project_json.at("id").is_string()) {
                continue;
            }

            const std::string id = project_json.at("id").get<std::string>();
            ProjectSideInfo info;
            info.client_side =
                project_json.contains("client_side") && project_json.at("client_side").is_string()
                    ? project_json.at("client_side").get<std::string>()
                    : "unknown";
            info.server_side =
                project_json.contains("server_side") && project_json.at("server_side").is_string()
                    ? project_json.at("server_side").get<std::string>()
                    : "unknown";

            side_info.insert_or_assign(id, std::move(info));
        }
    }

    return side_info;
}

bool ModrinthAPI::hasCompatibleProjectVersion(const std::string& project_id,
                                              const std::string& game_version,
                                              const std::string& loader) {
    if (project_id.empty() || game_version.empty() || loader.empty()) {
        return false;
    }

    httplib::SSLClient client(API_HOST.data(), HTTPS_PORT);
    client.set_follow_location(true);

    std::string warning_message;
    configureClientCertificates(client, &warning_message);
    if (!warning_message.empty()) {
        std::cerr << "Warning: " << warning_message << '\n';
    }

    const httplib::Headers headers = {{"User-Agent", USER_AGENT.data()}};

    nlohmann::json loaders_json = nlohmann::json::array({loader});
    nlohmann::json game_versions_json = nlohmann::json::array({game_version});

    const std::string path = "/v2/project/" + project_id +
                             "/version?loaders=" + urlEncode(loaders_json.dump()) +
                             "&game_versions=" + urlEncode(game_versions_json.dump());

    const auto res = client.Get(path, headers);
    if (!res || res->status != HTTP_STATUS_OK) {
        return true;
    }

    try {
        const nlohmann::json versions_json = nlohmann::json::parse(res->body);
        return versions_json.is_array() && !versions_json.empty();
    } catch (const std::exception&) {
        return true;
    }
}

bool ModrinthAPI::downloadFile(const std::string& url, const std::filesystem::path& destination,
                               std::string* error_message) {
    try {
        int port = 0;
        bool is_https = false;
        const auto [host, path] = splitUrlHostAndPath(url, &port, &is_https);

        const auto handle_response = [&](const httplib::Result& res) {
            if (!res) {
                return setError(error_message, "Failed to download " + url + ": " +
                                                   httplib::to_string(res.error()));
            }

            if (res->status != HTTP_STATUS_OK) {
                return setError(error_message, "Failed to download " + url + ": HTTP " +
                                                   std::to_string(res->status));
            }

            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            if (!output) {
                return setError(error_message,
                                "Failed to open destination file: " + destination.string());
            }

            output.write(res->body.data(), static_cast<std::streamsize>(res->body.size()));
            if (!output) {
                return setError(error_message,
                                "Failed to write destination file: " + destination.string());
            }

            return true;
        };

        if (is_https) {
            httplib::SSLClient client(host, port);
            client.set_follow_location(true);

            std::string tls_warning;
            configureClientCertificates(client, &tls_warning);
            if (!tls_warning.empty()) {
                std::cerr << "Warning: " << tls_warning << '\n';
            }

            const auto res = client.Get(path, httplib::Headers{{"User-Agent", USER_AGENT.data()}});
            return handle_response(res);
        }

        httplib::Client client(host, port);
        client.set_follow_location(true);
        const auto res = client.Get(path, httplib::Headers{{"User-Agent", USER_AGENT.data()}});
        return handle_response(res);
    } catch (const std::exception& ex) {
        return setError(error_message, ex.what());
    }
}

std::optional<std::string> ModrinthAPI::getNullableString(const nlohmann::json& obj,
                                                          const char* key) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return std::nullopt;
    }
    return obj.at(key).get<std::string>();
}

std::optional<bool> ModrinthAPI::getNullableBool(const nlohmann::json& obj, const char* key) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return std::nullopt;
    }
    return obj.at(key).get<bool>();
}

std::optional<int64_t> ModrinthAPI::getNullableInt64(const nlohmann::json& obj, const char* key) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return std::nullopt;
    }
    return obj.at(key).get<int64_t>();
}

ModDependency ModrinthAPI::parseDependency(const nlohmann::json& dep_json) {
    return ModDependency(
        getNullableString(dep_json, "version_id"), getNullableString(dep_json, "project_id"),
        getNullableString(dep_json, "file_name"), getNullableString(dep_json, "dependency_type"));
}

FileInfo ModrinthAPI::parseFileInfo(const nlohmann::json& file_json) {
    FileInfo file(file_json.at("url").get<std::string>(),
                  file_json.at("filename").get<std::string>(), file_json.at("primary").get<bool>(),
                  getNullableInt64(file_json, "size"), getNullableString(file_json, "file_type"));

    if (file_json.contains("hashes") && file_json.at("hashes").is_object()) {
        for (auto it = file_json.at("hashes").begin(); it != file_json.at("hashes").end(); ++it) {
            if (!it.value().is_null()) {
                file.addHash(it.key(), it.value().get<std::string>());
            }
        }
    }

    return file;
}

ModVersionInfo ModrinthAPI::parseVersion(const nlohmann::json& version_json) {
    std::vector<ModDependency> dependencies;
    if (version_json.contains("dependencies") && version_json.at("dependencies").is_array()) {
        for (const auto& dep_json : version_json.at("dependencies")) {
            dependencies.push_back(parseDependency(dep_json));
        }
    }

    std::vector<std::string> game_versions;
    if (version_json.contains("game_versions") && version_json.at("game_versions").is_array()) {
        game_versions = version_json.at("game_versions").get<std::vector<std::string>>();
    }

    std::vector<std::string> loaders;
    if (version_json.contains("loaders") && version_json.at("loaders").is_array()) {
        loaders = version_json.at("loaders").get<std::vector<std::string>>();
    }

    std::vector<FileInfo> files;
    if (version_json.contains("files") && version_json.at("files").is_array()) {
        for (const auto& file_json : version_json.at("files")) {
            files.push_back(parseFileInfo(file_json));
        }
    }

    return ModVersionInfo(
        getNullableString(version_json, "name"), getNullableString(version_json, "version_number"),
        getNullableString(version_json, "changelog"), std::move(dependencies),
        std::move(game_versions), getNullableString(version_json, "version_type"),
        std::move(loaders), getNullableBool(version_json, "featured"),
        getNullableString(version_json, "status"),
        getNullableString(version_json, "requested_status"),
        version_json.at("id").get<std::string>(), version_json.at("project_id").get<std::string>(),
        version_json.at("author_id").get<std::string>(),
        version_json.at("date_published").get<std::string>(),
        version_json.at("downloads").get<int64_t>(),
        getNullableString(version_json, "changelog_url"), std::move(files));
}

std::unordered_map<std::string, ModVersionInfo> ModrinthAPI::parseHashToVersionMap(
    const std::string& response_body) {
    const nlohmann::json root = nlohmann::json::parse(response_body);
    if (!root.is_object()) {
        throw std::runtime_error("Expected a JSON object mapping hashes to versions");
    }

    std::unordered_map<std::string, ModVersionInfo> result;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.value().is_object()) {
            result.emplace(it.key(), parseVersion(it.value()));
        }
    }

    return result;
}

std::pair<httplib::Headers, std::string> ModrinthAPI::buildCommonHeadersAndBody(
    const std::vector<std::string>& file_hashes) {
    nlohmann::json body;
    body["hashes"] = file_hashes;
    body["algorithm"] = ALGORITHM.data();

    httplib::Headers headers = {{"User-Agent", USER_AGENT.data()}};
    return {std::move(headers), body.dump()};
}

std::pair<httplib::Headers, std::string> ModrinthAPI::buildVersionLookupRequest(
    const std::vector<std::string>& file_hashes) {
    return buildCommonHeadersAndBody(file_hashes);
}

std::pair<httplib::Headers, std::string> ModrinthAPI::buildUpdateRequest(
    const std::vector<std::string>& file_hashes, const std::string& game_version,
    const std::string& loader) {
    auto [headers, body_string] = buildCommonHeadersAndBody(file_hashes);
    nlohmann::json body = nlohmann::json::parse(body_string);
    body["game_versions"] = {game_version};
    body["loaders"] = {loader};
    return {std::move(headers), body.dump()};
}

std::unordered_map<std::string, ModVersionInfo> ModrinthAPI::postVersionMapRequest(
    const std::string& endpoint_path, const httplib::Headers& headers, const std::string& body,
    size_t* response_bytes) {
    httplib::SSLClient client(API_HOST.data(), HTTPS_PORT);
    client.set_follow_location(true);

    std::string warning_message;
    configureClientCertificates(client, &warning_message);
    if (!warning_message.empty()) {
        std::cerr << "Warning: " << warning_message << '\n';
    }

    const auto res = client.Post(endpoint_path, headers, body, "application/json");
    if (!res) {
        throw std::runtime_error("Modrinth request failed: " + httplib::to_string(res.error()));
    }

    if (res->status != HTTP_STATUS_OK) {
        throw std::runtime_error("Modrinth request failed with HTTP " +
                                 std::to_string(res->status));
    }

    if (response_bytes != nullptr) {
        *response_bytes = res->body.size();
    }

    return parseHashToVersionMap(res->body);
}

bool ModrinthAPI::configureClientCertificates(httplib::SSLClient& client,
                                              std::string* warning_message) {
    const auto ca_bundle = resolveCaBundlePath();
    if (ca_bundle) {
        client.set_ca_cert_path(ca_bundle->string().c_str());
        return true;
    }

    if (warning_message != nullptr) {
        *warning_message = "system CA bundle not found; HTTPS verification failed.";
    }
    return false;
}

std::pair<std::string, std::string> ModrinthAPI::splitUrlHostAndPath(const std::string& url,
                                                                     int* port, bool* is_https) {
    if (port == nullptr || is_https == nullptr) {
        throw std::invalid_argument("port and is_https pointers must not be null");
    }

    const std::regex url_pattern(R"(^(https?)://([^/:]+)(?::(\d+))?(\/.*)?$)");
    std::smatch match;
    if (!std::regex_match(url, match, url_pattern)) {
        throw std::runtime_error("Unsupported URL format: " + url);
    }

    const std::string scheme = match[1].str();
    *is_https = scheme == "https";

    if (match[3].matched) {
        *port = std::stoi(match[3].str());
    } else {
        *port = *is_https ? HTTPS_PORT : HTTP_PORT;
    }

    const std::string host = match[2].str();
    const std::string path = match[4].matched ? match[4].str() : "/";
    return {host, path};
}