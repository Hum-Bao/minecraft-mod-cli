#include "ModVersionInfo.h"

ModVersionInfo::ModVersionInfo(
    std::optional<std::string> name, std::optional<std::string> version_number,
    std::optional<std::string> changelog, std::vector<ModDependency> dependencies,
    std::vector<std::string> game_versions, std::optional<std::string> version_type,
    std::vector<std::string> loaders, std::optional<bool> featured,
    std::optional<std::string> status, std::optional<std::string> requested_status, std::string id,
    std::string project_id, std::string author_id, std::string date_published, int64_t downloads,
    std::optional<std::string> changelog_url, std::vector<FileInfo> files)
    : name_(std::move(name)),
      version_number_(std::move(version_number)),
      changelog_(std::move(changelog)),
      dependencies_(std::move(dependencies)),
      game_versions_(std::move(game_versions)),
      version_type_(std::move(version_type)),
      loaders_(std::move(loaders)),
      featured_(featured),
      status_(std::move(status)),
      requested_status_(std::move(requested_status)),
      id_(std::move(id)),
      project_id_(std::move(project_id)),
      author_id_(std::move(author_id)),
      date_published_(std::move(date_published)),
      downloads_(downloads),
      changelog_url_(std::move(changelog_url)),
      files_(std::move(files)) {}