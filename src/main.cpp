// LLVM/SVF driver for constructing and inspecting a call graph.

#include "ldc/SVFFrontend.h"
#include "Util/CommandLine.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace SVF;

namespace {
const Option<std::string> GraphOut("program-dot", "Write the common pointer-flow graph to DOT", "");
}

int main(int argc, char** argv) {
    std::vector<std::string> modules =
        OptionBase::parseOptions(argc, argv, "ldc: LLVM/SVF call graph", "[options] <input.ll>");
    if (modules.empty()) {
        std::cerr << "usage: ldc [options] <input.ll>\n";
        return 1;
    }

    const auto result = ldc::frontend::analyzeModules(modules);
    result.printSummary(std::cout);
    bool ok = true;
    if (!GraphOut().empty()) {
        std::ofstream out(GraphOut());
        result.graph.dumpDot(out);
        out.close();
        if (!out) {
            std::cerr << "Cannot write program graph: " << GraphOut() << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
