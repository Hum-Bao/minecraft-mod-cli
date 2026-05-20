#include "OutputFormatter.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ModDependency.h"
#include "ModVersionInfo.h"

namespace modrinth_cli::output {
namespace {
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

std::string resolveVersionLabel(const std::optional<std::string>& version_number,
                                const std::string& fallback_id) {
    return version_number ? *version_number : fallback_id;
}

std::string resolveProjectLabel(
    const engine::UpdateComputation& computation, const std::string& project_id,
    const std::optional<std::string>& dependency_file_name = std::nullopt) {
    const auto local_file_it = computation.project_local_filenames.find(project_id);
    if (local_file_it != computation.project_local_filenames.end()) {
        return local_file_it->second;
    }

    if (dependency_file_name && !dependency_file_name->empty()) {
        return *dependency_file_name;
    }

    const auto project_title_it = computation.project_titles.find(project_id);
    if (project_title_it != computation.project_titles.end()) {
        return project_title_it->second;
    }

    return project_id;
}

std::string resolveSourceLabel(const state::UpdatePlanItem& update) {
    if (!update.source.empty()) {
        return update.source;
    }

    return update.project_id.rfind("curseforge-", 0) == 0 ? "curseforge" : "modrinth";
}

std::string formatDownloadSizeSuffix(const std::optional<int64_t>& size) {
    if (!size) {
        return "";
    }

    return " [" + formatBytes(*size) + "]";
}

std::string formatDependencyRequirementSuffix(const state::DependencyIssue& issue) {
    if (!issue.required_version_id || issue.required_version_id->empty()) {
        return "";
    }

    return " (requires version " + *issue.required_version_id + ")";
}

std::string formatDependencyRequirementSuffix(const ModDependency& dependency) {
    if (!dependency.getVersionID() || dependency.getVersionID()->empty()) {
        return "";
    }

    return " (requires version " + *dependency.getVersionID() + ")";
}
}  // namespace

void OutputFormatter::printAptStyleUpdateProgress(const engine::UpdateComputation& computation,
                                                  std::chrono::steady_clock::duration elapsed) {
    const size_t total_fetched_bytes =
        computation.current_response_bytes + computation.latest_response_bytes;

    std::cout << "Hit:1 local index [" << computation.scanned_file_count << " files]\n";
    std::cout << "Get:2 https://api.modrinth.com [" << formatBytes(total_fetched_bytes) << "]\n";

    const auto elapsed_millis = std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    const size_t bytes_per_second =
        (total_fetched_bytes * 1000ULL) / static_cast<size_t>(elapsed_millis);
    const auto whole_seconds =
        std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());

    std::cout << "Fetched " << formatBytes(total_fetched_bytes) << " in " << whole_seconds << "s ("
              << formatBytes(bytes_per_second) << "/s)\n";
    std::cout << "Reading mod lists... Done\n";
    std::cout << "Building dependency tree... Done\n";
    std::cout << "Reading state information... Done\n";
}

