#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace modrinth_cli::cli {

struct CommandLineOptions {
        std::string command;
        std::filesystem::path mods_path;
        std::filesystem::path state_file;
        std::string only_upgrade_name;
        std::string fix_broken_name;
        std::string game_version;
        std::string loader;
        std::vector<std::string> args;  // For generic subcommand arguments
        bool mods_path_provided = false;
        bool game_version_provided = false;
        bool loader_provided = false;
        bool list_upgradable = false;
        bool list_disabled = false;
        bool list_unknown = false;
        bool list_client_only = false;
        bool list_server_only = false;
        bool list_client_server = false;
        bool list_side_unknown = false;
        bool fix_broken = false;
        bool dry_run = false;
};

class CliError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
};

class CliParser {
    public:
        static CommandLineOptions parseArguments(int argc, char** argv);
        static void printUsage(const char* executable_name);
};

}  // namespace modrinth_cli::cli
