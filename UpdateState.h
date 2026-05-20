#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace modrinth_cli::state {

struct UpdatePlanItem {
        std::string source_path;
        std::string current_hash;
        std::string project_id;
        std::string source = "modrinth";
        std::string current_version_id;
        std::optional<std::string> current_version_number;
        std::string target_version_id;
        std::optional<std::string> target_version_number;
        std::string download_url;
        std::string download_filename;
        std::string download_sha512;
        std::optional<int64_t> download_size;
};

struct DependencyIssue {
        std::string requiring_project_id;
        std::string missing_project_id;
        std::string dependency_type;
        std::optional<std::string> required_version_id;
};

struct UpdateState {
        std::string generated_at;
        std::string mods_path;
        std::string game_version;
        std::string loader;
        std::vector<UpdatePlanItem> updates;
        std::vector<DependencyIssue> missing_required_dependencies;
        std::vector<std::string> updatable_projects;
        std::vector<std::string> unresolved_mod_filenames;
};

// Public JSON serialization for caching
nlohmann::json updateStateToJson(const UpdateState& state);
UpdateState updateStateFromJson(const nlohmann::json& root);

class UpdateStateSerializer {
    public:
        static void save(const std::filesystem::path& state_file, const UpdateState& state);
        static UpdateState load(const std::filesystem::path& state_file);
};

}  // namespace modrinth_cli::state
