#ifndef CURSEFORGE_API_H
#define CURSEFORGE_API_H

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ModVersionInfo.h"

class CurseForgeAPI {
    public:
        // Search for a mod by filename and return version info if found
        // Returns std::nullopt if not found or if API key is not configured
        static std::optional<ModVersionInfo> searchModByFilename(const std::string& filename,
                                                                 size_t* response_bytes = nullptr);

        // Get the API key from environment variable, or fallback to config file
        static std::optional<std::string> getApiKey();

        // Save the API key to ~/.modrinth-cli/api-key
        static bool saveApiKey(const std::string& api_key);

    private:
        // Internal helper to search mods and return project details
        static std::optional<ModVersionInfo> performModSearch(const std::string& mod_name,
                                                              const std::string& api_key,
                                                              size_t* response_bytes = nullptr);
};

#endif
