#include "UpdateEngine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openssl/sha.h>
#include <iostream>

#include <nlohmann/json.hpp>

#include "CurseForgeAPI.h"
#include "ModrinthAPI.h"
#include "UpdateState.h"

namespace modrinth_cli::engine {
namespace {
constexpr char kHashAlgorithm[] = "sha512";
constexpr char kRequiredDependencyType[] = "required";

std::string formatBytes(size_t bytes) {
    constexpr double kKilobyte = 1000.0;
    constexpr double kMegabyte = 1000.0 * 1000.0;

    std::ostringstream out;
    if (bytes >= static_cast<size_t>(kMegabyte)) {
        out << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / kMegabyte)
            << " MB";
        return out.str();
    }

    if (bytes >= static_cast<size_t>(kKilobyte)) {
        out << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / kKilobyte)
            << " kB";
        return out.str();
    }

    out << bytes << " B";
    return out.str();
}

std::string nowUtcIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &now_time);
#else
    gmtime_r(&now_time, &utc_tm);
#endif

    std::ostringstream out;
    out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

const FileInfo* selectPreferredFile(const ModVersionInfo& version) {
    const std::vector<FileInfo>& files = version.getFiles();
    if (files.empty()) {
        return nullptr;
    }

    const auto primary_it = std::find_if(files.begin(), files.end(),
                                         [](const FileInfo& file) { return file.isPrimary(); });
    if (primary_it != files.end()) {
        return &(*primary_it);
    }

    return &files.front();
}

std::optional<std::string> getHashForAlgorithm(const FileInfo& file_info,
                                               const std::string& algorithm) {
    const auto& hashes = file_info.getHashes();
    const auto hash_it = hashes.find(algorithm);
    if (hash_it == hashes.end()) {
        return std::nullopt;
    }

    return hash_it->second;
}

bool dependencyRequiresSpecificVersion(const ModDependency& dependency) {
    return dependency.getVersionID() && !dependency.getVersionID()->empty();
}

bool dependencyIsSatisfiedByVersion(const ModDependency& dependency,
                                    const ModVersionInfo& version) {
    if (!dependency.getProjectID()) {
        return false;
    }

    if (version.getProjectId() != *dependency.getProjectID()) {
        return false;
    }

    if (!dependencyRequiresSpecificVersion(dependency)) {
        return true;
    }

    return version.getId() == *dependency.getVersionID();
}

std::string computeFilesHash(const std::vector<files::FileHashRecord>& files) {
    if (files.empty()) {
        return "empty";
    }

    std::vector<std::string> sorted_hashes;
    sorted_hashes.reserve(files.size());
    for (const auto& file : files) {
        sorted_hashes.push_back(file.hash);
    }
    std::sort(sorted_hashes.begin(), sorted_hashes.end());

    std::ostringstream combined;
    for (const auto& hash : sorted_hashes) {
        combined << hash;
    }

    std::string combined_str = combined.str();
    unsigned char hash_result[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char*>(combined_str.data()), combined_str.length(),
           hash_result);

    std::ostringstream hash_hex;
    for (int i = 0; i < SHA512_DIGEST_LENGTH; ++i) {
        hash_hex << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<int>(hash_result[i]);
    }
    return hash_hex.str();
}

