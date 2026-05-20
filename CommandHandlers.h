#pragma once

#include "CliParser.h"

namespace modrinth_cli::commands {

class CommandDispatcher {
    public:
        static int dispatch(const cli::CommandLineOptions& options);
};

}  // namespace modrinth_cli::commands
