#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CliParser.h"
#include "FileScanner.h"
#include "UpdateState.h"

class ModVersionInfo;

namespace modrinth_cli::engine {

struct UpdateComputation {
        state::UpdateState state;
        std::unordered_map<std::string, ModVersionInfo> target_versions_by_project;
        std::unordered_map<std::string, ModVersionInfo> installed_versions_by_project;
        std::unordered_map<std::string, std::string> project_local_filenames;
        std::unordered_map<std::string, std::string> installed_source_paths_by_project;
        std::unordered_map<std::string, std::string> installed_hashes_by_project;
        std::unordered_map<std::string, std::string> project_titles;
        std::unordered_set<std::string> installed_project_ids;
        std::unordered_map<std::string, std::string> installed_version_ids_by_project;
        std::unordered_set<std::string> updatable_project_ids;
        std::vector<files::FileHashRecord> scanned_files;
        std::unordered_set<std::string> resolved_current_hashes;
        std::unordered_set<std::string> resolved_latest_hashes;
        size_t scanned_file_count = 0;
        size_t current_version_count = 0;
        size_t latest_version_count = 0;
        size_t current_response_bytes = 0;
        size_t latest_response_bytes = 0;
        size_t curseforge_response_bytes = 0;
        size_t curseforge_unresolved_count = 0;
        bool curseforge_attempted = false;
        std::vector<std::string> unresolved_mod_filenames;
};

class UpdateEngine {
    public:
        static UpdateComputation computeUpdateState(const cli::CommandLineOptions& options,
                                                    const std::filesystem::path& state_file);
};

}  // namespace modrinth_cli::engine
