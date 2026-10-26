#pragma once

#include <string>
#include <variant>
#include <vector>

namespace ldc {

struct CommandLine {
    std::vector<std::string> modules;
    std::string programDot;
    bool pointsTo = false; // solve points-to facts
    std::string facts;     // write the points-to facts
    std::string source;    // C++ source whose `// <label>` comments name allocation sites
    // Intersect with L_C.
    bool matchCalls = false;
    // The C++ heap model: objects at offsets and heap cells.
    bool cppHeap = false;
    // SVF's CFL solver instead of LAGraph.
    bool svfSolver = false;
    // The PAG right before solving, as DOT, with this source file at the top right.
    std::string pagDot;
    std::string pagDotCode;
    // `from:to` queries, by the names of --facts, and the languages to find their paths in.
    std::vector<std::string> paths;
    std::string languages = "D";
};

// Parses the arguments, or prints help or an error and returns the exit code.
std::variant<CommandLine, int> parseCommandLine(int argc, char** argv);

} // namespace ldc
