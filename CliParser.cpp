#include "CliParser.h"

#include <iostream>

namespace modrinth_cli::cli {
namespace {
constexpr char kDefaultStateFilename[] = ".modrinth-cli-update.json";
}  // namespace

void CliParser::printUsage(const char* executable_name) {
    std::cout
        << "Usage:\n"
        << "  " << executable_name
        << " update [mods-path] [--game-version <version>] [--loader <loader>]"
           " [--state <file>]\n"
        << "  " << executable_name
        << " install --only-upgrade <name> [mods-path] [--state <file>]"
           " [--game-version <version>] [--loader <loader>] [--dry-run]\n"
        << "  " << executable_name
        << " install --fix-broken <name> [mods-path] [--state <file>]"
           " [--game-version <version>] [--loader <loader>] [--dry-run]\n"
        << "  " << executable_name << " init <mods-path>\n"
        << "  " << executable_name << " upgrade [mods-path] [--state <file>] [--dry-run]\n"
        << "  " << executable_name
        << " deps [mods-path] [--game-version <version>] [--loader <loader>]\n"
        << "  " << executable_name
        << " list [--upgradable] [--disabled] [--unknown] [--client-only] [--server-only]"
           " [--client-server] [--side-unknown] [mods-path] [--state <file>]\n"
        << "  " << executable_name << " config [set-api-key <key> | show-api-key]\n\n"
        << "Run 'init' once per mod folder to save default game version and loader.\n"
        << "Update/deps will use saved values unless --game-version/--loader are provided.\n\n"
        << "If no mods-path is provided, the CLI checks the ./mods folder in the current "
           "working directory.\n";
}

CommandLineOptions CliParser::parseArguments(int argc, char** argv) {
    if (argc < 2) {
        throw CliError("Missing command and mods path.");
    }

    CommandLineOptions options;
    options.command = "update";
    options.mods_path = std::filesystem::absolute("mods");
    options.mods_path_provided = false;
    options.game_version.clear();
    options.loader.clear();

    bool state_file_set = false;

    int index = 1;
    const std::string first_arg = argv[index];
    if (first_arg == "update" || first_arg == "upgrade" || first_arg == "deps" ||
        first_arg == "list" || first_arg == "init" || first_arg == "install" ||
        first_arg == "config") {
        options.command = first_arg;
        ++index;
    } else {
        throw CliError("Unknown command: " + first_arg);
    }

    // For config command, collect all remaining args
    if (options.command == "config") {
        while (index < argc) {
            options.args.push_back(argv[index]);
            ++index;
        }
        return options;
    }

    while (index < argc) {
        const std::string arg = argv[index];

        if (arg == "--state") {
            if (index + 1 >= argc) {
                throw CliError("Expected a file path after --state.");
            }
            options.state_file = std::filesystem::absolute(argv[index + 1]);
            state_file_set = true;
            index += 2;
            continue;
        }

        if (arg == "--game-version") {
            if (index + 1 >= argc) {
                throw CliError("Expected a value after --game-version.");
            }
            options.game_version = argv[index + 1];
            options.game_version_provided = true;
            index += 2;
            continue;
        }

        if (arg == "--loader") {
            if (index + 1 >= argc) {
                throw CliError("Expected a value after --loader.");
            }
            options.loader = argv[index + 1];
            options.loader_provided = true;
            index += 2;
            continue;
        }

        if (arg == "--dry-run") {
            options.dry_run = true;
            ++index;
            continue;
        }

        if (arg == "--only-upgrade") {
            if (index + 1 >= argc) {
                throw CliError("Expected a mod name after --only-upgrade.");
            }
            options.only_upgrade_name = argv[index + 1];
            index += 2;
            continue;
        }

        if (arg == "--fix-broken" || arg == "-f") {
            if (index + 1 >= argc) {
                throw CliError("Expected a mod name after --fix-broken.");
            }
            options.fix_broken = true;
            options.fix_broken_name = argv[index + 1];
            index += 2;
            continue;
        }

        if (arg == "--upgradable") {
            options.list_upgradable = true;
            ++index;
            continue;
        }

        if (arg == "--disabled") {
            options.list_disabled = true;
            ++index;
            continue;
        }

        if (arg == "--unknown") {
            options.list_unknown = true;
            ++index;
            continue;
        }

        if (arg == "--client-only") {
            options.list_client_only = true;
            ++index;
            continue;
        }

        if (arg == "--server-only") {
            options.list_server_only = true;
            ++index;
            continue;
        }

        if (arg == "--client-server") {
            options.list_client_server = true;
            ++index;
            continue;
        }

        if (arg == "--side-unknown") {
            options.list_side_unknown = true;
            ++index;
            continue;
        }

        if (arg.starts_with("--")) {
            throw CliError("Unknown argument: " + arg);
        }

        if (options.mods_path_provided) {
            throw CliError("Unexpected positional argument: " + arg);
        }

        options.mods_path = std::filesystem::absolute(arg);
        options.mods_path_provided = true;
        ++index;
    }

    if (!state_file_set) {
        options.state_file = options.mods_path / kDefaultStateFilename;
    }

    if (options.command == "list") {
        const bool has_side_filter = options.list_client_only || options.list_server_only ||
                                     options.list_client_server || options.list_side_unknown;

        if (!options.list_upgradable && !options.list_disabled && !options.list_unknown &&
            has_side_filter) {
            options.list_upgradable = true;
        }

        if (!options.list_upgradable && !options.list_disabled && !options.list_unknown) {
            throw CliError(
                "The list command requires at least one of: --upgradable, --disabled, --unknown.");
        }

        if (has_side_filter && (options.list_disabled || options.list_unknown)) {
            throw CliError(
                "--client-only, --server-only, --client-server, and --side-unknown can only "
                "be used with upgradable Modrinth entries.");
        }
    } else {
        if (options.list_upgradable || options.list_disabled || options.list_unknown ||
            options.list_client_only || options.list_server_only || options.list_client_server ||
            options.list_side_unknown) {
            throw CliError(
                "--upgradable, --disabled, --unknown, --client-only, --server-only, "
                "--client-server, and --side-unknown are only supported with the list "
                "command.");
        }
    }

    if (options.command != "upgrade" && options.command != "install" && options.dry_run) {
        throw CliError("--dry-run is only supported with the upgrade command.");
    }

    if (options.command == "init" && (options.game_version_provided || options.loader_provided)) {
        throw CliError(
            "The init command is interactive and does not accept --game-version or --loader.");
    }

    if (options.command == "install") {
        const bool has_only_upgrade = !options.only_upgrade_name.empty();
        const bool has_fix_broken = options.fix_broken;

        if (has_only_upgrade == has_fix_broken) {
            throw CliError(
                "install requires exactly one of --only-upgrade <name> or --fix-broken <name>.");
        }
    } else if (!options.only_upgrade_name.empty() || options.fix_broken) {
        throw CliError(
            "--only-upgrade and --fix-broken are only supported with the install command.");
    }

    return options;
}

}  // namespace modrinth_cli::cli
