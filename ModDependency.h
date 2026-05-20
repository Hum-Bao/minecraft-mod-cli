#ifndef MODDEPENDENCY_H
#define MODDEPENDENCY_H

#include <optional>
#include <string>

class ModDependency {
    public:
        ModDependency(const std::optional<std::string>& version_id,
                      const std::optional<std::string>& project_id,
                      const std::optional<std::string>& file_name,
                      const std::optional<std::string>& dependency_type);

        [[nodiscard]] const std::optional<std::string>& getVersionID() const;

        [[nodiscard]] const std::optional<std::string>& getProjectID() const;

        [[nodiscard]] const std::optional<std::string>& getFileName() const;

        [[nodiscard]] const std::optional<std::string>& getDependencyType() const;

        [[nodiscard]] bool isRequired() const;

        void printDependency() const;

    private:
        std::optional<std::string> version_id_;
        std::optional<std::string> project_id_;
        std::optional<std::string> file_name_;
        std::optional<std::string> dependency_type_;
};
#endif