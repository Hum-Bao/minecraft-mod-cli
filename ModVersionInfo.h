#ifndef MODVERSIONINFO_H
#define MODVERSIONINFO_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "FileInfo.h"
#include "ModDependency.h"

class ModVersionInfo {
    public:
        ModVersionInfo(std::optional<std::string> name, std::optional<std::string> version_number,
                       std::optional<std::string> changelog,
                       std::vector<ModDependency> dependencies,
                       std::vector<std::string> game_versions,
                       std::optional<std::string> version_type, std::vector<std::string> loaders,
                       std::optional<bool> featured, std::optional<std::string> status,
                       std::optional<std::string> requested_status, std::string id,
                       std::string project_id, std::string author_id, std::string date_published,
                       int64_t downloads, std::optional<std::string> changelog_url,
                       std::vector<FileInfo> files);

        // Dependency Methods:
        //void addDependency(const ModDependency& dependency) { dependencies_.push_back(dependency); }

        [[nodiscard]] const std::optional<std::string>& getName() const { return name_; }

        [[nodiscard]] const std::optional<std::string>& getVersionNumber() const {
            return version_number_;
        }

        const std::optional<std::string>& getChangelog() const { return changelog_; }

        const std::vector<ModDependency>& getDependencies() const { return dependencies_; }

        const std::vector<std::string>& getGameVersions() const { return game_versions_; }

        const std::optional<std::string>& getVersionType() const { return version_type_; }

        const std::vector<std::string>& getLoaders() const { return loaders_; }

        const std::optional<bool>& isFeatured() const { return featured_; }

        const std::optional<std::string>& getStatus() const { return status_; }

        const std::optional<std::string>& getRequestedStatus() const { return requested_status_; }

        const std::string& getId() const { return id_; }

        const std::string& getProjectId() const { return project_id_; }

        const std::string& getAuthorId() const { return author_id_; }

        const std::string& getDatePublished() const { return date_published_; }

        int64_t getDownloads() const { return downloads_; }

        const std::optional<std::string>& getChangelogUrl() const { return changelog_url_; }

        const std::vector<FileInfo>& getFiles() const { return files_; }

        // TODO: Add - kbricks
        void printDependencies() {}

    private:
        // The name of this version
        std::optional<std::string> name_;
        // The version number. Ideally will follow semantic versioning
        std::optional<std::string> version_number_;
        // The changelog for this version (nullable)
        std::optional<std::string> changelog_;
        // A list of specific versions of projects that this version depends on
        std::vector<ModDependency> dependencies_;
        // A list of versions of Minecraft that this version supports
        std::vector<std::string> game_versions_;
        // The release channel for this version (Allowed values: release || beta || alpha)
        std::optional<std::string> version_type_;
        // The mod loaders that this version supports. In case of resource packs, use “minecraft”
        std::vector<std::string> loaders_;
        // Whether the version is featured or not
        std::optional<bool> featured_;
        // Allowed values: listed || archived || draft || unlisted || scheduled || unknown
        std::optional<std::string> status_;
        // Allowed values : listed || archived || draft || unlisted (nullable)
        std::optional<std::string> requested_status_;

        // REQUIRED FIELDS
        // The ID of the version, encoded as a base62 string
        std::string id_;
        // The ID of the project this version is for
        std::string project_id_;
        // The ID of the author who published this version
        std::string author_id_;
        // format: ISO-8601
        std::string date_published_;
        // The number of times this version has been downloaded
        int64_t downloads_ = 0;
        // A link to the changelog for this version. Always null, only kept for legacy compatibility.
        std::optional<std::string> changelog_url_;
        // A list of files available for download for this version
        std::vector<FileInfo> files_;
};
#endif