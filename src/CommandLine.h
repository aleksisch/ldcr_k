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
};

// Parses the arguments, or prints help or an error and returns the exit code.
std::variant<CommandLine, int> parseCommandLine(int argc, char** argv);

} // namespace ldc