void OutputFormatter::printDependencyTree(const engine::UpdateComputation& computation) {
    std::set<std::pair<std::string, std::string>> missing_required_pairs;
    for (const state::DependencyIssue& issue : computation.state.missing_required_dependencies) {
        missing_required_pairs.insert({issue.requiring_project_id, issue.missing_project_id});
    }

    std::vector<std::string> project_ids;
    project_ids.reserve(computation.target_versions_by_project.size());
    for (const auto& [project_id, version] : computation.target_versions_by_project) {
        (void)version;
        project_ids.push_back(project_id);
    }
    std::sort(project_ids.begin(), project_ids.end());

    std::cout << "\nDependency tree (required dependencies):\n";
    if (project_ids.empty()) {
        std::cout << "- No Modrinth projects were resolved from local files.\n";
        return;
    }

    for (const std::string& project_id : project_ids) {
        const ModVersionInfo& version = computation.target_versions_by_project.at(project_id);
        const std::string version_label =
            version.getVersionNumber() ? *version.getVersionNumber() : version.getId();
        const std::string project_label = resolveProjectLabel(computation, project_id);

        std::cout << "- " << project_label << " (" << version_label << ")\n";

        std::vector<std::string> dependency_lines;
        for (const ModDependency& dependency : version.getDependencies()) {
            if (!dependency.getProjectID()) {
                continue;
            }

            if (!dependency.isRequired()) {
                continue;
            }

            const std::string dependency_project_id = *dependency.getProjectID();
            const bool installed =
                computation.installed_project_ids.contains(dependency_project_id);
            const bool updatable =
                computation.updatable_project_ids.contains(dependency_project_id);
            const bool is_missing_for_target =
                missing_required_pairs.contains({project_id, dependency_project_id});
            const std::string dependency_label =
                resolveProjectLabel(computation, dependency_project_id, dependency.getFileName());
            const std::string requirement_suffix = formatDependencyRequirementSuffix(dependency);

            std::string line = "    -> " + dependency_label;
            if (installed && dependency.getVersionID()) {
                const auto installed_version_it =
                    computation.installed_version_ids_by_project.find(dependency_project_id);
                if (installed_version_it != computation.installed_version_ids_by_project.end() &&
                    installed_version_it->second == *dependency.getVersionID()) {
                    line += " [installed]";
                } else {
                    line += " [version mismatch]";
                }
            } else if (installed) {
                line += " [installed]";
            } else if (is_missing_for_target) {
                line += " [missing]";
            } else {
                line += " [not applicable for selected loader/version]";
            }
            if (updatable) {
                line += " [update available]";
            }
            line += requirement_suffix;

            dependency_lines.push_back(std::move(line));
        }

        if (dependency_lines.empty()) {
            std::cout << "    -> (no required dependencies)\n";
            continue;
        }

        std::sort(dependency_lines.begin(), dependency_lines.end());
        for (const std::string& line : dependency_lines) {
            std::cout << line << '\n';
        }
    }
}

void OutputFormatter::printMissingDependencies(const engine::UpdateComputation& computation) {
    const state::UpdateState& state = computation.state;
    if (state.missing_required_dependencies.empty()) {
        std::cout << "\nAll required dependencies appear to be installed.\n";
        return;
    }

    std::cout << "\nMissing required dependencies: " << state.missing_required_dependencies.size()
              << '\n';
    for (const state::DependencyIssue& issue : state.missing_required_dependencies) {
        std::cout << "- " << resolveProjectLabel(computation, issue.requiring_project_id)
                  << " requires " << resolveProjectLabel(computation, issue.missing_project_id)
                  << formatDependencyRequirementSuffix(issue) << "\n";
    }
}

void OutputFormatter::printUpgradableList(const std::vector<state::UpdatePlanItem>& updates) {
    std::vector<state::UpdatePlanItem> sorted_updates = updates;
    std::sort(sorted_updates.begin(), sorted_updates.end(),
              [](const state::UpdatePlanItem& left, const state::UpdatePlanItem& right) {
                  return left.source_path < right.source_path;
              });

    std::cout << "Listing... Done\n";
    for (const state::UpdatePlanItem& update : sorted_updates) {
        const std::string current_label =
            resolveVersionLabel(update.current_version_number, update.current_version_id);
        const std::string target_label =
            resolveVersionLabel(update.target_version_number, update.target_version_id);
        const std::string package_name =
            std::filesystem::path(update.source_path).filename().string();
        const std::string source_label = resolveSourceLabel(update);

        std::cout << package_name << '/' << source_label << ' ' << target_label
                  << " upgradable from: " << current_label
                  << formatDownloadSizeSuffix(update.download_size) << '\n';
    }
}

void OutputFormatter::printUpdatableProjects(const engine::UpdateComputation& computation) {
    if (computation.state.updatable_projects.empty()) {
        return;
    }

    std::cout << "\nProjects with updates available under current filters: "
              << computation.state.updatable_projects.size() << '\n';
    for (const std::string& project_id : computation.state.updatable_projects) {
        std::cout << "- " << resolveProjectLabel(computation, project_id) << '\n';
    }
}

}  // namespace modrinth_cli::output