bool tryLoadCachedComputation(const std::filesystem::path& mods_path,
                              const std::string& game_version, const std::string& loader,
                              const std::string& files_hash,
                              UpdateComputation& cached_computation) {
    const auto cache_file = mods_path.parent_path() / ".modrinth-cli-update-cache.json";
    if (!std::filesystem::exists(cache_file)) {
        return false;
    }

    try {
        std::ifstream input(cache_file);
        if (!input) {
            return false;
        }

        const nlohmann::json root = nlohmann::json::parse(input);
        if (!root.contains("files_hash") || !root.contains("game_version") ||
            !root.contains("loader") || !root.contains("computation")) {
            return false;
        }

        if (root.at("files_hash").get<std::string>() != files_hash ||
            root.at("game_version").get<std::string>() != game_version ||
            root.at("loader").get<std::string>() != loader) {
            return false;
        }

        const auto& comp_json = root.at("computation");
        const nlohmann::json state_json = comp_json.at("state");
        cached_computation.state = state::updateStateFromJson(state_json);

        if (comp_json.contains("scanned_file_count")) {
            cached_computation.scanned_file_count =
                comp_json.at("scanned_file_count").get<size_t>();
        }
        if (comp_json.contains("current_response_bytes")) {
            cached_computation.current_response_bytes =
                comp_json.at("current_response_bytes").get<size_t>();
        }
        if (comp_json.contains("latest_response_bytes")) {
            cached_computation.latest_response_bytes =
                comp_json.at("latest_response_bytes").get<size_t>();
        }
        if (comp_json.contains("curseforge_response_bytes")) {
            cached_computation.curseforge_response_bytes =
                comp_json.at("curseforge_response_bytes").get<size_t>();
        }
        if (!comp_json.contains("curseforge_unresolved_count") ||
            !comp_json.contains("curseforge_attempted")) {
            return false;
        }

        cached_computation.curseforge_unresolved_count =
            comp_json.at("curseforge_unresolved_count").get<size_t>();
        cached_computation.curseforge_attempted = comp_json.at("curseforge_attempted").get<bool>();

        return true;
    } catch (...) {
        return false;
    }
}

void saveCacheComputation(const std::filesystem::path& mods_path, const std::string& game_version,
                          const std::string& loader, const std::string& files_hash,
                          const UpdateComputation& computation) {
    const auto cache_file = mods_path.parent_path() / ".modrinth-cli-update-cache.json";

    try {
        nlohmann::json comp_json;
        comp_json["state"] = state::updateStateToJson(computation.state);
        comp_json["scanned_file_count"] = computation.scanned_file_count;
        comp_json["current_response_bytes"] = computation.current_response_bytes;
        comp_json["latest_response_bytes"] = computation.latest_response_bytes;
        comp_json["curseforge_response_bytes"] = computation.curseforge_response_bytes;
        comp_json["curseforge_unresolved_count"] = computation.curseforge_unresolved_count;
        comp_json["curseforge_attempted"] = computation.curseforge_attempted;

        nlohmann::json root;
        root["files_hash"] = files_hash;
        root["game_version"] = game_version;
        root["loader"] = loader;
        root["computation"] = comp_json;
        root["cached_at"] = nowUtcIso8601();

        std::filesystem::create_directories(cache_file.parent_path());
        std::ofstream output(cache_file, std::ios::out | std::ios::trunc);
        if (!output) {
            return;
        }

        output << root.dump(2) << '\n';
    } catch (...) {}
}

}  // namespace

