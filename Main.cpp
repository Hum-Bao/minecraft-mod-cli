#include <iostream>
#include <stdexcept>

#include "CliParser.h"
#include "CommandHandlers.h"

int main(int argc, char** argv) {
    try {
        const modrinth_cli::cli::CommandLineOptions options =
            modrinth_cli::cli::CliParser::parseArguments(argc, argv);
        return modrinth_cli::commands::CommandDispatcher::dispatch(options);
    } catch (const modrinth_cli::cli::CliError& ex) {
        std::cerr << "Error: " << ex.what() << "\n\n";
        modrinth_cli::cli::CliParser::printUsage(argv[0]);
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}