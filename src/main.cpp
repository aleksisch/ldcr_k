// LLVM/SVF driver for constructing and inspecting a call graph.

#include "ldc/SVFFrontend.h"
#include "CommandLine.h"

#include <fstream>
#include <iostream>
#include <exception>
#include <string>
#include <vector>

int main(int argc, char** argv) try {
    const auto options = ldc::parseCommandLine(argc, argv);
    if (options.help) {
        ldc::printHelp(std::cout);
        return 0;
    }

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
} catch (const std::exception& error) {
    std::cerr << "ldc: " << error.what() << "\n";
    return 1;
}