UpdateComputation UpdateEngine::computeUpdateState(const cli::CommandLineOptions& options,
                                                   const std::filesystem::path& state_file) {
    (void)state_file;

    UpdateComputation computation;
    const auto start_time = std::chrono::steady_clock::now();
    computation.state.generated_at = nowUtcIso8601();
    computation.state.mods_path = options.mods_path.string();
    computation.state.game_version = options.game_version;
    computation.state.loader = options.loader;

    std::vector<files::FileHashRecord> files =
        files::FileScanner::collectModFileHashes(options.mods_path);
    computation.scanned_files = files;
    computation.scanned_file_count = files.size();

    std::string files_hash = computeFilesHash(files);

    UpdateComputation cached_computation;
    if (tryLoadCachedComputation(options.mods_path, options.game_version, options.loader,
                                 files_hash, cached_computation)) {
        std::cout << "Hit:1 https://api.modrinth.com\n";
        if (cached_computation.curseforge_unresolved_count > 0) {
            std::cout << "Hit:2 https://api.curseforge.com ["
                      << formatBytes(cached_computation.curseforge_response_bytes) << "]";
            std::cout << "[" << cached_computation.curseforge_unresolved_count
                      << " unresolved mods]";
            std::cout << '\n';
        }
        std::cout.flush();
        return cached_computation;
    }

    if (files.empty()) {
        return computation;
    }

    std::vector<std::string> hashes;
    hashes.reserve(files.size());
    for (const files::FileHashRecord& file : files) {
        hashes.push_back(file.hash);
    }

    auto current_versions =
        ModrinthAPI::fetchCurrentVersions(hashes, &computation.current_response_bytes);
    const auto latest_versions = ModrinthAPI::fetchLatestUpdates(
        hashes, options.game_version, options.loader, &computation.latest_response_bytes);

    std::cout << "Get:1 https://api.modrinth.com ["
              << formatBytes(computation.current_response_bytes + computation.latest_response_bytes)
              << "]\n";
    std::cout.flush();

    std::vector<files::FileHashRecord> unresolved_files;
    unresolved_files.reserve(files.size());
    for (const files::FileHashRecord& file : files) {
        if (!current_versions.contains(file.hash)) {
            unresolved_files.push_back(file);
        }
    }

    computation.curseforge_attempted = !unresolved_files.empty();
    computation.curseforge_unresolved_count = unresolved_files.size();

    const auto curseforge_api_key = CurseForgeAPI::getApiKey();
    if (!unresolved_files.empty()) {
        if (curseforge_api_key) {
            for (const files::FileHashRecord& file : unresolved_files) {
                const auto curseforge_version = CurseForgeAPI::searchModByFilename(
                    file.path.filename().string(), &computation.curseforge_response_bytes);
                if (curseforge_version) {
                    current_versions.insert_or_assign(file.hash, *curseforge_version);
                }
            }
        }

        std::cout << "Get:2 https://api.curseforge.com ["
                  << formatBytes(computation.curseforge_response_bytes) << "]["
                  << unresolved_files.size() << " unresolved mods]\n";
        std::cout.flush();
    }

    computation.current_version_count = current_versions.size();
    computation.latest_version_count = latest_versions.size();

    for (const files::FileHashRecord& file : files) {
        const auto current_it = current_versions.find(file.hash);
        if (current_it == current_versions.end()) {
            continue;
        }
        computation.resolved_current_hashes.insert(file.hash);

        const std::string project_id = current_it->second.getProjectId();
        computation.project_local_filenames.emplace(project_id, file.path.filename().string());
        computation.installed_source_paths_by_project.emplace(project_id, file.path.string());
        computation.installed_hashes_by_project.emplace(project_id, file.hash);
        computation.installed_versions_by_project.insert_or_assign(project_id, current_it->second);
        computation.installed_version_ids_by_project.emplace(project_id,
                                                             current_it->second.getId());

        const ModVersionInfo* target_version = &current_it->second;
        computation.installed_project_ids.insert(project_id);

        const auto latest_it = latest_versions.find(file.hash);
        if (latest_it != latest_versions.end()) {
            computation.resolved_latest_hashes.insert(file.hash);
            const ModVersionInfo& latest_version = latest_it->second;
            const FileInfo* target_file = selectPreferredFile(latest_version);

            if (target_file != nullptr) {
                const std::optional<std::string> target_hash =
                    getHashForAlgorithm(*target_file, kHashAlgorithm);

                if (target_hash && *target_hash != file.hash) {
                    state::UpdatePlanItem item;
                    item.source_path = file.path.string();
                    item.current_hash = file.hash;
                    item.project_id = latest_version.getProjectId();
                    item.current_version_id = current_it->second.getId();
                    item.current_version_number = current_it->second.getVersionNumber();
                    item.target_version_id = latest_version.getId();
                    item.target_version_number = latest_version.getVersionNumber();
                    item.download_url = target_file->getUrl();
                    item.download_filename = target_file->getFilename();
                    item.download_sha512 = target_hash.value_or("");
                    item.download_size = target_file->getSize();

                    computation.state.updates.push_back(std::move(item));
                    computation.updatable_project_ids.insert(latest_version.getProjectId());
                    target_version = &latest_version;
                }
            }
        }

        computation.target_versions_by_project.insert_or_assign(target_version->getProjectId(),
                                                                *target_version);
    }

    std::sort(computation.state.updates.begin(), computation.state.updates.end(),
              [](const state::UpdatePlanItem& left, const state::UpdatePlanItem& right) {
                  return left.source_path < right.source_path;
              });
    computation.state.updates.erase(
        std::unique(computation.state.updates.begin(), computation.state.updates.end(),
                    [](const state::UpdatePlanItem& left, const state::UpdatePlanItem& right) {
                        return left.source_path == right.source_path;
                    }),
        computation.state.updates.end());

    computation.state.updatable_projects.assign(computation.updatable_project_ids.begin(),
                                                computation.updatable_project_ids.end());
    std::sort(computation.state.updatable_projects.begin(),
              computation.state.updatable_projects.end());

    std::set<std::pair<std::string, std::string>> seen_missing_dependencies;
    std::unordered_map<std::string, bool> compatibility_cache;
    for (const auto& [project_id, version] : computation.target_versions_by_project) {
        for (const ModDependency& dependency : version.getDependencies()) {
            if (!dependency.getProjectID()) {
                continue;
            }

            if (!dependency.isRequired()) {
                continue;
            }

            const std::string dependency_project_id = *dependency.getProjectID();
            bool dependency_satisfied = false;

            const auto installed_version_it =
                computation.installed_version_ids_by_project.find(dependency_project_id);
            if (installed_version_it != computation.installed_version_ids_by_project.end()) {
                if (!dependencyRequiresSpecificVersion(dependency)) {
                    dependency_satisfied = true;
                } else {
                    dependency_satisfied =
                        installed_version_it->second == *dependency.getVersionID();
                }
            }

            if (!dependency_satisfied) {
                const auto target_version_it =
                    computation.target_versions_by_project.find(dependency_project_id);
                if (target_version_it != computation.target_versions_by_project.end()) {
                    dependency_satisfied =
                        dependencyIsSatisfiedByVersion(dependency, target_version_it->second);
                }
            }

            if (dependency_satisfied) {
                continue;
            }

            bool is_compatible = true;
            const auto compatibility_it = compatibility_cache.find(dependency_project_id);
            if (compatibility_it != compatibility_cache.end()) {
                is_compatible = compatibility_it->second;
            } else {
                is_compatible = ModrinthAPI::hasCompatibleProjectVersion(
                    dependency_project_id, options.game_version, options.loader);
                compatibility_cache.emplace(dependency_project_id, is_compatible);
            }

            if (!is_compatible) {
                continue;
            }

            if (seen_missing_dependencies.insert({project_id, dependency_project_id}).second) {
                computation.state.missing_required_dependencies.push_back(
                    {project_id, dependency_project_id, kRequiredDependencyType,
                     dependency.getVersionID()});
            }
        }
    }

    std::sort(computation.state.missing_required_dependencies.begin(),
              computation.state.missing_required_dependencies.end(),
              [](const state::DependencyIssue& left, const state::DependencyIssue& right) {
                  if (left.requiring_project_id != right.requiring_project_id) {
                      return left.requiring_project_id < right.requiring_project_id;
                  }
                  return left.missing_project_id < right.missing_project_id;
              });

    std::set<std::string> project_ids_for_titles;
    for (const auto& [project_id, version] : computation.target_versions_by_project) {
        (void)version;
        project_ids_for_titles.insert(project_id);
    }

    for (const auto& [project_id, version] : computation.target_versions_by_project) {
        (void)project_id;
        for (const ModDependency& dependency : version.getDependencies()) {
            if (dependency.getProjectID()) {
                project_ids_for_titles.insert(*dependency.getProjectID());
            }
        }
    }

    for (const state::DependencyIssue& issue : computation.state.missing_required_dependencies) {
        project_ids_for_titles.insert(issue.requiring_project_id);
        project_ids_for_titles.insert(issue.missing_project_id);
    }

    computation.project_titles = ModrinthAPI::fetchProjectTitles(
        std::vector<std::string>(project_ids_for_titles.begin(), project_ids_for_titles.end()));

    computation.unresolved_mod_filenames.clear();
    for (const files::FileHashRecord& file : files) {
        if (!current_versions.contains(file.hash)) {
            computation.unresolved_mod_filenames.push_back(file.path.filename().string());
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const size_t total_response_bytes = computation.current_response_bytes +
                                        computation.latest_response_bytes +
                                        computation.curseforge_response_bytes;
    const auto elapsed_millis = std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    const size_t bytes_per_second =
        (total_response_bytes * 1000ULL) / static_cast<size_t>(elapsed_millis);
    const auto whole_seconds =
        std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());

    std::cout << "Fetched " << formatBytes(total_response_bytes) << " in " << whole_seconds << "s ("
              << formatBytes(bytes_per_second) << "/s)\n";
    std::cout << "Reading mod lists... Done\n";
    std::cout << "Building dependency tree... Done\n";
    std::cout << "Reading state information... Done\n";

    saveCacheComputation(options.mods_path, options.game_version, options.loader, files_hash,
                         computation);

    return computation;
}

}  // namespace modrinth_cli::engine
