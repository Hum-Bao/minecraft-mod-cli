#include "ModDependency.h"

#include <algorithm>
#include <cctype>
#include <iostream>

ModDependency::ModDependency(const std::optional<std::string>& version_id,
                             const std::optional<std::string>& project_id,
                             const std::optional<std::string>& file_name,
                             const std::optional<std::string>& dependency_type)
    : version_id_(version_id),
      project_id_(project_id),
      file_name_(file_name),
      dependency_type_(dependency_type) {}

const std::optional<std::string>& ModDependency::getVersionID() const {
    return version_id_;
}

const std::optional<std::string>& ModDependency::getProjectID() const {
    return project_id_;
}

const std::optional<std::string>& ModDependency::getFileName() const {
    return file_name_;
}

const std::optional<std::string>& ModDependency::getDependencyType() const {
    return dependency_type_;
}

bool ModDependency::isRequired() const {
    if (!dependency_type_ || dependency_type_->empty()) {
        return true;
    }

    std::string normalized = *dependency_type_;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "required";
}

void ModDependency::printDependency() const {
    if (getFileName()) {
        std::cout << "File name: " << *getFileName() << "\n";
    }
    if (getVersionID()) {
        std::cout << "Version ID: " << *getVersionID() << "\n";
    }
    if (getProjectID()) {
        std::cout << "Project ID: " << *getProjectID() << "\n";
    }
    if (getDependencyType()) {
        std::cout << "Dependency type: " << *getDependencyType() << "\n";
    }
}