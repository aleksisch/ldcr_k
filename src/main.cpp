// LLVM/SVF driver for constructing and inspecting a call graph.

#include "ldc/SVFFrontend.h"
#include "CommandLine.h"

#include <fstream>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

int main(int argc, char** argv) {
    const auto parsed = ldc::parseCommandLine(argc, argv);
    if (const auto* exitCode = std::get_if<int>(&parsed)) return *exitCode;
    const auto& options = std::get<ldc::CommandLine>(parsed);

    const auto result = ldc::frontend::analyzeModules(options.modules);
    result.printSummary(std::cout);
    bool ok = true;
    if (!options.programDot.empty()) {
        std::ofstream out(options.programDot);
        result.graph.dumpDot(out);
        out.close();
        if (!out) {
            std::cerr << "Cannot write program graph: " << options.programDot << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
