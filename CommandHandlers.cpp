#include "CommandHandlers.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "FileScanner.h"
#include "ModrinthAPI.h"
#include "OutputFormatter.h"
#include "UpdateEngine.h"
#include "UpdateState.h"

namespace modrinth_cli::commands {
namespace {
constexpr char kModpackConfigFilename[] = ".modrinth-cli-config.json";
constexpr char kScopedInstallStateFilename[] = ".modrinth-cli-install-scoped.json";

struct ModpackConfig {
        std::string game_version;
        std::string loader;
        std::string initialized_at;
};

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

std::filesystem::path getModpackConfigPath(const std::filesystem::path& mods_path) {
    return std::filesystem::absolute(mods_path) / kModpackConfigFilename;
}

std::string trimCopy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string normalizePackageName(std::string value) {
    value = trimCopy(value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    constexpr size_t jar_suffix_len = 4;
    if (value.size() > jar_suffix_len && value.ends_with(".jar")) {
        value.resize(value.size() - jar_suffix_len);
    }

    return value;
}

bool isSafeFilename(const std::filesystem::path& filename) {
    if (filename.empty()) {
        return false;
    }

    if (filename.has_root_path() || filename.has_parent_path()) {
        return false;
    }

    const auto name = filename.filename().string();
    if (name.empty() || name == "." || name == "..") {
        return false;
    }

    return filename == filename.filename();
}

bool isPathWithin(const std::filesystem::path& base, const std::filesystem::path& candidate) {
    const auto base_abs = std::filesystem::absolute(base).lexically_normal();
    const auto candidate_abs = std::filesystem::absolute(candidate).lexically_normal();

    const auto mismatch =
        std::mismatch(base_abs.begin(), base_abs.end(), candidate_abs.begin(), candidate_abs.end());
    return mismatch.first == base_abs.end();
}

bool matchesOnlyUpgradeName(const std::string& requested_name, const state::UpdatePlanItem& item) {
    const std::string normalized_requested = normalizePackageName(requested_name);
    if (normalized_requested.empty()) {
        return false;
    }

    const std::string source_name = std::filesystem::path(item.source_path).filename().string();
    const std::string normalized_source_name = normalizePackageName(source_name);
    if (normalized_source_name == normalized_requested) {
        return true;
    }

    const std::string normalized_download_name = normalizePackageName(item.download_filename);
    return normalized_download_name == normalized_requested;
}

bool partiallyMatchesOnlyUpgradeName(const std::string& requested_name,
                                     const state::UpdatePlanItem& item) {
    const std::string normalized_requested = normalizePackageName(requested_name);
    if (normalized_requested.empty()) {
        return false;
    }

    const std::string source_name = std::filesystem::path(item.source_path).filename().string();
    const std::string normalized_source_name = normalizePackageName(source_name);
    if (normalized_source_name.find(normalized_requested) != std::string::npos) {
        return true;
    }

    const std::string normalized_download_name = normalizePackageName(item.download_filename);
    return normalized_download_name.find(normalized_requested) != std::string::npos;
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

std::optional<std::string> findInstalledProjectIdForName(
    const engine::UpdateComputation& computation, const std::string& requested_name) {
    const std::string normalized_requested = normalizePackageName(requested_name);
    if (normalized_requested.empty()) {
        return std::nullopt;
    }

    std::vector<std::string> matches;
    for (const auto& [project_id, version] : computation.installed_versions_by_project) {
        (void)version;
        const auto local_name_it = computation.project_local_filenames.find(project_id);
        const std::string local_name =
            local_name_it != computation.project_local_filenames.end() ? local_name_it->second : "";
        const auto title_it = computation.project_titles.find(project_id);
        const std::string title =
            title_it != computation.project_titles.end() ? title_it->second : "";

        const std::string normalized_local = normalizePackageName(local_name);
        const std::string normalized_title = normalizePackageName(title);
        if (normalized_local == normalized_requested || normalized_title == normalized_requested ||
            normalized_local.find(normalized_requested) != std::string::npos ||
            normalized_title.find(normalized_requested) != std::string::npos) {
            matches.push_back(project_id);
        }
    }

    if (matches.size() != 1) {
        return std::nullopt;
    }

    return matches.front();
}

state::UpdatePlanItem buildRepairPlanItem(const engine::UpdateComputation& computation,
                                          const std::string& project_id,
                                          const ModVersionInfo& target_version) {
    const auto source_it = computation.installed_source_paths_by_project.find(project_id);
    const auto hash_it = computation.installed_hashes_by_project.find(project_id);
    const auto current_it = computation.installed_versions_by_project.find(project_id);
    if (source_it == computation.installed_source_paths_by_project.end() ||
        hash_it == computation.installed_hashes_by_project.end() ||
        current_it == computation.installed_versions_by_project.end()) {
        throw cli::CliError("Cannot repair mod because the installed file could not be located: " +
                            project_id);
    }

    const FileInfo* target_file = selectPreferredFile(target_version);
    if (target_file == nullptr) {
        throw cli::CliError("Cannot repair mod because target version has no downloadable file: " +
                            project_id);
    }

    const std::optional<std::string> target_hash = getHashForAlgorithm(*target_file, "sha512");
    if (!target_hash) {
        throw cli::CliError("Cannot repair mod because target version has no sha512 hash: " +
                            project_id);
    }

    state::UpdatePlanItem item;
    item.source_path = source_it->second;
    item.current_hash = hash_it->second;
    item.project_id = project_id;
    item.current_version_id = current_it->second.getId();
    item.current_version_number = current_it->second.getVersionNumber();
    item.target_version_id = target_version.getId();
    item.target_version_number = target_version.getVersionNumber();
    item.download_url = target_file->getUrl();
    item.download_filename = target_file->getFilename();
    item.download_sha512 = *target_hash;
    item.download_size = target_file->getSize();
    return item;
}

void collectRepairTargets(const engine::UpdateComputation& computation,
                          const ModVersionInfo& version, const std::string& project_id,
                          std::unordered_map<std::string, ModVersionInfo>& selected_versions,
                          std::unordered_set<std::string>& seen_version_ids) {
    if (!seen_version_ids.insert(version.getId()).second) {
        return;
    }

    const auto existing_it = selected_versions.find(project_id);
    if (existing_it != selected_versions.end() && existing_it->second.getId() != version.getId()) {
        throw cli::CliError("Conflicting required versions for project: " + project_id);
    }

    selected_versions.insert_or_assign(project_id, version);

    for (const ModDependency& dependency : version.getDependencies()) {
        if (!dependency.getProjectID() || !dependency.isRequired()) {
            continue;
        }

        const std::string dependency_project_id = *dependency.getProjectID();

        if (dependency.getVersionID() && !dependency.getVersionID()->empty()) {
            const auto installed_it =
                computation.installed_versions_by_project.find(dependency_project_id);
            if (installed_it != computation.installed_versions_by_project.end() &&
                installed_it->second.getId() == *dependency.getVersionID()) {
                collectRepairTargets(computation, installed_it->second, dependency_project_id,
                                     selected_versions, seen_version_ids);
                continue;
            }

            const auto exact_version = ModrinthAPI::fetchVersionById(*dependency.getVersionID());
            if (!exact_version) {
                throw cli::CliError(
                    "Unable to fetch required dependency version: " + dependency_project_id + " (" +
                    *dependency.getVersionID() + ")");
            }

            const auto source_it =
                computation.installed_versions_by_project.find(dependency_project_id);
            if (source_it == computation.installed_versions_by_project.end()) {
                throw cli::CliError(
                    "Required dependency version is not installed and cannot be repaired: " +
                    dependency_project_id + " (" + *dependency.getVersionID() + ")");
            }

            collectRepairTargets(computation, *exact_version, dependency_project_id,
                                 selected_versions, seen_version_ids);
            continue;
        }

        const auto installed_it =
            computation.installed_versions_by_project.find(dependency_project_id);
        if (installed_it == computation.installed_versions_by_project.end()) {
            throw cli::CliError("Required dependency is not installed and cannot be repaired: " +
                                dependency_project_id);
        }

        collectRepairTargets(computation, installed_it->second, dependency_project_id,
                             selected_versions, seen_version_ids);
    }
}

bool isDisabledModFilePath(const std::filesystem::path& file_path) {
    const std::string file_name = normalizePackageName(file_path.filename().string());
    return file_name.ends_with(".jar.disabled") || file_name.ends_with(".zip.disabled");
}

std::vector<std::filesystem::path> collectDisabledModFiles(const std::filesystem::path& mods_path) {
    if (!std::filesystem::exists(mods_path)) {
        throw std::runtime_error("Mods path does not exist: " + mods_path.string());
    }

    if (!std::filesystem::is_directory(mods_path)) {
        throw std::runtime_error("Mods path is not a directory: " + mods_path.string());
    }

    std::vector<std::filesystem::path> results;
    for (const auto& entry : std::filesystem::directory_iterator(mods_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::filesystem::path file_path = entry.path();
        if (isDisabledModFilePath(file_path)) {
            results.push_back(std::filesystem::absolute(file_path));
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::vector<std::filesystem::path> collectUnknownModFiles(const std::filesystem::path& mods_path) {
    const std::vector<files::FileHashRecord> files =
        files::FileScanner::collectModFileHashes(mods_path);
    if (files.empty()) {
        return {};
    }

    std::vector<std::string> hashes;
    hashes.reserve(files.size());
    for (const files::FileHashRecord& file : files) {
        hashes.push_back(file.hash);
    }

    const auto current_versions = ModrinthAPI::fetchCurrentVersions(hashes);

    std::vector<std::filesystem::path> unknown_files;
    for (const files::FileHashRecord& file : files) {
        if (!current_versions.contains(file.hash)) {
            unknown_files.push_back(file.path);
        }
    }

    std::sort(unknown_files.begin(), unknown_files.end());
    return unknown_files;
}

bool isSideUsable(const std::string& side_value) {
    return side_value == "required" || side_value == "optional";
}

bool isClientOnlyProject(const ProjectSideInfo& side_info) {
    return isSideUsable(side_info.client_side) && side_info.server_side == "unsupported";
}

bool isServerOnlyProject(const ProjectSideInfo& side_info) {
    return isSideUsable(side_info.server_side) && side_info.client_side == "unsupported";
}

bool isClientServerProject(const ProjectSideInfo& side_info) {
    return isSideUsable(side_info.client_side) && isSideUsable(side_info.server_side);
}

bool isUnknownSideProject(const ProjectSideInfo& side_info) {
    return side_info.client_side == "unknown" || side_info.server_side == "unknown";
}

std::vector<state::UpdatePlanItem> filterUpdatesByEnvironment(
    const std::vector<state::UpdatePlanItem>& updates, bool list_client_only, bool list_server_only,
    bool list_client_server, bool list_side_unknown) {
    if (!list_client_only && !list_server_only && !list_client_server && !list_side_unknown) {
        return updates;
    }

    std::vector<std::string> project_ids;
    project_ids.reserve(updates.size());
    std::unordered_set<std::string> seen_project_ids;
    for (const state::UpdatePlanItem& update : updates) {
        if (seen_project_ids.insert(update.project_id).second) {
            project_ids.push_back(update.project_id);
        }
    }

    const auto side_info_by_project = ModrinthAPI::fetchProjectSideInfo(project_ids);

    std::vector<state::UpdatePlanItem> filtered_updates;
    filtered_updates.reserve(updates.size());
    for (const state::UpdatePlanItem& update : updates) {
        const auto side_it = side_info_by_project.find(update.project_id);
        if (side_it == side_info_by_project.end()) {
            continue;
        }

        const ProjectSideInfo& side_info = side_it->second;
        const bool match_client_only = list_client_only && isClientOnlyProject(side_info);
        const bool match_server_only = list_server_only && isServerOnlyProject(side_info);
        const bool match_client_server = list_client_server && isClientServerProject(side_info);
        const bool match_side_unknown = list_side_unknown && isUnknownSideProject(side_info);
        if (match_client_only || match_server_only || match_client_server || match_side_unknown) {
            filtered_updates.push_back(update);
        }
    }

    return filtered_updates;
}

std::vector<state::UpdatePlanItem> mergeUnselectedAndScopedRemainingUpdates(
    const std::vector<state::UpdatePlanItem>& full_updates,
    const std::unordered_set<std::string>& selected_projects,
    const std::vector<state::UpdatePlanItem>& scoped_remaining_updates) {
    std::vector<state::UpdatePlanItem> merged_updates;
    merged_updates.reserve(full_updates.size());

    for (const state::UpdatePlanItem& update : full_updates) {
        if (!selected_projects.contains(update.project_id)) {
            merged_updates.push_back(update);
        }
    }

    std::unordered_set<std::string> seen_source_paths;
    for (const state::UpdatePlanItem& update : merged_updates) {
        seen_source_paths.insert(update.source_path);
    }

    for (const state::UpdatePlanItem& update : scoped_remaining_updates) {
        if (seen_source_paths.insert(update.source_path).second) {
            merged_updates.push_back(update);
        }
    }

    std::sort(merged_updates.begin(), merged_updates.end(),
              [](const state::UpdatePlanItem& left, const state::UpdatePlanItem& right) {
                  return left.source_path < right.source_path;
              });

    return merged_updates;
}

std::vector<std::string> collectUpdatableProjectsFromUpdates(
    const std::vector<state::UpdatePlanItem>& updates) {
    std::unordered_set<std::string> unique_project_ids;
    unique_project_ids.reserve(updates.size());

    for (const state::UpdatePlanItem& update : updates) {
        unique_project_ids.insert(update.project_id);
    }

    std::vector<std::string> project_ids(unique_project_ids.begin(), unique_project_ids.end());
    std::sort(project_ids.begin(), project_ids.end());
    return project_ids;
}

std::string formatAptSize(int64_t bytes) {
    constexpr int64_t kKilobyte = 1000;
    constexpr int64_t kMegabyte = 1000 * 1000;

    std::ostringstream out;
    if (bytes >= kMegabyte) {
        out << std::fixed << std::setprecision(1)
            << (static_cast<double>(bytes) / static_cast<double>(kMegabyte)) << " MB";
        return out.str();
    }

    if (bytes >= kKilobyte) {
        out << std::fixed << std::setprecision(1)
            << (static_cast<double>(bytes) / static_cast<double>(kKilobyte)) << " kB";
        return out.str();
    }

    out << bytes << " B";
    return out.str();
}

std::vector<std::string> collectUpgradePackageNames(
    const std::vector<state::UpdatePlanItem>& updates) {
    std::vector<std::string> package_names;
    package_names.reserve(updates.size());

    for (const state::UpdatePlanItem& update : updates) {
        package_names.push_back(std::filesystem::path(update.source_path).filename().string());
    }

    std::sort(package_names.begin(), package_names.end());
    package_names.erase(std::unique(package_names.begin(), package_names.end()),
                        package_names.end());
    return package_names;
}

void printUpgradePreview(const std::vector<state::UpdatePlanItem>& updates) {
    const std::vector<std::string> package_names = collectUpgradePackageNames(updates);
    if (package_names.empty()) {
        return;
    }

    std::cout << "The following mods will be upgraded:\n  ";
    for (size_t index = 0; index < package_names.size(); ++index) {
        std::cout << package_names[index];
        if (index + 1 < package_names.size()) {
            std::cout << ' ';
            if ((index + 1) % 8 == 0) {
                std::cout << "\n  ";
            }
        }
    }
    std::cout << "\n";

    std::cout << package_names.size()
              << " upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n";
}

int64_t getCurrentFileSizeBytes(const std::filesystem::path& source_path) {
    std::error_code file_error;
    const auto size = std::filesystem::file_size(source_path, file_error);
    if (file_error) {
        return 0;
    }

    return static_cast<int64_t>(size);
}

void printUpgradeArchiveSummary(const std::vector<state::UpdatePlanItem>& updates) {
    int64_t total_download_bytes = 0;
    int64_t total_disk_delta_bytes = 0;

    for (const state::UpdatePlanItem& update : updates) {
        const int64_t current_size = getCurrentFileSizeBytes(update.source_path);
        const int64_t download_size = update.download_size.value_or(current_size);

        total_download_bytes += std::max<int64_t>(0, download_size);
        total_disk_delta_bytes += download_size - current_size;
    }

    std::cout << "Need to get " << formatAptSize(total_download_bytes) << " of archives.\n";

    if (total_disk_delta_bytes >= 0) {
        std::cout << "After this operation, " << formatAptSize(total_disk_delta_bytes)
                  << " of additional disk space will be used.\n";
    } else {
        std::cout << "After this operation, " << formatAptSize(-total_disk_delta_bytes)
                  << " of disk space will be freed.\n";
    }
}

std::optional<ModpackConfig> tryLoadModpackConfig(const std::filesystem::path& mods_path) {
    const std::filesystem::path config_path = getModpackConfigPath(mods_path);
    if (!std::filesystem::exists(config_path)) {
        return std::nullopt;
    }

    std::ifstream input(config_path);
    if (!input) {
        throw std::runtime_error("Failed to open modpack config file: " + config_path.string());
    }

    const nlohmann::json root = nlohmann::json::parse(input);
    if (!root.contains("game_version") || !root.at("game_version").is_string() ||
        !root.contains("loader") || !root.at("loader").is_string()) {
        throw std::runtime_error("Invalid modpack config format: " + config_path.string());
    }

    ModpackConfig config;
    config.game_version = root.at("game_version").get<std::string>();
    config.loader = root.at("loader").get<std::string>();
    if (root.contains("initialized_at") && root.at("initialized_at").is_string()) {
        config.initialized_at = root.at("initialized_at").get<std::string>();
    }

    return config;
}

void saveModpackConfig(const std::filesystem::path& mods_path, const ModpackConfig& config) {
    const std::filesystem::path config_path = getModpackConfigPath(mods_path);
    std::filesystem::create_directories(config_path.parent_path());

    nlohmann::json root;
    root["initialized_at"] = config.initialized_at;
    root["game_version"] = config.game_version;
    root["loader"] = config.loader;

    std::ofstream output(config_path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open modpack config file for write: " +
                                 config_path.string());
    }

    output << root.dump(2) << '\n';
    if (!output) {
        throw std::runtime_error("Failed to write modpack config file: " + config_path.string());
    }
}

std::string promptForRequiredValue(const std::string& label,
                                   const std::optional<std::string>& default_value = std::nullopt) {
    while (true) {
        std::cout << label;
        if (default_value && !default_value->empty()) {
            std::cout << " [" << *default_value << "]";
        }
        std::cout << ": ";

        std::string input;
        if (!std::getline(std::cin, input)) {
            throw cli::CliError("Input cancelled.");
        }

        input = trimCopy(input);
        if (!input.empty()) {
            return input;
        }

        if (default_value && !default_value->empty()) {
            return *default_value;
        }

        std::cout << "Value is required.\n";
    }
}

bool promptForYesNo(const std::string& question, bool default_yes = true) {
    while (true) {
        std::cout << question << (default_yes ? " [Y/n]: " : " [y/N]: ");

        std::string input;
        if (!std::getline(std::cin, input)) {
            throw cli::CliError("Input cancelled.");
        }

        input = trimCopy(input);
        if (input.empty()) {
            return default_yes;
        }

        for (char& ch : input) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (input == "y" || input == "yes") {
            return true;
        }

        if (input == "n" || input == "no") {
            return false;
        }

        std::cout << "Please answer with y or n.\n";
    }
}

cli::CommandLineOptions resolveCommandOptions(const cli::CommandLineOptions& original_options,
                                              bool require_config) {
    cli::CommandLineOptions options = original_options;

    if (!std::filesystem::exists(options.mods_path)) {
        if (!options.mods_path_provided) {
            throw cli::CliError(
                "No ./mods folder was found next to the CLI. Run the command with a "
                "mods folder path, for example: modrinth-cli update /path/to/mods");
        }

        throw cli::CliError("Mods path does not exist: " + options.mods_path.string());
    }

    const std::optional<ModpackConfig> config = tryLoadModpackConfig(options.mods_path);

    if (require_config && !config) {
        throw cli::CliError("Modpack is not initialized for this mods folder. Run 'modrinth-cli " +
                            std::string("init ") + options.mods_path.string() + "' first.");
    }

    if (!options.game_version_provided) {
        if (config) {
            options.game_version = config->game_version;
        }
    }

    if (!options.loader_provided) {
        if (config) {
            options.loader = config->loader;
        }
    }

    if (options.command == "update" || options.command == "deps" || options.command == "install") {
        if (options.game_version.empty() || options.loader.empty()) {
            throw cli::CliError("Missing game version or loader. Run 'modrinth-cli init " +
                                options.mods_path.string() +
                                "' to configure this folder, or pass --game-version and --loader.");
        }
    }

    return options;
}

int runUpgradeCommand(const cli::CommandLineOptions& options);

int runInitCommand(const cli::CommandLineOptions& options) {
    if (!std::filesystem::exists(options.mods_path)) {
        throw cli::CliError("Mods path does not exist: " + options.mods_path.string());
    }

    if (!std::filesystem::is_directory(options.mods_path)) {
        throw cli::CliError("Mods path is not a directory: " + options.mods_path.string());
    }

    const std::optional<ModpackConfig> existing_config = tryLoadModpackConfig(options.mods_path);

    std::cout << "Initializing modpack settings for: " << options.mods_path << '\n';
    std::cout << "This will save defaults used by update/deps/upgrade in this folder.\n\n";

    const std::string game_version = promptForRequiredValue(
        "Minecraft game version",
        existing_config ? std::optional<std::string>(existing_config->game_version) : std::nullopt);
    const std::string loader = promptForRequiredValue(
        "Mod loader",
        existing_config ? std::optional<std::string>(existing_config->loader) : std::nullopt);

    ModpackConfig config;
    config.initialized_at = nowUtcIso8601();
    config.game_version = game_version;
    config.loader = loader;

    saveModpackConfig(options.mods_path, config);

    std::cout << "\nSaved modpack config: " << getModpackConfigPath(options.mods_path) << '\n';
    std::cout << "  game version: " << config.game_version << '\n';
    std::cout << "  loader: " << config.loader << '\n';
    return 0;
}

int runUpdateCommand(const cli::CommandLineOptions& options) {
    const cli::CommandLineOptions resolved_options = resolveCommandOptions(options, true);
    engine::UpdateComputation computation =
        engine::UpdateEngine::computeUpdateState(resolved_options, resolved_options.state_file);
    state::UpdateStateSerializer::save(resolved_options.state_file, computation.state);
    std::cout << computation.scanned_file_count << " mod files scanned.\n";
    std::cout << computation.state.updates.size() << " mods can be upgraded. Run './modrinth-cli "
              << "list --upgradable " << resolved_options.mods_path.string() << "' to see them.\n";

    if (!computation.state.missing_required_dependencies.empty()) {
        std::cout << computation.state.missing_required_dependencies.size()
                  << " required dependencies are missing. Run './modrinth-cli deps "
                  << resolved_options.mods_path.string() << "' to inspect.\n";
    }

    std::cout << "Update plan written to: " << resolved_options.state_file << '\n';
    return 0;
}

int runListCommand(const cli::CommandLineOptions& options) {
    bool printed_section = false;

    if (options.list_upgradable) {
        state::UpdateState state = state::UpdateStateSerializer::load(options.state_file);
        std::vector<state::UpdatePlanItem> filtered_updates = filterUpdatesByEnvironment(
            state.updates, options.list_client_only, options.list_server_only,
            options.list_client_server, options.list_side_unknown);

        output::OutputFormatter::printUpgradableList(filtered_updates);
        printed_section = true;
    }

    if (options.list_disabled) {
        const std::vector<std::filesystem::path> disabled_files =
            collectDisabledModFiles(options.mods_path);

        if (printed_section) {
            std::cout << '\n';
        }
        std::cout << "Disabled mods (" << disabled_files.size() << "):\n";
        for (const auto& file_path : disabled_files) {
            std::cout << "- " << file_path.filename().string() << '\n';
        }
        printed_section = true;
    }

    if (options.list_unknown) {
        const std::vector<std::filesystem::path> unknown_files =
            collectUnknownModFiles(options.mods_path);

        if (printed_section) {
            std::cout << '\n';
        }
        std::cout << "Unknown mods (not detected by Modrinth) (" << unknown_files.size() << "):\n";
        for (const auto& file_path : unknown_files) {
            std::cout << "- " << file_path.filename().string() << '\n';
        }
    }

    return 0;
}

int runDepsCommand(const cli::CommandLineOptions& options) {
    const cli::CommandLineOptions resolved_options = resolveCommandOptions(options, true);
    engine::UpdateComputation computation =
        engine::UpdateEngine::computeUpdateState(resolved_options, resolved_options.state_file);
    output::OutputFormatter::printDependencyTree(computation);
    output::OutputFormatter::printMissingDependencies(computation);
    output::OutputFormatter::printUpdatableProjects(computation);
    return 0;
}

int runFixBrokenCommand(const cli::CommandLineOptions& options) {
    const cli::CommandLineOptions resolved_options = resolveCommandOptions(options, true);
    engine::UpdateComputation computation =
        engine::UpdateEngine::computeUpdateState(resolved_options, resolved_options.state_file);

    const std::optional<std::string> root_project_id =
        findInstalledProjectIdForName(computation, resolved_options.fix_broken_name);
    if (!root_project_id) {
        throw cli::CliError("No installed mod matched '--fix-broken " +
                            resolved_options.fix_broken_name + "'. Run 'modrinth-cli deps " +
                            resolved_options.mods_path.string() + "' to inspect installed mods.");
    }

    const auto root_version_it = computation.installed_versions_by_project.find(*root_project_id);
    if (root_version_it == computation.installed_versions_by_project.end()) {
        throw cli::CliError(
            "The selected mod could not be repaired because its current version was not found.");
    }

    std::unordered_map<std::string, ModVersionInfo> repair_versions;
    std::unordered_set<std::string> seen_version_ids;
    collectRepairTargets(computation, root_version_it->second, *root_project_id, repair_versions,
                         seen_version_ids);

    std::vector<state::UpdatePlanItem> repair_updates;
    repair_updates.reserve(repair_versions.size());
    for (const auto& [project_id, version] : repair_versions) {
        repair_updates.push_back(buildRepairPlanItem(computation, project_id, version));
    }

    std::sort(repair_updates.begin(), repair_updates.end(),
              [](const state::UpdatePlanItem& left, const state::UpdatePlanItem& right) {
                  return left.source_path < right.source_path;
              });

    if (repair_updates.empty()) {
        throw cli::CliError("The selected mod does not have any repairable files.");
    }

    computation.state.updates = std::move(repair_updates);
    computation.state.updatable_projects.clear();
    for (const auto& [project_id, version] : repair_versions) {
        (void)version;
        computation.state.updatable_projects.push_back(project_id);
    }
    std::sort(computation.state.updatable_projects.begin(),
              computation.state.updatable_projects.end());
    state::UpdateStateSerializer::save(resolved_options.state_file, computation.state);

    std::cout << "Prepared fix-broken plan for --fix-broken " << resolved_options.fix_broken_name
              << " (" << computation.state.updates.size()
              << " package entries including required dependency versions).\n";

    return runUpgradeCommand(resolved_options);
}

int runInstallCommand(const cli::CommandLineOptions& options) {
    const cli::CommandLineOptions resolved_options = resolveCommandOptions(options, true);
    if (resolved_options.fix_broken) {
        return runFixBrokenCommand(resolved_options);
    }

    // Try to load cached state file first instead of re-querying APIs
    engine::UpdateComputation computation;
    bool loaded_from_cache = false;
    if (!resolved_options.only_upgrade_name.empty() &&
        std::filesystem::exists(resolved_options.state_file)) {
        try {
            state::UpdateState cached_state =
                state::UpdateStateSerializer::load(resolved_options.state_file);
            // Only use cache if game_version and loader match
            if (cached_state.game_version == resolved_options.game_version &&
                cached_state.loader == resolved_options.loader) {
                computation.state = cached_state;
                loaded_from_cache = true;
                std::cout << "Hit:1 local index [from cached state]\n";
                std::cout.flush();
            }
        } catch (const std::exception&) {
            // If cache loading fails, fall through to compute fresh state
        }
    }

    if (!loaded_from_cache) {
        computation =
            engine::UpdateEngine::computeUpdateState(resolved_options, resolved_options.state_file);
    }

    std::unordered_set<std::string> exact_match_projects;
    std::unordered_set<std::string> partial_match_projects;
    std::unordered_map<std::string, std::string> project_names;
    std::vector<std::string> unresolved_dependency_requirements;

    for (const state::UpdatePlanItem& update : computation.state.updates) {
        project_names.emplace(update.project_id,
                              std::filesystem::path(update.source_path).filename().string());

        if (matchesOnlyUpgradeName(resolved_options.only_upgrade_name, update)) {
            exact_match_projects.insert(update.project_id);
            continue;
        }

        if (partiallyMatchesOnlyUpgradeName(resolved_options.only_upgrade_name, update)) {
            partial_match_projects.insert(update.project_id);
        }
    }

    std::unordered_set<std::string> selected_projects =
        exact_match_projects.empty() ? partial_match_projects : exact_match_projects;

    if (selected_projects.empty()) {
        throw cli::CliError("No upgradable mod matched '--only-upgrade " +
                            resolved_options.only_upgrade_name +
                            "'. Run 'modrinth-cli list --upgradable " +
                            resolved_options.mods_path.string() + "' to see valid names.");
    }

    if (selected_projects.size() > 1) {
        std::vector<std::string> matched_names;
        matched_names.reserve(selected_projects.size());
        for (const std::string& project_id : selected_projects) {
            const auto name_it = project_names.find(project_id);
            matched_names.push_back(name_it != project_names.end() ? name_it->second : project_id);
        }
        std::sort(matched_names.begin(), matched_names.end());

        std::ostringstream message;
        message << "Multiple upgradable mods matched '--only-upgrade "
                << resolved_options.only_upgrade_name
                << "'. Please rerun with a more specific/full name. Matches:";
        for (const std::string& name : matched_names) {
            message << "\n  - " << name;
        }

        throw cli::CliError(message.str());
    }

    std::vector<std::string> queue(selected_projects.begin(), selected_projects.end());

    // Only resolve dependencies if we computed fresh state (not loading from cache).
    // When loading from cache, dependencies were already validated during the update command.
    if (!loaded_from_cache) {
        for (size_t index = 0; index < queue.size(); ++index) {
            const std::string& project_id = queue[index];
            const auto version_it = computation.target_versions_by_project.find(project_id);
            if (version_it == computation.target_versions_by_project.end()) {
                continue;
            }

            for (const ModDependency& dependency : version_it->second.getDependencies()) {
                if (!dependency.getProjectID() || !dependency.isRequired()) {
                    continue;
                }

                const std::string dependency_project_id = *dependency.getProjectID();
                const auto installed_version_it =
                    computation.installed_version_ids_by_project.find(dependency_project_id);
                if (installed_version_it != computation.installed_version_ids_by_project.end()) {
                    if (!dependency.getVersionID() ||
                        installed_version_it->second == *dependency.getVersionID()) {
                        continue;
                    }
                }

                const auto target_version_it =
                    computation.target_versions_by_project.find(dependency_project_id);
                if (dependency.getVersionID()) {
                    if (target_version_it != computation.target_versions_by_project.end() &&
                        target_version_it->second.getId() == *dependency.getVersionID()) {
                        if (selected_projects.insert(dependency_project_id).second) {
                            queue.push_back(dependency_project_id);
                        }
                        continue;
                    }

                    unresolved_dependency_requirements.push_back(dependency_project_id +
                                                                 " (requires version " +
                                                                 *dependency.getVersionID() + ")");
                    continue;
                }

                if (computation.updatable_project_ids.contains(dependency_project_id) &&
                    selected_projects.insert(dependency_project_id).second) {
                    queue.push_back(dependency_project_id);
                }
            }
        }

        if (!unresolved_dependency_requirements.empty()) {
            std::sort(unresolved_dependency_requirements.begin(),
                      unresolved_dependency_requirements.end());
            unresolved_dependency_requirements.erase(
                std::unique(unresolved_dependency_requirements.begin(),
                            unresolved_dependency_requirements.end()),
                unresolved_dependency_requirements.end());

            std::ostringstream message;
            message
                << "The selected mod requires an exact dependency version that is not available in "
                   "the current update plan:";
            for (const std::string& requirement : unresolved_dependency_requirements) {
                message << "\n  - " << requirement;
            }
            throw cli::CliError(message.str());
        }
    }

    std::vector<state::UpdatePlanItem> filtered_updates;
    filtered_updates.reserve(computation.state.updates.size());
    for (const state::UpdatePlanItem& update : computation.state.updates) {
        if (selected_projects.contains(update.project_id)) {
            filtered_updates.push_back(update);
        }
    }

    if (filtered_updates.empty()) {
        throw cli::CliError("Selected mod has no applicable upgrades in the current plan.");
    }

    const std::vector<state::UpdatePlanItem> full_updates = computation.state.updates;

    state::UpdateState scoped_state = computation.state;
    scoped_state.updates = std::move(filtered_updates);
    scoped_state.updatable_projects.assign(selected_projects.begin(), selected_projects.end());
    std::sort(scoped_state.updatable_projects.begin(), scoped_state.updatable_projects.end());

    cli::CommandLineOptions scoped_options = resolved_options;
    scoped_options.state_file = resolved_options.mods_path / kScopedInstallStateFilename;

    state::UpdateStateSerializer::save(scoped_options.state_file, scoped_state);

    std::cout << "Prepared upgrade plan for --only-upgrade " << resolved_options.only_upgrade_name
              << " (" << scoped_state.updates.size()
              << " package entries including required upgradable dependencies).\n";

    try {
        const int upgrade_exit_code = runUpgradeCommand(scoped_options);

        if (!resolved_options.dry_run) {
            const state::UpdateState scoped_after_upgrade =
                state::UpdateStateSerializer::load(scoped_options.state_file);

            state::UpdateState merged_state = computation.state;
            merged_state.generated_at = nowUtcIso8601();
            merged_state.updates = mergeUnselectedAndScopedRemainingUpdates(
                full_updates, selected_projects, scoped_after_upgrade.updates);
            merged_state.updatable_projects =
                collectUpdatableProjectsFromUpdates(merged_state.updates);

            state::UpdateStateSerializer::save(resolved_options.state_file, merged_state);
        }

        std::error_code cleanup_error;
        std::filesystem::remove(scoped_options.state_file, cleanup_error);

        return upgrade_exit_code;
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove(scoped_options.state_file, cleanup_error);
        throw;
    }
}

int runUpgradeCommand(const cli::CommandLineOptions& options) {
    const cli::CommandLineOptions resolved_options = resolveCommandOptions(options, true);
    state::UpdateState state = state::UpdateStateSerializer::load(resolved_options.state_file);

    if (!state.mods_path.empty()) {
        const auto current_mods_path =
            std::filesystem::absolute(resolved_options.mods_path).lexically_normal();
        const auto state_mods_path = std::filesystem::absolute(state.mods_path).lexically_normal();
        if (current_mods_path != state_mods_path) {
            std::cerr << "Warning: state file was generated for a different mods path:\n"
                      << "  state: " << state_mods_path << '\n'
                      << "  current: " << current_mods_path << '\n';
        }
    }

    if (state.updates.empty()) {
        std::cout << "No pending updates in state file: " << resolved_options.state_file << '\n';
        return 0;
    }

    if (!resolved_options.dry_run) {
        std::cout << "Reading package lists... Done\n";
        std::cout << "Building dependency tree... Done\n";
        std::cout << "Reading state information... Done\n";
        std::cout << "Calculating upgrade... Done\n";
        printUpgradePreview(state.updates);
        printUpgradeArchiveSummary(state.updates);

        if (!promptForYesNo("Do you want to continue?", true)) {
            std::cout << "Abort.\n";
            return 0;
        }
    }

    std::vector<state::UpdatePlanItem> remaining_updates;
    int applied_count = 0;
    int skipped_count = 0;
    int failed_count = 0;

    for (const state::UpdatePlanItem& update : state.updates) {
        const std::filesystem::path source_path(update.source_path);

        if (!std::filesystem::exists(source_path)) {
            std::cerr << "Failed: source file not found: " << source_path << '\n';
            remaining_updates.push_back(update);
            ++failed_count;
            continue;
        }

        std::string current_hash;
        try {
            current_hash = files::FileHasher::getSha512(source_path);
        } catch (const std::exception& ex) {
            std::cerr << "Failed: could not hash " << source_path << ": " << ex.what() << '\n';
            remaining_updates.push_back(update);
            ++failed_count;
            continue;
        }

        if (current_hash != update.current_hash) {
            std::cerr << "Skipped: local file changed since update plan was created: "
                      << source_path << '\n';
            remaining_updates.push_back(update);
            ++skipped_count;
            continue;
        }

        if (resolved_options.dry_run) {
            std::cout << "Dry run: would replace " << source_path.filename().string() << " with "
                      << update.download_filename << '\n';
            remaining_updates.push_back(update);
            ++skipped_count;
            continue;
        }

        const std::filesystem::path temp_path = source_path.string() + ".modrinth-cli.tmp";
        const std::filesystem::path backup_path = source_path.string() + ".modrinth-cli.bak";
        std::error_code file_error;

        std::filesystem::remove(temp_path, file_error);
        file_error.clear();

        std::string download_error;
        if (!ModrinthAPI::downloadFile(update.download_url, temp_path, &download_error)) {
            std::cerr << "Failed: download error for " << source_path << ": " << download_error
                      << '\n';
            remaining_updates.push_back(update);
            ++failed_count;
            continue;
        }

        if (!update.download_sha512.empty()) {
            try {
                const std::string downloaded_hash = files::FileHasher::getSha512(temp_path);
                if (downloaded_hash != update.download_sha512) {
                    std::cerr << "Failed: downloaded hash mismatch for " << source_path << '\n';
                    std::filesystem::remove(temp_path, file_error);
                    remaining_updates.push_back(update);
                    ++failed_count;
                    continue;
                }
            } catch (const std::exception& ex) {
                std::cerr << "Failed: could not verify downloaded file for " << source_path << ": "
                          << ex.what() << '\n';
                std::filesystem::remove(temp_path, file_error);
                remaining_updates.push_back(update);
                ++failed_count;
                continue;
            }
        }

        const std::filesystem::path mods_dir = source_path.parent_path();
        const std::filesystem::path download_filename(update.download_filename);
        if (!isSafeFilename(download_filename)) {
            std::cerr << "Failed: unsafe download filename: " << update.download_filename << '\n';
            std::filesystem::remove(temp_path, file_error);
            remaining_updates.push_back(update);
            ++failed_count;
            continue;
        }

        // Construct target path with new filename
        const std::filesystem::path target_path = mods_dir / download_filename;
        if (!isPathWithin(mods_dir, target_path)) {
            std::cerr << "Failed: download filename escapes mods directory: "
                      << update.download_filename << '\n';
            std::filesystem::remove(temp_path, file_error);
            remaining_updates.push_back(update);
            ++failed_count;
            continue;
        }

        std::filesystem::remove(backup_path, file_error);
        file_error.clear();
        std::filesystem::rename(source_path, backup_path, file_error);
        if (file_error) {
            std::cerr << "Failed: could not create backup for " << source_path << " ("
                      << file_error.message() << ")\n";
            std::filesystem::remove(temp_path, file_error);
            remaining_updates.push_back(update);
            ++failed_count;
            continue;
        }

        // Remove target if it already exists (in case of version downgrade or replacement)
        file_error.clear();
        std::filesystem::remove(target_path, file_error);

        file_error.clear();
        std::filesystem::rename(temp_path, target_path, file_error);
        if (file_error) {
            std::cerr << "Failed: could not move " << temp_path << " to " << target_path << " ("
                      << file_error.message() << ")\n";

            std::error_code restore_error;
            std::filesystem::rename(backup_path, source_path, restore_error);
            if (restore_error) {
                std::cerr << "Warning: failed to restore original file for " << source_path << " ("
                          << restore_error.message() << ")\n";
            }

            std::filesystem::remove(temp_path, file_error);
            remaining_updates.push_back(update);
            ++failed_count;
            continue;
        }

        std::filesystem::remove(backup_path, file_error);
        ++applied_count;
        std::cout << "Updated: " << source_path.filename().string() << " -> "
                  << update.download_filename << '\n';
    }

    if (!resolved_options.dry_run) {
        state.generated_at = nowUtcIso8601();
        state.updates = std::move(remaining_updates);
        state::UpdateStateSerializer::save(resolved_options.state_file, state);
    }

    std::cout << "\nUpgrade summary:\n";
    std::cout << "  applied: " << applied_count << '\n';
    std::cout << "  skipped: " << skipped_count << '\n';
    std::cout << "  failed: " << failed_count << '\n';

    if (!resolved_options.dry_run) {
        std::cout << "  remaining in plan: " << state.updates.size() << '\n';
        std::cout << "  state file: " << resolved_options.state_file << '\n';
    }

    return failed_count == 0 ? 0 : 1;
}
}  // namespace

int CommandDispatcher::dispatch(const cli::CommandLineOptions& options) {
    if (options.command == "init") {
        return runInitCommand(options);
    }

    if (options.command == "update") {
        return runUpdateCommand(options);
    }

    if (options.command == "upgrade") {
        return runUpgradeCommand(options);
    }

    if (options.command == "install") {
        return runInstallCommand(options);
    }

    if (options.command == "deps") {
        return runDepsCommand(options);
    }

    if (options.command == "list") {
        return runListCommand(options);
    }

    throw cli::CliError("Unknown command: " + options.command);
}

}  // namespace modrinth_cli::commands
