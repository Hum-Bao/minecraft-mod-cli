#pragma once

#include <chrono>
#include <vector>

#include "UpdateEngine.h"
#include "UpdateState.h"

namespace modrinth_cli::output {

class OutputFormatter {
    public:
        static void printAptStyleUpdateProgress(const engine::UpdateComputation& computation,
                                                std::chrono::steady_clock::duration elapsed);
        static void printDependencyTree(const engine::UpdateComputation& computation);
        static void printMissingDependencies(const engine::UpdateComputation& computation);
        static void printUpgradableList(const std::vector<state::UpdatePlanItem>& updates);
        static void printUpdatableProjects(const engine::UpdateComputation& computation);
};

}  // namespace modrinth_cli::output
