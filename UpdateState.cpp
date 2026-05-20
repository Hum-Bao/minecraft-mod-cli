#include "UpdateState.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace modrinth_cli::state {

std::optional<std::string> getOptionalString(const nlohmann::json& json_obj, const char* key) {
    if (!json_obj.contains(key) || json_obj.at(key).is_null()) {
        return std::nullopt;
    }
    return json_obj.at(key).get<std::string>();
}

nlohmann::json updatePlanItemToJson(const UpdatePlanItem& item) {
    nlohmann::json json_item;
    json_item["source_path"] = item.source_path;
    json_item["current_hash"] = item.current_hash;
    json_item["project_id"] = item.project_id;
    json_item["source"] = item.source;
    json_item["current_version_id"] = item.current_version_id;
    json_item["current_version_number"] = item.current_version_number
                                              ? nlohmann::json(*item.current_version_number)
                                              : nlohmann::json(nullptr);
    json_item["target_version_id"] = item.target_version_id;
    json_item["target_version_number"] = item.target_version_number
                                             ? nlohmann::json(*item.target_version_number)
                                             : nlohmann::json(nullptr);
    json_item["download_url"] = item.download_url;
    json_item["download_filename"] = item.download_filename;
    json_item["download_sha512"] = item.download_sha512;
    json_item["download_size"] =
        item.download_size ? nlohmann::json(*item.download_size) : nlohmann::json(nullptr);
    return json_item;
}

UpdatePlanItem updatePlanItemFromJson(const nlohmann::json& json_item) {
    UpdatePlanItem item;
    item.source_path = json_item.at("source_path").get<std::string>();
    item.current_hash = json_item.at("current_hash").get<std::string>();
    item.project_id = json_item.at("project_id").get<std::string>();
    if (json_item.contains("source") && json_item.at("source").is_string()) {
        item.source = json_item.at("source").get<std::string>();
    } else if (item.project_id.rfind("curseforge-", 0) == 0) {
        item.source = "curseforge";
    }
    item.current_version_id = json_item.at("current_version_id").get<std::string>();
    item.current_version_number = getOptionalString(json_item, "current_version_number");
    item.target_version_id = json_item.at("target_version_id").get<std::string>();
    item.target_version_number = getOptionalString(json_item, "target_version_number");
    item.download_url = json_item.at("download_url").get<std::string>();
    item.download_filename = json_item.at("download_filename").get<std::string>();
    item.download_sha512 = json_item.at("download_sha512").get<std::string>();
    if (json_item.contains("download_size") && !json_item.at("download_size").is_null()) {
        item.download_size = json_item.at("download_size").get<int64_t>();
    }
    return item;
}

nlohmann::json dependencyIssueToJson(const DependencyIssue& issue) {
    nlohmann::json json_issue;
    json_issue["requiring_project_id"] = issue.requiring_project_id;
    json_issue["missing_project_id"] = issue.missing_project_id;
    json_issue["dependency_type"] = issue.dependency_type;
    json_issue["required_version_id"] = issue.required_version_id
                                            ? nlohmann::json(*issue.required_version_id)
                                            : nlohmann::json(nullptr);
    return json_issue;
}

DependencyIssue dependencyIssueFromJson(const nlohmann::json& json_issue) {
    DependencyIssue issue;
    issue.requiring_project_id = json_issue.at("requiring_project_id").get<std::string>();
    issue.missing_project_id = json_issue.at("missing_project_id").get<std::string>();
    issue.dependency_type = json_issue.at("dependency_type").get<std::string>();
    if (json_issue.contains("required_version_id") &&
        !json_issue.at("required_version_id").is_null()) {
        issue.required_version_id = json_issue.at("required_version_id").get<std::string>();
    }
    return issue;
}

nlohmann::json updateStateToJson(const UpdateState& state) {
    nlohmann::json root;
    root["generated_at"] = state.generated_at;
    root["mods_path"] = state.mods_path;
    root["game_version"] = state.game_version;
    root["loader"] = state.loader;

    root["updates"] = nlohmann::json::array();
    for (const UpdatePlanItem& update : state.updates) {
        root["updates"].push_back(updatePlanItemToJson(update));
    }

    root["missing_required_dependencies"] = nlohmann::json::array();
    for (const DependencyIssue& issue : state.missing_required_dependencies) {
        root["missing_required_dependencies"].push_back(dependencyIssueToJson(issue));
    }

    root["updatable_projects"] = state.updatable_projects;
    return root;
}

UpdateState updateStateFromJson(const nlohmann::json& root) {
    UpdateState state;
    state.generated_at = root.at("generated_at").get<std::string>();
    state.mods_path = root.at("mods_path").get<std::string>();
    state.game_version = root.at("game_version").get<std::string>();
    state.loader = root.at("loader").get<std::string>();

    if (root.contains("updates") && root.at("updates").is_array()) {
        for (const auto& json_update : root.at("updates")) {
            state.updates.push_back(updatePlanItemFromJson(json_update));
        }
    }

    if (root.contains("missing_required_dependencies") &&
        root.at("missing_required_dependencies").is_array()) {
        for (const auto& json_issue : root.at("missing_required_dependencies")) {
            state.missing_required_dependencies.push_back(dependencyIssueFromJson(json_issue));
        }
    }

    if (root.contains("updatable_projects") && root.at("updatable_projects").is_array()) {
        state.updatable_projects = root.at("updatable_projects").get<std::vector<std::string>>();
    }

    return state;
}

void UpdateStateSerializer::save(const std::filesystem::path& state_file,
                                 const UpdateState& state) {
    if (!state_file.parent_path().empty()) {
        std::filesystem::create_directories(state_file.parent_path());
    }

    std::ofstream output(state_file, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Failed to open state file for write: " + state_file.string());
    }

    output << updateStateToJson(state).dump(2) << '\n';
    if (!output) {
        throw std::runtime_error("Failed to write state file: " + state_file.string());
    }
}

UpdateState UpdateStateSerializer::load(const std::filesystem::path& state_file) {
    std::ifstream input(state_file);
    if (!input) {
        throw std::runtime_error("Failed to open state file: " + state_file.string());
    }

    const nlohmann::json root = nlohmann::json::parse(input);
    return updateStateFromJson(root);
}

}  // namespace modrinth_cli::state
